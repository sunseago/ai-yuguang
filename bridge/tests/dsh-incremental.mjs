// 增量解析自测：复制一份真实 dsh 日志到临时目录，
// 追加一个压缩帧，确认「新增事件被看到」且「旧事件不被重复计数」。
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { readDsh } from '../adapters/dsh.mjs';

const src = path.join(os.homedir(), '.dsh/sessions');
const proj = fs.readdirSync(src).find((d) => fs.statSync(path.join(src, d)).isDirectory());
const sess = fs.readdirSync(path.join(src, proj)).find((d) => d.startsWith('session-'));
const srcFile = path.join(src, proj, sess, 'session.v4.jsonl.zstd');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-'));
const dir = path.join(tmp, 'demo-project', 'session-test');
fs.mkdirSync(dir, { recursive: true });
const dst = path.join(dir, 'session.v4.jsonl.zstd');
fs.copyFileSync(srcFile, dst);

const cfg = { sessionsDir: tmp };
let fails = 0;
const check = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const a = readDsh(cfg, {});
console.log('基线:', a.state, 'step=' + a.task.step, 'jobs=' + a.subagents, 'tools=' + JSON.stringify(a.recent_tools));
check('解析出任务标题', !!a.task.title, a.task.title);
check('解析出 step', a.task.step !== null);

const b = readDsh(cfg, {});
check('无变更时不重复计数', b.subagents === a.subagents && b.recent_tools.length === a.recent_tools.length);

// 追加「新的一轮 + 工具调用」
const baseTime = Date.now();
const evs = [
  { type: 'user/message', seq: 900001, time: baseTime + 1000, data: { message: { role: 'user', content: [{ type: 'text', text: '刷新一下固件测试' }] } } },
  { type: 'turn/start', seq: 900002, time: baseTime + 1001, data: { turn: 33 } },
  { type: 'step/start', seq: 900003, time: baseTime + 1002, data: { turn: 33, step: 1 } },
  { type: 'tool/call', seq: 900004, time: baseTime + 1003, data: { turn: 33, step: 1, callId: 'c1', name: 'bash', arguments: '{}' } },
  { type: 'job/registered', seq: 900005, time: baseTime + 1004, data: { jobId: 'job-test', kind: 'bash' } },
];
const payload = evs.map((e) => JSON.stringify(e)).join('\n') + '\n';
fs.appendFileSync(dst, zlib.zstdCompressSync(Buffer.from(payload, 'utf8')));

const c = readDsh(cfg, {});
console.log('追加后:', c.state, 'step=' + c.task.step, 'jobs=' + c.subagents, 'tools=' + JSON.stringify(c.recent_tools));
check('看到新状态', c.state === 'tool');
check('看到新 step', c.task.step === 1);
check('看到新标题', c.task.title === '刷新一下固件测试', c.task.title);
check('jobs 只加 1', c.subagents === a.subagents + 1, `${a.subagents} -> ${c.subagents}`);
check('工具栈长度不超过 3', c.recent_tools.length <= 3);

// 再追加一个半帧（模拟写入中途被读到），不应崩
const half = zlib.zstdCompressSync(Buffer.from('{"type":"turn/end","seq":900006,"time":' + (baseTime + 2000) + ',"data":{"turn":33,"reason":{"kind":"completed"}}}\n'));
fs.appendFileSync(dst, half.subarray(0, Math.floor(half.length / 2)));
const d = readDsh(cfg, {});
check('半帧不崩且状态不乱', typeof d.state === 'string', d.state);

fs.appendFileSync(dst, half.subarray(Math.floor(half.length / 2)));
const e = readDsh(cfg, {});
console.log('补齐半帧后:', e.state);
check('补齐后状态推进到 done', e.state === 'done', e.state);

fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
