#!/usr/bin/env node
// AI余光 桥接：把三家的状态与额度聚合成一份 JSON，给 ESP32 拉取。
// 设计约束（来自 PRD）：零第三方依赖、只读本地数据、密钥不出电脑。
import http from 'node:http';
import dgram from 'node:dgram';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { BRIDGE_VERSION } from './version.mjs';
import { pickConfig, log } from './lib/util.mjs';
import { createQuotaStore } from './lib/quota.mjs';
import { hostIdleSeconds } from './lib/host-idle.mjs';
import { refreshDeepSeekQuota, deepSeekQuota } from './lib/deepseek-balance.mjs';
import { readWorkBuddy } from './adapters/workbuddy.mjs';
import { readDsh } from './adapters/dsh.mjs';
import { refreshKimiQuota, readKimi } from './adapters/kimi.mjs';
import { mockState } from './lib/mock.mjs';

const appDir = path.dirname(fileURLToPath(import.meta.url));

// ---- 运行目录 ----
// 开发态：配置和数据就在脚本旁边（node deskpulse-bridge.mjs 直接跑）。
// 打包成 .app 后：bundle 内的文件一个字节都不能改，否则 codesign 封条失效、
// 重签之前整包会被判定为已损坏。所以 DP_PACKAGED=1 时把读写目录搬到用户目录。
const runtimeDir =
  process.env.DP_HOME ||
  (process.env.DP_PACKAGED === '1'
    ? path.join(os.homedir(), 'Library', 'Application Support', 'DeskPulse')
    : appDir);
// 配置目录：优先用运行目录里的 config.json，没有就退回包内自带的默认配置（只读没关系）
const configDir =
  process.env.DP_CONFIG_DIR ||
  (fs.existsSync(path.join(runtimeDir, 'config.json')) ? runtimeDir : appDir);
const dataDir = process.env.DP_DATA_DIR || path.join(runtimeDir, 'data');
fs.mkdirSync(dataDir, { recursive: true });

const args = process.argv.slice(2);
const hasFlag = (f) => args.includes(f);
const argValue = (f) => {
  const i = args.indexOf(f);
  return i >= 0 ? args[i + 1] : null;
};

// 数字参数必须校验：Number('abc') 是 NaN，直接喂给 listen()/setInterval()
// 会得到一个抛异常或 0ms 空转的进程。范围也一起卡住。
function intArg(flag, min, max, fallback) {
  const raw = argValue(flag);
  if (raw == null) return fallback;
  const n = Number(raw);
  if (!Number.isFinite(n) || n < min || n > max) {
    log(`${flag} 参数无效（${raw}），沿用 ${fallback}`);
    return fallback;
  }
  return Math.floor(n);
}

const cfg = pickConfig(configDir);
if (hasFlag('--port')) cfg.port = intArg('--port', 1, 65535, cfg.port || 8787);
if (hasFlag('--interval')) cfg.intervalMs = intArg('--interval', 200, 600000, cfg.intervalMs || 2000);
// 配置文件里的值同样不可信
if (!Number.isFinite(cfg.port) || cfg.port < 1 || cfg.port > 65535) cfg.port = 8787;
if (!Number.isFinite(cfg.intervalMs) || cfg.intervalMs < 200) cfg.intervalMs = 2000;
if (!Number.isFinite(cfg.quotaIntervalMs) || cfg.quotaIntervalMs < 5000) cfg.quotaIntervalMs = 60000;

// 可选访问令牌：设备端在配网页填同一个值后，会带 X-DP-Token 请求。
// 留空 = 不鉴权（保持对旧固件的兼容）。
const authToken = String(cfg.authToken || process.env.DP_TOKEN || '');

const once = hasFlag('--once');
const mock = hasFlag('--mock');

const quotaFile = path.join(dataDir, 'quota_history.jsonl');
const store = createQuotaStore(quotaFile);

const startedAt = Date.now();

// 被 App 托管时看着父进程：父进程被 kill 掉（不是走菜单退出），本进程会被 init 收养，
// getppid() 变成 1。这时候必须自己退出，否则会留一个孤儿占着 8787，
// 下次启动的 App 只能把它当"外部实例"，就管不动了。
if (process.env.DP_SUPERVISED === '1') {
  const parentPid = process.ppid;
  setInterval(() => {
    if (process.ppid !== parentPid) {
      log('父进程已退出，桥接跟着收工');
      process.exit(0);
    }
  }, 4000).unref();
}

function healthOf(p, pcfg) {
  if (!pcfg) return p.health || 'unknown';
  const warn = Number(pcfg.warnBalance ?? 20);
  const crit = Number(pcfg.criticalBalance ?? 5);
  if (p.quota?.kind === 'balance' && Number.isFinite(p.quota.remaining)) {
    if (p.quota.remaining <= crit) return 'critical';
    if (p.quota.remaining <= warn) return 'warn';
    return 'ok';
  }
  if (p.quota?.kind === 'credit') {
    const used = Number(p.quota.used || 0);
    const budget = Number(p.quota.budget || 1);
    const left = budget > 0 ? 1 - used / budget : 1;
    if (left <= 0.05) return 'critical';
    if (left <= 0.2) return 'warn';
    return 'ok';
  }
  return p.health || 'ok';
}

// 某个适配器整体失败时的占位：只影响它自己，别让设备以为整台电脑离线。
function offlineProvider(id, name) {
  return {
    id,
    name,
    state: 'offline',
    state_since: Date.now(),
    task: { title: '', step: null, total: null, tool: null },
    quota: { kind: 'balance', remaining: null, currency: 'CNY' },
    health: 'unknown',
    source: 'error',
    error: '读取失败（详见电脑端日志）',
  };
}

async function buildStateInner() {
  if (mock) return mockState(startedAt);

  const pc = cfg.providers || {};
  const providers = [];

  // 每个 provider 独立兜底：一个适配器抛错不能把整个 /v1/state 打成 500，
  // 否则设备会判 NO LINK、按策略熄屏——一个坏数据源连累整块屏。
  if (pc.workbuddy?.enabled !== false) {
    try {
      const wb = readWorkBuddy(pc.workbuddy || {}, cfg);
      wb.health = healthOf(wb, pc.workbuddy);
      providers.push(wb);
    } catch (e) {
      log('[workbuddy] 读取失败:', e.message);
      providers.push(offlineProvider('workbuddy', 'WorkBuddy'));
    }
  }

  if (pc.deepseek?.enabled !== false) {
    try {
      const dsh = readDsh(pc.deepseek || {}, cfg);
      const { detail } = deepSeekQuota();
      if (detail) {
        dsh.quota = {
          kind: 'balance',
          remaining: Math.round(detail.total * 100) / 100,
          granted: Math.round(detail.granted * 100) / 100,
          topped_up: Math.round(detail.toppedUp * 100) / 100,
          spent_total: detail.spentTotal != null ? Math.round(detail.spentTotal * 100) / 100 : null,
          currency: detail.currency,
          available: detail.available,
          balance_source: detail.source,
          spent_today: store.spentToday('deepseek'),
          age_s: Math.round((Date.now() - detail.at) / 1000),
        };
        const avg = store.avgDaily('deepseek');
        dsh.quota.avg_daily = Math.round(avg * 100) / 100;
        dsh.quota.burn_days_left = avg > 0 ? Math.round((detail.total / avg) * 10) / 10 : null;
      } else {
        dsh.quota = { kind: 'balance', remaining: null, currency: 'CNY' };
      }
      dsh.health = healthOf(dsh, pc.deepseek);
      providers.push(dsh);
    } catch (e) {
      log('[deepseek] 读取失败:', e.message);
      providers.push(offlineProvider('deepseek', 'DeepSeek'));
    }
  }

  if (pc.kimi?.enabled !== false) {
    try {
      const km = await readKimi(pc.kimi || {}, store);
      km.health = healthOf(km, pc.kimi);
      providers.push(km);
    } catch (e) {
      log('[kimi] 读取失败:', e.message);
      providers.push(offlineProvider('kimi', 'Kimi'));
    }
  }

  // exposeTitle:false 时不下发任务标题。以前这个开关只存在于配置里、
  // 代码从来没读过——同网段请求仍然能拿到完整标题。
  if (cfg.exposeTitle === false) {
    for (const p of providers) {
      if (p.task) p.task.title = '';
    }
  }

  return {
    ts: Date.now(),
    bridge: {
      ver: BRIDGE_VERSION,
      uptime_s: Math.round((Date.now() - startedAt) / 1000),
      host: os.hostname(),
      // 设备没有 NTP 也能显示正确时间：桥接直接把本地时钟格式化好
      localTime: new Date().toTimeString().slice(0, 5),
    },
    // 主机侧状态：设备据此决定要不要变暗/熄屏。
    // idle_s 是距离上次键鼠输入的秒数，-1 表示读不到（设备会当作"在用"）。
    host: {
      idle_s: hostIdleSeconds(),
    },
    providers,
  };
}

// 400ms 合并窗口：设备 2 秒轮询一次，但 App 探针、SSE、调试请求会叠加。
// 没有缓存的话每一发都要重读 sqlite / 扫日志 / spawn pgrep。
let lastState = null;
let lastStateAt = 0;
let stateInflight = null;
let lastErrAt = 0;

async function buildState() {
  const now = Date.now();
  if (lastState && now - lastStateAt < 400) return lastState;
  if (stateInflight) return stateInflight;

  stateInflight = (async () => {
    try {
      const s = await buildStateInner();
      lastState = s;
      lastStateAt = Date.now();
      return s;
    } catch (e) {
      // 最外层兜底：能返回上一份就返回，实在没有就给一个最小可用状态，
      // 保证 /v1/state 永远是 200——设备不该因为桥接内部异常而熄屏。
      if (now - lastErrAt > 10000) {
        lastErrAt = now;
        log('buildState 失败:', e.message);
      }
      if (lastState) return { ...lastState, ts: Date.now(), stale: true };
      return {
        ts: Date.now(),
        bridge: { ver: BRIDGE_VERSION, uptime_s: Math.round((Date.now() - startedAt) / 1000), host: os.hostname(), localTime: new Date().toTimeString().slice(0, 5) },
        host: { idle_s: hostIdleSeconds() },
        providers: [],
        stale: true,
      };
    } finally {
      stateInflight = null;
    }
  })();

  return stateInflight;
}

// ---------- HTTP ----------
const sseClients = new Set();
let lastJson = '';
const MAX_SSE_CLIENTS = 20;

// 配了 authToken 才鉴权；留空保持和旧固件/旧书签的兼容。
// 设备端在配网页填同一个值，会在请求里带 X-DP-Token。
function authorized(req, url) {
  if (!authToken) return true;
  return req.headers['x-dp-token'] === authToken || url.searchParams.get('token') === authToken;
}

function deny(res) {
  res.writeHead(401, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'unauthorized' }));
}

function sseSend(res, event, data) {
  if (res.writableEnded || res.destroyed) return false;
  try {
    res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
    return true;
  } catch {
    return false;
  }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/v1/state' || url.pathname === '/state') {
    if (!authorized(req, url)) return deny(res);
    try {
      const s = await buildState();
      const body = JSON.stringify(s);
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(body);
    } catch (e) {
      // buildState 内部已有兜底，走到这里说明真的很糟：仍然返回 200 + 空状态，
      // 让设备显示「离线」而不是直接判 500。
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ts: Date.now(), providers: [], error: 'internal' }));
      log('/v1/state 异常:', e.message);
    }
    return;
  }
  if (url.pathname === '/v1/stream') {
    if (!authorized(req, url)) return deny(res);
    if (sseClients.size >= MAX_SSE_CLIENTS) {
      res.writeHead(503, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'too many clients' }));
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-store',
      Connection: 'keep-alive',
      'X-Accel-Buffering': 'no',
    });
    // retry 让浏览器/客户端断线后自己重连；注释行同时充当首包心跳
    res.write('retry: 3000\n\n');
    res.write(': deskpulse\n\n');
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (url.pathname === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('ok');
    return;
  }
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'not found' }));
});

// SSE 心跳：15 秒一行注释，防止中间设备把空闲连接掐掉。
// 同时顺手清掉已经断开的客户端，避免 Set 无限增长。
setInterval(() => {
  for (const c of sseClients) {
    if (c.writableEnded || c.destroyed) {
      sseClients.delete(c);
      continue;
    }
    try {
      c.write(': ping\n\n');
    } catch {
      sseClients.delete(c);
    }
  }
}, 15000).unref();

// ---------- UDP 发现：设备广播 DESKPULSE?，桥接回一个地址 ----------
function lanIp() {
  const ifs = os.networkInterfaces();
  const cands = [];
  for (const list of Object.values(ifs)) {
    for (const i of list || []) {
      if (i.family === 'IPv4' && !i.internal) cands.push(i.address);
    }
  }
  return cands.find((a) => a.startsWith('192.168.')) || cands.find((a) => a.startsWith('10.')) || cands[0] || '127.0.0.1';
}

const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
udp.on('message', (msg, rinfo) => {
  if (!msg.toString().startsWith('DESKPULSE?')) return;
  const reply = Buffer.from(JSON.stringify({ host: lanIp(), port: cfg.port, ver: BRIDGE_VERSION }));
  udp.send(reply, rinfo.port, rinfo.address);
});
udp.on('error', (e) => log('UDP error:', e.message));

let slowRunning = false;
async function slowLoop() {
  // 网络差的时候一次刷新可能超过 quotaIntervalMs，串起来会叠加并发请求。
  // 上一轮没跑完就跳过这一轮。
  if (slowRunning) return;
  slowRunning = true;
  const pc = cfg.providers || {};
  try {
    if (pc.deepseek?.enabled !== false) await refreshDeepSeekQuota(pc.deepseek || {}, store);
    if (pc.kimi?.enabled !== false) await refreshKimiQuota(pc.kimi || {}, store);
  } catch (e) {
    log('quota refresh failed:', e.message);
  } finally {
    slowRunning = false;
  }
}

async function fastLoop() {
  const s = await buildState();
  const body = JSON.stringify(s);
  if (body !== lastJson) {
    lastJson = body;
    for (const c of sseClients) {
      try {
        sseSend(c, 'state', s);
      } catch {
        sseClients.delete(c);
      }
    }
  }
}

if (once) {
  await slowLoop();
  const s = await buildState();
  process.stdout.write(JSON.stringify(s, null, 2) + '\n');
  process.exit(0);
}

udp.bind(8788, () => log('UDP 发现已监听 :8788'));

server.listen(cfg.port, cfg.bind || '0.0.0.0', () => {
  log(`AI余光 bridge 已启动  http://${lanIp()}:${cfg.port}/v1/state`);
  log(`采样: 状态 ${cfg.intervalMs}ms / 额度 ${cfg.quotaIntervalMs}ms`);
});

slowLoop();
fastLoop().catch((e) => log('loop error:', e.message));
setInterval(slowLoop, cfg.quotaIntervalMs || 60000);
setInterval(() => {
  fastLoop().catch((e) => log('loop error:', e.message));
}, cfg.intervalMs || 2000);

// 常驻进程不该因为一次未捕获异常就消失（App 会每 2 秒拉起一次，日志里只剩崩溃循环）。
// 记下来、继续跑；真正的 bug 仍然能在日志里看到。
process.on('unhandledRejection', (e) => log('unhandledRejection:', e?.message || String(e)));
process.on('uncaughtException', (e) => log('uncaughtException:', e?.stack || e?.message || String(e)));

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    log('bye');
    process.exit(0);
  });
}
