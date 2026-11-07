#!/usr/bin/env node
// AI余光 桥接：把三家的状态与额度聚合成一份 JSON，给 ESP32 拉取。
// 设计约束（来自 PRD）：零第三方依赖、只读本地数据、密钥不出电脑。
import http from 'node:http';
import dgram from 'node:dgram';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { pickConfig, log } from './lib/util.mjs';
import { createQuotaStore } from './lib/quota.mjs';
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

const cfg = pickConfig(configDir);
if (hasFlag('--port')) cfg.port = Number(argValue('--port'));
if (hasFlag('--interval')) cfg.intervalMs = Number(argValue('--interval'));
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

async function buildState() {
  if (mock) return mockState(startedAt);

  const pc = cfg.providers || {};
  const providers = [];

  if (pc.workbuddy?.enabled !== false) {
    const wb = readWorkBuddy(pc.workbuddy || {}, cfg);
    wb.health = healthOf(wb, pc.workbuddy);
    providers.push(wb);
  }

  if (pc.deepseek?.enabled !== false) {
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
  }

  if (pc.kimi?.enabled !== false) {
    const km = await readKimi(pc.kimi || {}, store);
    km.health = healthOf(km, pc.kimi);
    providers.push(km);
  }

  return {
    ts: Date.now(),
    bridge: {
      ver: '0.1.1',
      uptime_s: Math.round((Date.now() - startedAt) / 1000),
      host: os.hostname(),
      // 设备没有 NTP 也能显示正确时间：桥接直接把本地时钟格式化好
      localTime: new Date().toTimeString().slice(0, 5),
    },
    providers,
  };
}

// ---------- HTTP ----------
const sseClients = new Set();
let lastJson = '';

function sseSend(res, event, data) {
  res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/v1/state' || url.pathname === '/state') {
    try {
      const s = await buildState();
      const body = JSON.stringify(s);
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(body);
    } catch (e) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: e.message }));
    }
    return;
  }
  if (url.pathname === '/v1/stream') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-store',
      Connection: 'keep-alive',
    });
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
  const reply = Buffer.from(JSON.stringify({ host: lanIp(), port: cfg.port, ver: '0.1.1' }));
  udp.send(reply, rinfo.port, rinfo.address);
});
udp.on('error', (e) => log('UDP error:', e.message));

async function slowLoop() {
  const pc = cfg.providers || {};
  try {
    if (pc.deepseek?.enabled !== false) await refreshDeepSeekQuota(pc.deepseek || {}, store);
    if (pc.kimi?.enabled !== false) await refreshKimiQuota(pc.kimi || {}, store);
  } catch (e) {
    log('quota refresh failed:', e.message);
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
fastLoop();
setInterval(slowLoop, cfg.quotaIntervalMs || 60000);
setInterval(() => {
  fastLoop().catch((e) => log('loop error:', e.message));
}, cfg.intervalMs || 2000);

process.on('SIGINT', () => {
  log('bye');
  process.exit(0);
});
