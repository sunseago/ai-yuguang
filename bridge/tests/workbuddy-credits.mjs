// WorkBuddy 真实余额取数：解析 WBSUMMARY、缓存、失败回退。
//   node tests/workbuddy-credits.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-wbc-'));
const okBin = path.join(tmp, 'fake-wb-ok.sh');
fs.writeFileSync(
  okBin,
  '#!/bin/sh\nprintf "Electron noise\\nWBSUMMARY {\\"total\\":2041.55,\\"capacity\\":3229,\\"used\\":1187.45,\\"groups\\":[],\\"at\\":1794317603509}\\n"\n'
);
fs.chmodSync(okBin, 0o755);

const badBin = path.join(tmp, 'fake-wb-bad.sh');
fs.writeFileSync(badBin, '#!/bin/sh\necho "未登录" >&2\nexit 3\n');
fs.chmodSync(badBin, 0o755);

let fails = 0;
const ok = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

process.env.WB_ELECTRON_BIN = okBin;
const mod = await import('../lib/workbuddy-credits.mjs');

const v = await mod.refreshWorkBuddyCredits();
ok('解析出真实余额', Math.abs((v?.total ?? 0) - 2041.55) < 0.01, String(v?.total));
ok('解析出总额度', v?.capacity === 3229, String(v?.capacity));
ok('解析出已用', Math.abs((v?.used ?? 0) - 1187.45) < 0.01, String(v?.used));
ok('成功时清空错误', mod.workBuddyCreditsError() === null);
ok('同步读取命中缓存', Math.abs((mod.workBuddyCredits()?.total ?? 0) - 2041.55) < 0.01);

// 取数失败：本轮返回 null、记录错误，但保留上一次的余额（屏上不该突然变空）。
// force=true 才能绕过 TTL——正常轮询里 5 分钟内不会再起 Electron。
process.env.WB_ELECTRON_BIN = badBin;
const v2 = await mod.refreshWorkBuddyCredits(true);
ok('失败时返回 null', v2 === null);
ok('失败信息可见', /未登录/.test(mod.workBuddyCreditsError() || ''), mod.workBuddyCreditsError());
ok('失败后仍保留上次余额', Math.abs((mod.workBuddyCredits()?.total ?? 0) - 2041.55) < 0.01);

fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
