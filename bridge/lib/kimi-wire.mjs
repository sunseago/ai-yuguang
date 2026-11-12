// Kimi 桌面端的会话事件流（wire.jsonl）解析。
//
// 为什么需要它：conversation-statuses.json 在"等你确认"时仍然是 running，
// 分不出"正在干活"和"卡在弹窗上"；runner.state.json 的 pendingInteractionCount
// 又不是实时字段。真正可靠的信号是会话目录里的 wire.jsonl：
//
//   interaction.request  { id, kind: 'approval'|'user_tool', request: {...}, time }
//   interaction.resolved { id, response, time }
//   agent.turn.ended     { outcome: 'done', time }
//
// 判定：某个 interaction.request 的 id 没有对应的 resolved，且没有更晚的
// turn.ended 把它作废 —— 就是"还挂在那儿等你点确认"。
//
// 取证记录见 docs/ 与工作日志：这是 2026-11-12 实测 Kimi 桌面版得到的**未公开格式**，
// 客户端升级可能变。所有解析都要能失败后安静退化，不能把桥接带崩。
import fs from 'node:fs';
import path from 'node:path';

const TAIL_BYTES = 128 * 1024;

// 只有 approval 才是"要用户点确认"。
// user_tool 是 agent 调工具时的往返（几秒内自动 resolved）—— 2026-11-12 实测：
// 一次普通任务里 agent 连着调了 5 次工具，每次 request→resolved 之间有 4~7 秒，
// 当成审批会让卡片每几秒红闪+响一次。把 user_tool 也算进去 = 误报机器。
const USER_APPROVAL_KINDS = new Set(['approval']);

// 刚出现不足这么久的新 request 不报：用户秒点确认时别闪一下
const MIN_PENDING_AGE_MS = 1500;

/** 纯函数：把一段 wire 文本（允许开头是半行）解析成信号，便于单测。 */
export function parseWireText(text) {
  let pending = null;
  const resolved = new Set();
  let lastTurnEnded = 0;

  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    // 尾部读取可能从半行开始；只认以 '{' 开头的完整 JSON 行
    if (!line || line[0] !== '{') continue;
    let o;
    try {
      o = JSON.parse(line);
    } catch {
      continue;
    }
    switch (o.type) {
      case 'interaction.resolved':
        if (o.id) resolved.add(o.id);
        break;
      case 'interaction.request':
        if (o.id && USER_APPROVAL_KINDS.has(String(o.kind || ''))) {
          pending = {
            id: o.id,
            kind: o.kind || '',
            time: Number(o.time) || 0,
            toolName: o.request?.toolName || '',
            action: o.request?.action || '',
            description: o.request?.display?.description || '',
          };
        }
        break;
      case 'agent.turn.ended':
        if (o.outcome === 'done' || !o.outcome) {
          const t = Number(o.time) || 0;
          if (t > lastTurnEnded) lastTurnEnded = t;
        }
        break;
      default:
        break;
    }
  }

  // 已被回答，或那一轮已经结束（弹窗随轮次作废）
  if (pending && (resolved.has(pending.id) || (lastTurnEnded && lastTurnEnded > pending.time))) {
    pending = null;
  }
  return { pending, lastTurnEnded };
}

/** 只读文件末尾，避免每 2 秒把 2MB+ 的事件流整个读一遍。 */
export function readWireTail(file, maxBytes = TAIL_BYTES) {
  const fd = fs.openSync(file, 'r');
  try {
    const size = fs.fstatSync(fd).size;
    const start = Math.max(0, size - maxBytes);
    const len = size - start;
    const buf = Buffer.alloc(len);
    fs.readSync(fd, buf, 0, len, start);
    let text = buf.toString('utf8');
    if (start > 0) {
      const nl = text.indexOf('\n');
      text = nl >= 0 ? text.slice(nl + 1) : '';  // 丢掉开头被截断的半行
    }
    return text;
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * 找到某个会话（conversationKey，形如 agent:main:main:conversation:<uuid>）的 wire 文件。
 * 目录结构：sessions/<workspace>/conv-<id>/state.json（里面有 custom.conversationKey）
 * 事件流在：sessions/<workspace>/conv-<id>/agents/main/wire.jsonl
 * 找不到精确匹配时退回"最近改动的那个"会话。
 */
export function findConversationWire(appDataDir, conversationKey) {
  const root = path.join(appDataDir, 'daimon-share', 'daimon', 'runtime', 'kimi-code', 'home', 'sessions');
  let workspaces;
  try {
    workspaces = fs.readdirSync(root);
  } catch {
    return null;
  }
  let best = null;
  let fallback = null;
  for (const ws of workspaces) {
    const wsDir = path.join(root, ws);
    let convs;
    try {
      convs = fs.readdirSync(wsDir);
    } catch {
      continue;
    }
    for (const conv of convs) {
      if (!conv.startsWith('conv-')) continue;
      const convDir = path.join(wsDir, conv);
      const wire = path.join(convDir, 'agents', 'main', 'wire.jsonl');
      let key = null;
      try {
        const st = JSON.parse(fs.readFileSync(path.join(convDir, 'state.json'), 'utf8'));
        key = st?.custom?.conversationKey || null;
      } catch {
        // 没 state.json 也能靠目录名兜底，只是对不上会话
      }
      let mtime = 0;
      try {
        mtime = fs.statSync(wire).mtimeMs;
      } catch {
        continue;
      }
      const item = { wire, key, mtime };
      if (!fallback || mtime > fallback.mtime) fallback = item;
      if (conversationKey && key === conversationKey && (!best || mtime > best.mtime)) best = item;
    }
  }
  // 指定了会话却没找到它 —— 不要退回别的会话：那份 wire 的 turn.ended 属于
  // 另一个任务，会造成"这边在跑、那边刚结束"的假 DONE。
  return conversationKey ? best : fallback;
}

/**
 * 带 mtime 缓存的读取器：文件没变就直接返回上次结果。
 * 返回 { pending, lastTurnEnded, key }，任何异常都退化成空信号。
 */
export function createWireReader(appDataDir) {
  const cache = new Map();  // wire path -> {mtime, result}
  return {
    sample(conversationKey) {
      const found = findConversationWire(appDataDir, conversationKey);
      if (!found) return { pending: null, lastTurnEnded: 0, key: null };
      const hit = cache.get(found.wire);
      if (hit && hit.mtime === found.mtime) return { ...hit.result, key: found.key };
      let result = { pending: null, lastTurnEnded: 0 };
      try {
        result = parseWireText(readWireTail(found.wire));
      } catch {
        // 事件流格式变了/正在写入：安静退化，不影响额度与其它 provider
      }
      cache.set(found.wire, { mtime: found.mtime, result });
      // 宽限：刚出现的 request 先不报，等它"悬着"够久才算真的在等你
      const pending = result.pending && Date.now() - result.pending.time >= MIN_PENDING_AGE_MS
        ? result.pending
        : null;
      return { ...result, pending, key: found.key };
    },
  };
}
