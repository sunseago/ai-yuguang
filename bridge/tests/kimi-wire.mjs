// Kimi wire 事件流解析单测：
//   node tests/kimi-wire.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parseWireText, readWireTail, findConversationWire, createWireReader } from '../lib/kimi-wire.mjs';

let fails = 0;
const ok = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const line = (o) => JSON.stringify(o);
const req = (id, time, extra = {}) =>
  line({ type: 'interaction.request', id, kind: 'approval', time, request: { toolName: 'Bash', ...extra } });
const resolved = (id, time) => line({ type: 'interaction.resolved', id, time });
const ended = (time) => line({ type: 'agent.turn.ended', outcome: 'done', time });

// 1) 挂起的审批 = 等确认
{
  const r = parseWireText([
    line({ type: 'agent.turn.started', time: 1000 }),
    req('approval_1', 2000, { display: { description: '删除文件夹' } }),
  ].join('\n'));
  ok('有 request 无 resolved → pending', !!r.pending);
  ok('pending 带出工具名', r.pending?.toolName === 'Bash');
  ok('pending 带出中文描述', r.pending?.description === '删除文件夹');
  ok('pending 时间是事件时间', r.pending?.time === 2000);
}

// 2) 已点确认 → 不再挂起
{
  const r = parseWireText([req('approval_1', 2000), resolved('approval_1', 3500)].join('\n'));
  ok('同 id 的 resolved 会消掉 pending', r.pending === null);
}

// 3) 那一轮结束了，弹窗作废
{
  const r = parseWireText([req('approval_1', 2000), ended(4000)].join('\n'));
  ok('更晚的 turn.ended 让弹窗失效', r.pending === null);
  ok('turn.ended 时间被记下', r.lastTurnEnded === 4000);
}

// 4) 半行开头 / 噪声行不能把解析带崩
{
  const broken = '{"type":"interaction.resol' + '\n' + req('approval_2', 5000) + '\n' + 'garbage\n';
  const r = parseWireText(broken);
  ok('截断的半行被忽略', r.pending?.id === 'approval_2');
}

// 5) 多个 request 只看最后一个；旧 id 的 resolved 不能误伤新的
{
  const r = parseWireText([
    req('old', 1000), resolved('old', 1500), req('new', 2000),
  ].join('\n'));
  ok('只保留最后一个 request', r.pending?.id === 'new');
}

// 6) 尾部读取：大文件里仍能拿到最后一行
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kimi-wire-'));
  const f = path.join(dir, 'wire.jsonl');
  fs.writeFileSync(f, 'x'.repeat(200 * 1024) + '\n' + req('tail_req', 9000) + '\n');
  const r = parseWireText(readWireTail(f, 4096));
  ok('尾部 4KB 能读到最后一条事件', r.pending?.id === 'tail_req');
}

// 7) 会话目录发现 + mtime 缓存
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kimi-tree-'));
  const root = path.join(dir, 'daimon-share', 'daimon', 'runtime', 'kimi-code', 'home', 'sessions');
  const conv = path.join(root, 'ws-1', 'conv-abc');
  fs.mkdirSync(path.join(conv, 'agents', 'main'), { recursive: true });
  fs.writeFileSync(path.join(conv, 'state.json'), JSON.stringify({
    custom: { conversationKey: 'agent:main:main:conversation:abc' },
  }));
  fs.writeFileSync(path.join(conv, 'agents', 'main', 'wire.jsonl'), req('live_req', 7000) + '\n');

  const found = findConversationWire(dir, 'agent:main:main:conversation:abc');
  ok('按 conversationKey 找到 wire', !!found && found.key === 'agent:main:main:conversation:abc');
  // 2026-11-12 改：指定了会话却找不到时不跨会话回退（否则会拿别的任务的 turn.ended 假报 DONE）
  ok('找不到的 key 不跨会话回退', findConversationWire(dir, 'nope') === null);

  const reader = createWireReader(dir);
  ok('reader 读出挂起事件', reader.sample('agent:main:main:conversation:abc').pending?.id === 'live_req');
  fs.writeFileSync(path.join(conv, 'agents', 'main', 'wire.jsonl'),
    req('live_req', 7000) + '\n' + resolved('live_req', 8000) + '\n');
  ok('mtime 变化后重新解析', reader.sample('agent:main:main:conversation:abc').pending === null);
}

// 8) user_tool 不是"等用户确认"：agent 调工具的往返会误报
{
  const r = parseWireText([
    line({ type: 'interaction.request', id: 'user_tool_1', kind: 'user_tool', time: 1000 }),
  ].join('\n'));
  ok('user_tool 的 request 不算 pending', r.pending === null);
  const a = parseWireText([
    line({ type: 'interaction.request', id: 'approval_1', kind: 'approval', time: 1000 }),
  ].join('\n'));
  ok('approval 的 request 才算 pending', a.pending?.id === 'approval_1');
}

// 9) 指定了会话却找不到它 → 不退回别的会话（避免跨会话假 DONE）
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kimi-nofallback-'));
  const root = path.join(dir, 'daimon-share', 'daimon', 'runtime', 'kimi-code', 'home', 'sessions');
  const conv = path.join(root, 'ws-1', 'conv-abc');
  fs.mkdirSync(path.join(conv, 'agents', 'main'), { recursive: true });
  fs.writeFileSync(path.join(conv, 'state.json'), JSON.stringify({
    custom: { conversationKey: 'agent:main:main:conversation:abc' },
  }));
  fs.writeFileSync(path.join(conv, 'agents', 'main', 'wire.jsonl'), line({ type: 'agent.turn.ended', outcome: 'done', time: 9000 }) + '\n');
  ok('找不到指定会话时返回 null（不跨会话）', findConversationWire(dir, 'agent:main:main:conversation:zzz') === null);
  ok('没指定会话时才回退到最近的', !!findConversationWire(dir, null));
}

// 10) 目录不存在时安静退化
{
  const reader = createWireReader(path.join(os.tmpdir(), 'definitely-not-here-' + Date.now()));
  const r = reader.sample(null);
  ok('目录缺失返回空信号', r.pending === null && r.lastTurnEnded === 0);
}

console.log(fails ? `\n有 ${fails} 项失败` : '\n全部通过');
process.exit(fails ? 1 : 0);
