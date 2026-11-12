// Kimi 适配器
//
// 额度：两条路，默认走本地（零配置）
//   a) 本地登录态 —— 从桌面版 Local Storage 拿 refresh_token，
//      换 access_token 后打 /api/user/usage，得到每日「深度研究 / 图像生成」次数，
//      这是 Kimi 会员真正意义上的"剩余额度"（订阅制，没有 ¥ 余额）；
//   b) Moonshot 开放平台余额 —— 需要 MOONSHOT_API_KEY，仅作兼容保留。
//
// 进度：桌面版是 Electron，没有明文会话日志。可用的是 kimi-agent/ 下的
//   conversation-statuses.json（会话状态）与 conversation-context-usage.json
//   （模型 + 上下文占用），所以用「进程存活 + 会话状态 + 上下文用量」拼出进度。
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { secretOf, readJson } from '../lib/util.mjs';
import { createKimiQuota, DEFAULT_APP_DATA } from '../lib/kimi-session.mjs';
import { createWireReader } from '../lib/kimi-wire.mjs';

let apiDetail = null; // 开放平台余额（仅当走 API Key 时）
let lastError = null;

const localStorageQuota = createKimiQuota(readLocalCfg());
// wire 事件流读取器按 appData 目录缓存，避免每次轮询重新枚举会话目录
const wireReaders = new Map();
function wireReaderFor(dir) {
  if (!wireReaders.has(dir)) wireReaders.set(dir, createWireReader(dir));
  return wireReaders.get(dir);
}
function readLocalCfg() {
  return { appDataPath: DEFAULT_APP_DATA };
}

function appDataDir(cfg) {
  const p = cfg.appDataPath || DEFAULT_APP_DATA;
  return p.startsWith('~') ? path.join(os.homedir(), p.slice(1)) : p;
}

/** 开放平台余额（兼容路径） */
export async function refreshKimiQuota(cfg = {}, store) {
  const local = cfg.localSession !== false;
  const key = secretOf(cfg, cfg.apiKeyEnv);

  if (local) {
    const st = await localStorageQuota.refresh();
    if (st.usage) {
      lastError = null;
      const q = st.usage.research || st.usage.image;
      if (q && store) store.append('kimi', q.remain);
      return;
    }
    lastError = st.error;
  }

  if (!key) {
    if (!local) lastError = '未配置 MOONSHOT_API_KEY';
    return;
  }
  try {
    const base = (cfg.baseUrl || 'https://api.moonshot.cn').replace(/\/+$/, '');
    const res = await fetch(`${base}/v1/users/me/balance`, {
      headers: { Authorization: `Bearer ${key}`, Accept: 'application/json' },
      signal: AbortSignal.timeout(8000),
    });
    if (!res.ok) throw new Error(`balance HTTP ${res.status}`);
    const body = await res.json();
    const d = body?.data || {};
    const available = Number(d.available_balance);
    if (!Number.isFinite(available)) throw new Error('balance 字段缺失');
    apiDetail = {
      available,
      cash: Number(d.cash_balance ?? 0),
      voucher: Number(d.voucher_balance ?? 0),
      at: Date.now(),
    };
    lastError = null;
    if (store) store.append('kimi', available);
  } catch (e) {
    lastError = e.message;
  }
}

// pgrep 结果缓存 5 秒：设备每 2 秒拉一次 /v1/state，加上 App 探针，
// 不缓存等于每两秒 fork 好几次进程，纯浪费。进程是否存活不需要秒级精度。
const aliveCache = { at: 0, value: false, pending: null };

function processAlive(pattern) {
  const now = Date.now();
  if (now - aliveCache.at < 5000) return Promise.resolve(aliveCache.value);
  if (aliveCache.pending) return aliveCache.pending;

  aliveCache.at = now;  // 先占位，避免并发请求各起一个 pgrep
  aliveCache.pending = new Promise((resolve) => {
    execFile('pgrep', ['-f', pattern], { timeout: 2000 }, (err, stdout) => {
      aliveCache.value = !err && String(stdout).trim().length > 0;
      aliveCache.at = Date.now();
      aliveCache.pending = null;
      resolve(aliveCache.value);
    });
  });
  return aliveCache.pending;
}

const ACTIVE_STATES = new Set(['running', 'working', 'in_progress', 'thinking', 'streaming']);

/** 读 kimi-agent 下的会话状态与上下文用量，取最近活跃的那条 */
function readAgentSnapshot(cfg) {
  const dir = path.join(appDataDir(cfg), 'kimi-agent');
  const statuses = readJson(path.join(dir, 'conversation-statuses.json'), {}) || {};
  const usage = readJson(path.join(dir, 'conversation-context-usage.json'), {}) || {};

  let latest = null;
  for (const [id, u] of Object.entries(usage)) {
    if (!u || !u.updatedAt) continue;
    if (!latest || u.updatedAt > latest.updatedAt) latest = { id, ...u };
  }

  const active = Object.entries(statuses).filter(([, s]) => ACTIVE_STATES.has(String(s)));
  // 多个会话同时活动时，挑 updatedAt 最新的那个去对 wire 事件流
  let activeKey = null;
  let activeAt = -Infinity;
  for (const [id] of active) {
    const t = Date.parse(usage[id]?.updatedAt || '') || 0;
    if (t >= activeAt) { activeAt = t; activeKey = id; }
  }
  if (!activeKey && active.length) activeKey = active[active.length - 1][0];
  return {
    activeKey,
    contextTokens: latest?.contextTokens ?? null,
    maxContextTokens: latest?.maxContextTokens ?? null,
    model: latest?.model ?? null,
    updatedAt: latest?.updatedAt ?? null,
    activeCount: active.length,
  };
}

export async function readKimi(cfg = {}, store) {
  const provider = {
    id: 'kimi',
    name: 'Kimi',
    state: 'idle',
    state_since: Date.now(),
    task: { title: '', step: null, total: null, tool: null },
    quota: { kind: 'balance', remaining: null, currency: 'CNY' },
    health: 'unknown',
    source: 'local:kimi-desktop',
  };

  const alive = await processAlive(cfg.processPattern || 'Kimi');
  const snap = readAgentSnapshot(cfg);
  const st = localStorageQuota.last;
  const useLocal = cfg.localSession !== false;

  if (useLocal && st.usage) {
    const q = st.usage[cfg.quotaSource === 'image' ? 'image' : 'research'] || st.usage.image;
    if (q) {
      const used = q.used;
      const budget = q.total;
      provider.quota = {
        kind: 'credit',
        used,
        budget,
        remaining: q.remain,
        unit: '次',
        label: cfg.quotaSource === 'image' ? '今日图像' : '今日研究',
        // 固件不读这些字段，但 /v1/state 的调用方（调试、网页）用得上
        detail: {
          research: st.usage.research,
          image: st.usage.image,
          plan_level: st.membershipLevel,
          date: q.date,
        },
        age_s: st.refreshedAt ? Math.round((Date.now() - st.refreshedAt) / 1000) : null,
      };
      const avg = store ? store.avgDaily('kimi') : 0;
      provider.quota.avg_daily = Math.round(avg * 100) / 100;
      provider.health = 'ok';
    } else {
      provider.health = 'unknown';
    }
    provider.source = 'local:kimi-desktop/usage';
  } else if (apiDetail) {
    provider.quota = {
      kind: 'balance',
      remaining: Math.round(apiDetail.available * 100) / 100,
      cash: Math.round(apiDetail.cash * 100) / 100,
      voucher: Math.round(apiDetail.voucher * 100) / 100,
      currency: 'CNY',
      age_s: Math.round((Date.now() - apiDetail.at) / 1000),
    };
    provider.health = 'ok';
    provider.source = 'api:moonshot/balance';
  } else {
    provider.health = 'unknown';
    provider.quota = { kind: 'balance', remaining: null, currency: 'CNY' };
    if (lastError) provider.error = lastError;
  }

  // 上下文占用：有值就带给上层（固件暂未渲染，网页/调试可用）
  if (snap.contextTokens != null) {
    provider.context = {
      used: snap.contextTokens,
      size: snap.maxContextTokens,
      pct: snap.maxContextTokens
        ? Math.round((snap.contextTokens / snap.maxContextTokens) * 1000) / 10
        : null,
      model: snap.model,
      updated_at: snap.updatedAt,
    };
  }

  // ---- 弹窗 / 完成：从 wire.jsonl 的事件配对里读，比会话状态表准 ----
  const wire = wireReaderFor(appDataDir(cfg)).sample(snap.activeKey);
  const sameConv = !snap.activeKey || !wire.key || wire.key === snap.activeKey;
  const pending = sameConv ? wire.pending : null;
  const justEnded = wire.lastTurnEnded && Date.now() - wire.lastTurnEnded < 10000;

  if (pending) {
    provider.state = 'awaiting';
    provider.state_since = pending.time || Date.now();
    provider.task.tool = pending.toolName || (pending.kind === 'approval' ? 'approval' : 'kimi');
    // 详情页能看到"在等什么"，比一句「会话进行中」有用得多
    provider.task.title = pending.description || pending.action || '等待你确认';
  } else if (justEnded) {
    provider.state = 'done';
    provider.state_since = wire.lastTurnEnded;
    provider.task.title = '任务已完成';
  } else if (snap.activeCount > 0) {
    provider.state = 'thinking';
    provider.state_since = snap.updatedAt ? Date.parse(snap.updatedAt) : Date.now();
    provider.task.tool = snap.model || 'kimi';
    provider.task.title = '会话进行中';
  } else if (alive) {
    provider.task.tool = snap.model || 'kimi-desktop';
    // 标题只放会员等级：卡片那行还要拼状态前缀，太长会被截成「图像 2..」
    provider.task.title = st.membershipLevel != null ? `Lv.${st.membershipLevel}` : '桌面端已打开';
  }

  // 把弹窗细节带到 /v1/state（固件暂不渲染，调试和网页能看）
  if (pending) {
    provider.pending = {
      id: pending.id,
      kind: pending.kind,
      tool: pending.toolName,
      command: pending.action,
      description: pending.description,
    };
  }

  return provider;
}

export function kimiError() {
  return lastError;
}
