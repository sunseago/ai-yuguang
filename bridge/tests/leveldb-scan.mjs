// Kimi 登录态兜底：WAL 里没有令牌时，从 .ldb 的明文 JWT 里扫出来。
//   node tests/leveldb-scan.mjs
//
// 背景：Chromium 会把 Local Storage 的 WAL 压实成 .ldb，压实后 *.log 里就
// 找不到 access_token / refresh_token 了。这里造一个"只有 .ldb"的目录验证兜底。
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { scanJwtTokens } from '../lib/leveldb-log.mjs';
import { readKimiSession } from '../lib/kimi-session.mjs';

let fails = 0;
const ok = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const now = Math.floor(Date.now() / 1000);
const b64 = (o) => Buffer.from(JSON.stringify(o)).toString('base64url');
const jwt = (claims) => `${b64({ alg: 'HS512', typ: 'JWT' })}.${b64(claims)}.${'sig'.repeat(10)}`;

const access = jwt({ sub: 'user-1', exp: now + 600, membership: { level: 10 } });
const refresh = jwt({ sub: 'user-1', exp: now + 90 * 86400, membership: { level: 10 } });
const expired = jwt({ sub: 'user-1', exp: now - 3600 });

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-ldb-'));
const dir = path.join(tmp, 'Local Storage', 'leveldb');
fs.mkdirSync(dir, { recursive: true });
// 模拟 .ldb：JWT 前后夹一些二进制噪声（真实 data block 就是这样）
const blob = Buffer.concat([
  Buffer.from([0x00, 0x2b, 0x03]),
  Buffer.from(`_https://www.kimi.com\x00\x01access_token\x01${access}`, 'latin1'),
  Buffer.from([0x00, 0xff, 0x0d]),
  Buffer.from(`_https://www.kimi.com\x00\x01refresh_token\x01${refresh}`, 'latin1'),
  Buffer.from([0x01, 0x00]),
  Buffer.from(`_https://www.kimi.com\x00\x01old_token\x01${expired}`, 'latin1'),
]);
fs.writeFileSync(path.join(dir, '000123.ldb'), blob);

const scanned = scanJwtTokens(dir);
ok('从 .ldb 扫到 refresh', scanned.refresh === refresh);
ok('从 .ldb 扫到 access', scanned.access === access);
ok('过期令牌被排除', scanned.refresh !== expired);

const sess = readKimiSession(tmp);
ok('readKimiSession 走兜底拿到 refresh', sess?.refresh === refresh);
ok('会员等级从 refresh 声明里读出来', sess?.membershipLevel === 10, String(sess?.membershipLevel));

// 只有过期令牌的目录应该返回 null，不能把过期令牌当有效
const dir2 = path.join(tmp, 'expired', 'Local Storage', 'leveldb');
fs.mkdirSync(dir2, { recursive: true });
fs.writeFileSync(path.join(dir2, '000001.ldb'), Buffer.from(expired, 'latin1'));
ok('全部过期时返回 null', readKimiSession(path.join(tmp, 'expired')) === null);

fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
