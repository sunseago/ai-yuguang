// DeepSeek Harness 适配器：解析 ~/.dsh/sessions/<项目>/session-*/session.v4.jsonl(.zstd)
// 日志是仅追加的事件流（turn/step/tool/job…），因此可以增量解析：
// 记住上次读到哪一帧，只解后续帧，避免每次全量解压。
import fs from 'node:fs';
import path from 'node:path';
import { decompressFrames } from '../lib/zstdframes.mjs';
import { expandHome, truncateChars } from '../lib/util.mjs';

const cache = {
  file: null,
  consumedBytes: 0,
  size: -1,
  mtime: -1,
  state: null,
  dirCache: { at: 0, file: null },
};

function latestSessionFile(dir) {
  const now = Date.now();
  if (now - cache.dirCache.at < 3000 && cache.dirCache.file) return cache.dirCache.file;
  let best = null;
  let bestM = -1;
  let projs = [];
  try {
    projs = fs.readdirSync(dir, { withFileTypes: true }).filter((d) => d.isDirectory());
  } catch {
    return null;
  }
  for (const p of projs) {
    const pdir = path.join(dir, p.name);
    let sessions = [];
    try {
      sessions = fs.readdirSync(pdir, { withFileTypes: true }).filter((d) => d.isDirectory());
    } catch {
      continue;
    }
    for (const s of sessions) {
      for (const name of ['session.v4.jsonl.zstd', 'session.v4.jsonl']) {
        const f = path.join(pdir, s.name, name);
        try {
          const st = fs.statSync(f);
          if (st.mtimeMs > bestM) {
            bestM = st.mtimeMs;
            best = f;
          }
        } catch {
          /* 不存在 */
        }
      }
    }
  }
  cache.dirCache = { at: now, file: best };
  return best;
}

function emptyState(file) {
  return {
    file,
    turn: null,
    step: null,
    title: '',
    tool: null,
    tools: [],
    jobs: new Set(),
    state: 'idle',
    stateSince: Date.now(),
    lastEventAt: 0,
    // 正在等用户确认的那条审批（approval/asked 的 data.id），用于配对 decided
    approvalId: null,
  };
}

function textOfMessage(data) {
  const m = data?.message ?? data;
  const c = m?.content;
  if (typeof c === 'string') return c;
  if (Array.isArray(c)) {
    const t = c.find((x) => x && (x.type === 'text' || typeof x.text === 'string'));
    if (t?.text) return t.text;
  }
  if (typeof m?.text === 'string') return m.text;
  return '';
}

// 事件时间的防御性归一化：真实日志里 time 可能是毫秒数、秒数，甚至 ISO 字符串。
// 直接 Number() 会得到 NaN，进而让 state_since 变成 null、awaiting 永不过期。
function eventTime(ev) {
  const n = Number(ev.time);
  if (!Number.isFinite(n)) return Date.now();
  return n < 1e12 ? n * 1000 : n;  // 秒级时间戳换算成毫秒
}

function apply(st, ev) {
  const t = ev.type;
  const time = eventTime(ev);
  const d = ev.data || {};
  switch (t) {
    case 'user/message':
      st.title = truncateChars(textOfMessage(d), 30);
      break;
    case 'turn/start':
      st.turn = d.turn ?? st.turn;
      st.step = 0;
      st.tools = [];
      st.tool = null;
      st.state = 'thinking';
      st.stateSince = time;
      break;
    case 'step/start':
      st.step = d.step ?? st.step;
      st.state = 'thinking';
      st.stateSince = time;
      break;
    case 'tool/call':
      st.state = 'tool';
      st.tool = d.name || 'tool';
      st.stateSince = time;
      st.tools.unshift({ name: String(d.name || 'tool'), ok: null, ts: time });
      st.tools = st.tools.slice(0, 3);
      break;
    case 'tool/result': {
      const last = st.tools[0];
      if (last && last.ok === null) last.ok = !(d.error || d.isError);
      st.state = 'thinking';
      st.stateSince = time;
      break;
    }
    case 'turn/end':
      st.state = d.reason?.kind === 'completed' ? 'done' : 'error';
      st.stateSince = time;
      st.tool = null;
      break;
    case 'job/registered':
      st.jobs.add(d.jobId || `anon-${time}`);
      break;
    case 'job/settled':
      st.jobs.delete(d.jobId || `anon-${time}`);
      break;
    // ---- 权限弹框 ----
    // dsh 会把审批过程写进会话日志，两条事件靠 data.id 配对：
    //   approval/asked   { id, toolName, callId, reason }   ← 弹框出现
    //   approval/decided { id, outcome }                    ← 你点了（allowed-once / denied…）
    // 「有 asked 但没有同 id 的 decided」= 正在等你。
    case 'approval/asked':
      st.state = 'awaiting';
      st.stateSince = time;
      st.approvalId = d.id || null;
      st.tool = d.toolName || null;
      break;
    case 'approval/decided':
      // 只认当前那一条的决议：旧决议不该把后来的新弹框解掉
      if (st.approvalId && d.id && d.id !== st.approvalId) break;
      st.approvalId = null;
      // 决议之后通常会接着执行那个工具，先回到 thinking，等 tool/call 再覆盖
      st.state = 'thinking';
      st.stateSince = time;
      break;
    case 'compaction/prune':
      // 上下文压缩：说明还在同一步里推进，不改状态
      break;
    default:
      break;
  }
  if (time > st.lastEventAt) st.lastEventAt = time;
}

function parseChunk(st, text) {
  for (const line of text.split('\n')) {
    if (!line) continue;
    try {
      apply(st, JSON.parse(line));
    } catch {
      /* 半行/坏行跳过 */
    }
  }
}

export function readDsh(cfg, opts) {
  const provider = {
    id: 'deepseek',
    name: 'DeepSeek',
    state: 'offline',
    state_since: Date.now(),
    task: { title: '', step: null, total: null, tool: null },
    quota: { kind: 'balance', remaining: null, currency: 'CNY' },
    health: 'unknown',
    source: 'local:dsh-session-log',
  };

  try {
    const dir = expandHome(cfg.sessionsDir || '~/.dsh/sessions');
    const file = latestSessionFile(dir);
    if (!file) {
      provider.state = 'idle';
      provider.health = 'ok';
      return provider;
    }

    const st0 = fs.statSync(file);
    if (cache.file !== file || st0.size < cache.consumedBytes) {
      cache.file = file;
      cache.consumedBytes = 0;
      cache.state = emptyState(file);
    }

    if (st0.size > cache.consumedBytes) {
      const compressed = file.endsWith('.zstd');
      // consumedBytes 始终停在「已确认消费」的边界上，所以直接从它开始读
      const start = cache.consumedBytes;
      const len = st0.size - start;
      const fd = fs.openSync(file, 'r');
      let buf;
      try {
        buf = Buffer.alloc(len);
        fs.readSync(fd, buf, 0, len, start);
      } finally {
        fs.closeSync(fd);
      }

      // 文件已经 30 秒没动 → 认为写入方不会再补齐最后一帧
      const stale = Date.now() - st0.mtimeMs > 30000;

      if (compressed) {
        // decompressFrames 只把「完整帧」算进 consumed，写到一半的帧会留在原地。
        // 这里必须按 consumed 推进 offset；旧实现无条件推到最后，半帧那批事件
        // 就被永久跳过了（审批/问答事件恰好可能落在那半帧里）。
        const { text, consumed, tail } = decompressFrames(buf, { allowTail: stale });
        if (consumed > 0) {
          parseChunk(cache.state, text);
          cache.consumedBytes = start + consumed;
        } else if (tail && stale) {
          // 半帧放了很久也没补齐，文件大概率写坏了：跳过它，别让后续帧永远读不到
          cache.consumedBytes = st0.size;
        }
      } else {
        // 未压缩日志：按最后一个换行增量消费。旧实现每次都从 0 重放整个文件，
        // 长会话下是 O(n²)，而且每 2 秒都要重新解析一遍全部事件。
        const nl = buf.lastIndexOf(0x0a);
        if (nl >= 0) {
          parseChunk(cache.state, buf.toString('utf8', 0, nl));
          cache.consumedBytes = start + nl + 1;
        } else if (stale && len > 0) {
          parseChunk(cache.state, buf.toString('utf8'));
          cache.consumedBytes = st0.size;
        }
      }
      cache.mtime = st0.mtimeMs;
    }

    return project(cache.state, opts, provider);
  } catch (e) {
    provider.state = 'offline';
    provider.error = e.message;
    return provider;
  }
}

function project(st, opts, provider) {
  const age = Date.now() - (st.lastEventAt || 0);
  let state = st.state;
  if ((state === 'thinking' || state === 'tool') && age > (opts?.idleAfterMs ?? 300000)) state = 'idle';
  if (state === 'done' && Date.now() - st.stateSince > (opts?.doneAfterMs ?? 180000)) state = 'idle';
  // 等你确认的状态不会自己消失，但也不能无限闪下去：弹框可能已被你关掉窗口。
  // 和 workbuddy 那边用同一个配置项，行为保持一致。
  if (state === 'awaiting' && Date.now() - st.stateSince > (opts?.awaitExpireMs ?? 300000)) {
    state = 'idle';
  }

  provider.state = state;
  provider.state_since = st.stateSince;
  provider.task.title = st.title || '';
  provider.task.step = st.step;
  provider.task.tool = st.tool;
  provider.subagents = st.jobs ? st.jobs.size : 0;
  provider.recent_tools = st.tools.map((t) => ({ name: t.name, ok: t.ok }));
  provider.health = 'ok';
  return provider;
}
