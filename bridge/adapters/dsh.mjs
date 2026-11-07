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

function apply(st, ev) {
  const t = ev.type;
  const time = Number(ev.time || Date.now());
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
      cache.size = -1;
    }

    if (st0.size !== cache.size) {
      const compressed = file.endsWith('.zstd');
      // consumedBytes 始终停在帧边界上，所以直接从它开始读，不需要回退（回退会重复解析）
      const start = compressed ? cache.consumedBytes : 0;
      const len = st0.size - start;
      if (len > 0) {
        const fd = fs.openSync(file, 'r');
        let buf;
        try {
          buf = Buffer.alloc(len);
          fs.readSync(fd, buf, 0, len, start);
        } finally {
          fs.closeSync(fd);
        }

        if (compressed) {
          // 关键：Node 的 zstd 对「被截断的帧」不报错，会静默返回部分内容。
          // 因此用 JSONL 的行边界做完整性判据：解出来的文本必须以换行结尾，
          // 否则说明最后一帧还在写，本轮不消费，下轮从同一帧边界重试。
          const { text, frames } = decompressFrames(buf);
          const settled = text.endsWith('\n') || Date.now() - st0.mtimeMs > 30000;
          if (frames > 0 && settled) {
            parseChunk(cache.state, text);
            cache.consumedBytes = st0.size;
            cache.size = st0.size;
          }
        } else {
          parseChunk(cache.state, buf.toString('utf8'));
          cache.consumedBytes = st0.size;
          cache.size = st0.size;
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
