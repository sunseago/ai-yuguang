// 「等确认」检测自测：纯文件，不需要 WorkBuddy 也不需要数据库。
//
//   node tests/approval.mjs
//
// 验的是 lib/approval.mjs 的四件事：配对、增量、半行、过期。
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createApprovalWatcher } from '../lib/approval.mjs';

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-approval-'));
const d = new Date();
const key = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(
  d.getDate()
).padStart(2, '0')}`;
const file = path.join(dir, `${key}.jsonl`);
const cfg = {};

let fails = 0;
const check = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const ev = (o) => JSON.stringify({ timestamp: Date.now(), ...o }) + '\n';
const watch = () => createApprovalWatcher(() => dir);

// 1. 今天还没有审计文件
check('文件不存在时返回 null', watch().poll(cfg) === null);

// 2. 弹框出现
fs.writeFileSync(file, '');
check('空文件返回 null', watch().poll(cfg) === null);
fs.appendFileSync(file, ev({ eventType: 'file-safety.needs-approval', toolCallId: 'a' }));
const w1 = watch();
const p1 = w1.poll(cfg);
check('needs-approval → 判定为等待中', !!p1 && p1.kind === 'file-safety.needs-approval');

// 3. 同一实例增量读到新增的 approved（模拟轮询）
fs.appendFileSync(
  file,
  ev({ eventType: 'file-safety.approved', toolCallId: 'a' })
);
check('同一实例读增量：批准后解除', w1.poll(cfg) === null);

// 4. 两条未决，取最新的一条
fs.appendFileSync(
  file,
  ev({ eventType: 'file-safety.needs-approval', toolCallId: 'b' }) +
    ev({ eventType: 'file-safety.bulk-delete.needs-approval', toolCallId: 'c' })
);
const w2 = watch();
const p2 = w2.poll(cfg);
check('两条未决时取最新', !!p2 && p2.kind === 'file-safety.bulk-delete.needs-approval');

// 5. 只拒绝其中一条，另一条继续算等待
fs.appendFileSync(file, ev({ eventType: 'file-safety.rejected', toolCallId: 'b' }));
const p3 = w2.poll(cfg);
check('拒绝一条后另一条仍是等待', !!p3 && p3.kind.endsWith('bulk-delete.needs-approval'));

// 6. 半行写入：不能被解析成半条记录，也不能把 offset 带偏
const before = fs.readFileSync(file, 'utf8');
fs.appendFileSync(file, '{"eventType":"file-safety.need'); // 故意写半截
const p4 = w2.poll(cfg);
check('末尾半行不影响当前判定', !!p4 && p4.kind.endsWith('bulk-delete.needs-approval'));
fs.writeFileSync(file, before); // 抹掉半行，模拟那一行其实没写成
fs.appendFileSync(file, ev({ eventType: 'file-safety.approved', toolCallId: 'c' }));
check('补齐后全部解除', w2.poll(cfg) === null);

// 7. 过期：很久以前的弹框不再算数（不能永远闪下去）
fs.appendFileSync(
  file,
  JSON.stringify({
    eventType: 'file-safety.needs-approval',
    toolCallId: 'old',
    timestamp: Date.now() - 600000, // 10 分钟前
  }) + '\n'
);
check('超过 5 分钟的未决判为过期', watch().poll(cfg, 300000) === null);
check('放宽到 15 分钟后仍算等待', watch().poll(cfg, 900000) !== null);

fs.rmSync(dir, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
