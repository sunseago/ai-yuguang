/**
 * 读取 WorkBuddy 真实积分余额。
 *
 * 为什么需要「借壳」：2026-09 起客户端把 accessToken 改成了 at-rest 信封
 * （AES-256-GCM，保护钥来自定制 Electron 的原生绑定），磁盘上没有可直接用的明文
 * token。社区做法（workbuddy-checkin 的 oracle 模块）是让 WorkBuddy 客户端自己
 * 当解释器跑这个脚本：进程内取凭证 → 立刻发请求，token 不落盘、不打印。
 *
 * 调用方式（由 bridge/lib/workbuddy-credits.mjs 负责）：
 *   <WorkBuddy 可执行文件> <本目录> summary
 *
 * stdout 约定：一行 `WBSUMMARY {json}`，json = { total, groups, at }
 *   total  剩余积分（与客户端顶部「剩余积分」一致）
 *   groups [{ kind, name, remain, total, used }]
 * 退出码：0 成功；3 取凭证失败（未登录 / 客户端升级改了格式）
 *
 * 格式说明见 docs：at-rest 信封 = {suite,keyId,nonce,authTag,ciphertext}，
 * AAD 为 "WB-AAD\0" | 0x01 | LP(magic) | LP("sym-v1") | u32(suite) | LP(keyId) | code | 0x00 | 0x00，
 * 字段级 framing 用 magic "WBEV1" / code 2。
 */
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const API_BASES = ['https://copilot.tencent.com', 'https://www.workbuddy.cn'];
const AUTH_CANDIDATES = [
  path.join(
    os.homedir(),
    'Library',
    'Application Support',
    'CodeBuddyExtension',
    'Data',
    'Public',
    'auth',
    'workbuddy-desktop.info'
  ),
  path.join(
    os.homedir(),
    'Library',
    'Application Support',
    'CodeBuddyExtension',
    'Data',
    'Public',
    'auth',
    'Tencent-Cloud.coding-copilot.info'
  ),
  path.join(
    process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'),
    'CodeBuddyExtension',
    'Data',
    'Public',
    'auth',
    'workbuddy-desktop.info'
  ),
];

function die(msg) {
  process.stderr.write('[wb-oracle] ' + msg + '\n');
  process.exit(3);
}

/** 客户端刷新 token 时会整体重写文件，撞上中途会读到半截 JSON */
function readAuthJson() {
  let last;
  for (let i = 0; i < 4; i++) {
    for (const p of AUTH_CANDIDATES) {
      try {
        return JSON.parse(fs.readFileSync(p, 'utf8'));
      } catch (e) {
        last = e;
      }
    }
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 700);
  }
  die('读不到登录态文件（' + (last ? last.message : '未登录') + '）');
}

function lp(s) {
  const b = Buffer.from(s, 'utf8');
  const l = Buffer.alloc(4);
  l.writeUInt32BE(b.length);
  return Buffer.concat([l, b]);
}

function buildAad(keyId, framing) {
  const isFile = framing === 'file';
  const suite = Buffer.alloc(4);
  suite.writeUInt32BE(1);
  return Buffer.concat([
    Buffer.from('WB-AAD\0', 'latin1'),
    Buffer.from([1]),
    lp(isFile ? 'WBEF1' : 'WBEV1'),
    lp('sym-v1'),
    suite,
    lp(keyId),
    Buffer.from([isFile ? 1 : 2]),
    Buffer.from([0]),
    Buffer.from([0]),
  ]);
}

function openEnvelope(env, key, framing) {
  const d = crypto.createDecipheriv('aes-256-gcm', key, Buffer.from(env.nonce, 'base64'), {
    authTagLength: 16,
  });
  d.setAAD(buildAad(env.keyId, framing));
  d.setAuthTag(Buffer.from(env.authTag, 'base64'));
  return Buffer.concat([d.update(Buffer.from(env.ciphertext, 'base64')), d.final()]);
}

function loadCredential() {
  let payload;
  try {
    // 定制 Electron 的原生绑定：只有 WorkBuddy 自己跑起来才有
    payload = JSON.parse(process._linkedBinding('electron_browser_workbuddy_storage').loggerGet());
  } catch (e) {
    die('原生绑定不可用（需用 WorkBuddy 客户端运行）: ' + e.message);
  }
  const protectorKey = crypto.createHash('sha256').update(payload.atRestSecretKey, 'utf8').digest();

  const auth = readAuthJson();
  const wrapper = (auth.auth || {}).accessToken;
  if (!wrapper || wrapper.$wbEncrypted !== 1 || typeof wrapper.envelope !== 'string') {
    die('accessToken 不是信封格式（客户端版本可能已变）');
  }

  let token;
  try {
    token = openEnvelope(
      JSON.parse(Buffer.from(wrapper.envelope, 'base64').toString('utf8')),
      protectorKey,
      'field'
    ).toString('utf8');
  } catch {
    die('accessToken 解密失败（保护钥换代，需更新本脚本）');
  }
  if (!token) die('accessToken 为空，请重新登录 WorkBuddy');
  return { token, uid: (auth.account || {}).uid || '' };
}

function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

function summarize(body) {
  const d = body.data || body.Data || {};
  const groups = Array.isArray(d.CreditGroups) ? d.CreditGroups : [];
  const compact = groups.map((g) => ({
    kind: g.Kind || '',
    name: g.Name || '',
    remain: num(g.CycleRemainCapacity),
    total: num(g.CycleTotalCapacity),
    used: num(g.CycleUsedCapacity),
  }));

  // TotalCredits 是官方口径；缺失时退回按分组求和
  let total = d.TotalCredits == null ? null : num(d.TotalCredits);
  if (total == null) total = compact.reduce((s, g) => s + g.remain, 0);

  const totalCap = compact.reduce((s, g) => s + g.total, 0);
  const usedCap = compact.reduce((s, g) => s + g.used, 0);
  return { total, capacity: totalCap, used: usedCap, groups: compact, at: Date.now() };
}

async function main() {
  const cred = loadCredential();
  const headers = {
    'Content-Type': 'application/json',
    Accept: 'application/json',
    Authorization: 'Bearer ' + cred.token,
  };
  if (cred.uid) {
    headers['X-User-Id'] = cred.uid;
    headers['X-Account-Uid'] = cred.uid;
  }

  let lastStatus = 0;
  for (const base of API_BASES) {
    try {
      const resp = await fetch(base + '/billing/meter/get-user-resource-summary', {
        method: 'POST',
        headers,
        body: '{}',
      });
      lastStatus = resp.status;
      if (resp.status !== 200) continue;
      const body = await resp.json();
      if (body.code !== 0 && body.code !== undefined) {
        process.stderr.write('[wb-oracle] ' + base + ' code=' + body.code + ' ' + (body.msg || '') + '\n');
        continue;
      }
      process.stdout.write('WBSUMMARY ' + JSON.stringify(summarize(body)) + '\n');
      return;
    } catch (e) {
      process.stderr.write('[wb-oracle] ' + base + ' 请求失败: ' + e.message + '\n');
    }
  }
  die('余额接口不可用（最后 HTTP ' + lastStatus + '）');
}

main().catch((e) => die(e && e.stack ? e.stack : String(e)));
