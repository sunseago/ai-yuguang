// DeepSeek 余额。
//
// 优先级：
//   1) 本地登录态（推荐，零配置）——读 ~/.dsh/.credentials.yaml 里的 Platform grant，
//      打 /api/v0/users/get_user_summary，拿到的就是桌面端显示的同一个钱包余额；
//   2) 官方 API Key（保留兼容）——GET /user/balance，需要 DEEPSEEK_API_KEY。
//
// 两条路都失败时把错误抛给上层，由上层决定显示 -- 还是沿用上一份缓存。
import { secretOf, expandHome } from './util.mjs';
import { readDshGrant, fetchDshSummary, sumWallets, DEFAULT_CRED_PATH } from './dsh-account.mjs';

let detail = null;
let lastError = null;

/** 本地账户（桌面版登录态） */
async function fromLocalAccount(cfg) {
  const file = expandHome(cfg.credentialsPath) || DEFAULT_CRED_PATH;
  const grant = readDshGrant(file);
  if (!grant) throw new Error(`未找到 dsh 登录态（${file}）`);
  const s = await fetchDshSummary(grant);
  const currency = s.wallets[0]?.currency || s.bonusWallets[0]?.currency || 'CNY';
  const cash = sumWallets(s.wallets, currency); // 充值钱包
  const bonus = sumWallets(s.bonusWallets, currency); // 赠送钱包
  const spentTotal = (s.totalCosts || [])
    .filter((c) => c.currency === currency)
    .reduce((n, c) => n + (Number(c.amount) || 0), 0);
  return {
    total: cash + bonus,
    granted: bonus,
    toppedUp: cash,
    currency,
    spentTotal,
    available: true,
    source: 'dsh-local',
    at: Date.now(),
  };
}

/** 官方开放平台 API Key */
async function fromApiKey(cfg) {
  const key = secretOf(cfg, cfg.apiKeyEnv);
  if (!key) throw new Error('未配置 DEEPSEEK_API_KEY');
  const res = await fetch('https://api.deepseek.com/user/balance', {
    headers: { Authorization: `Bearer ${key}`, Accept: 'application/json' },
    signal: AbortSignal.timeout(8000),
  });
  if (!res.ok) throw new Error(`balance HTTP ${res.status}`);
  const body = await res.json();
  const info =
    (body.balance_infos || []).find((b) => b.currency === 'CNY') || (body.balance_infos || [])[0];
  if (!info) throw new Error('balance_infos 为空');
  return {
    total: Number(info.total_balance),
    granted: Number(info.granted_balance ?? 0),
    toppedUp: Number(info.topped_up_balance ?? 0),
    currency: info.currency || 'CNY',
    spentTotal: null,
    available: body.is_available !== false,
    source: 'api-key',
    at: Date.now(),
  };
}

export async function refreshDeepSeekQuota(cfg = {}, store) {
  const tries = cfg.localAccount === false
    ? [fromApiKey]
    : [fromLocalAccount, fromApiKey];

  const errors = [];
  for (const fn of tries) {
    try {
      const d = await fn(cfg);
      if (!Number.isFinite(d.total)) throw new Error('余额字段不是数字');
      detail = d;
      lastError = null;
      if (store) store.append('deepseek', d.total);
      return;
    } catch (e) {
      errors.push(e.message);
    }
  }
  lastError = errors.join(' / ');
}

export function deepSeekQuota() {
  return { detail, lastError };
}
