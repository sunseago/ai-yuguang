// dsh「等确认」检测自测：自己造一份合成日志，不依赖真实会话文件。
//
//   node tests/dsh-approval.mjs
//
// 事件形状取自 2026-11-07 的真实抓包：
//   approval/asked   { id, toolName, callId, reason }
//   approval/decided { id, outcome }         outcome 例如 allowed-once
//
// 注意这里必须"追加新帧"而不是重写文件：dsh 每批事件写一个新的 zstd 帧，
// 适配器也是按帧增量读的。用 writeFileSync 整体重写会把全部内容压成单帧，
// 增量区间跨在帧中间，解不出来——第一版测试就是这么假失败的。
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { readDsh } from '../adapters/dsh.mjs';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-dsh-'));
const dir = path.join(root, 'demo-project', 'session-test');
fs.mkdirSync(dir, { recursive: true });
const file = path.join(dir, 'session.v4.jsonl.zstd');
const cfg = { sessionsDir: root };

const T0 = Date.now() - 60000;
let seq = 0;

/** 追加一个 zstd 帧（dsh 就是这么写的） */
const append = (...events) => {
  const txt = events
    .map((e) => JSON.stringify({ seq: ++seq, ...e }))
    .join('\n');
  fs.appendFileSync(file, zlib.zstdCompressSync(Buffer.from(txt + '\n')));
};

let fails = 0;
const check = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

// 1. 正常跑，没有任何审批
fs.writeFileSync(file, '');
append({ type: 'turn/start', time: T0, data: { turn: 1 } });
append({ type: 'tool/call', time: T0 + 1, data: { name: 'bash' } });
check('普通 tool/call 是 tool 状态', readDsh(cfg, {}).state === 'tool');

// 2. 弹框出现，用户还没点
append({
  type: 'approval/asked',
  time: T0 + 2,
  data: { id: 'a1', toolName: 'bash', callId: 'c1', reason: 'need write' },
});
const p2 = readDsh(cfg, {});
check('approval/asked → awaiting', p2.state === 'awaiting', p2.state);
check('等待期间 task.tool 显示被问的工具', p2.task.tool === 'bash', String(p2.task.tool));

// 3. 用户点了允许
append({ type: 'approval/decided', time: T0 + 3, data: { id: 'a1', outcome: 'allowed-once' } });
check('同 id 的 decided 解除等待', readDsh(cfg, {}).state !== 'awaiting');

// 4. 又弹一个；此时来一条"旧 id"的决议，不该把新的解掉
append({
  type: 'approval/asked',
  time: T0 + 4,
  data: { id: 'a2', toolName: 'write_file', callId: 'c2', reason: 'need write' },
});
append({ type: 'approval/decided', time: T0 + 5, data: { id: 'a1', outcome: 'allowed-once' } });
const p4 = readDsh(cfg, {});
check('旧 id 的决议不影响新弹框', p4.state === 'awaiting', p4.state);

// 5. 过期：15 分钟前弹的、一直没人理，不能永远闪
append({ type: 'approval/decided', time: T0 + 6, data: { id: 'a2', outcome: 'denied' } });
append({
  type: 'approval/asked',
  time: Date.now() - 900000,
  data: { id: 'a3', toolName: 'bash', callId: 'c3' },
});
check('超过 5 分钟的未决判为过期', readDsh(cfg, {}).state !== 'awaiting');
check('放宽到 20 分钟后仍算等待', readDsh(cfg, { awaitExpireMs: 1200000 }).state === 'awaiting');

fs.rmSync(root, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
