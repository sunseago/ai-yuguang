// DeepSeek 桌面版（dsh / DeepSeek Harness）的账户凭据。
//
// dsh 登录后会把 Platform 的 grant 落在 ~/.dsh/.credentials.yaml：
//   records:
//     deepseek-account-platform/default:
//       kind: grant
//       payload:
//         version: 1
//         token: <64 字符>
//         issuer: https://platform.deepseek.com
//
// 拿这个 token 打 GET /api/v0/users/get_user_summary（请求头 x-dsh-auth-token）
// 就能读到充值钱包和赠送钱包余额 —— 跟桌面端自己显示的是同一个数字，
// 不需要用户再去开放平台申请 API Key。
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const DEFAULT_CRED_PATH = path.join(os.homedir(), '.dsh', '.credentials.yaml');
export const ACCOUNT_KEY = 'deepseek-account-platform/default';

/** 够用就好的 YAML 子集解析：只支持嵌套 map + 标量，不碰列表/多行 */
export function parseMiniYaml(text) {
  const root = {};
  const stack = [{ indent: -1, node: root }];
  for (const raw of String(text).split(/\r?\n/)) {
    const trimmed = raw.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const indent = raw.length - raw.trimStart().length;
    const m = trimmed.match(/^([^:]+):\s*(.*)$/);
    if (!m) continue;
    const key = m[1].trim();
    let val = m[2].trim();
    while (stack.length > 1 && stack[stack.length - 1].indent >= indent) stack.pop();
    const parent = stack[stack.length - 1].node;
    if (val === '') {
      const child = {};
      parent[key] = child;
      stack.push({ indent, node: child });
    } else {
      if (
        (val.startsWith('"') && val.endsWith('"')) ||
        (val.startsWith("'") && val.endsWith("'"))
      ) {
        val = val.slice(1, -1);
      }
      parent[key] = val;
    }
  }
  return root;
}

/** 读出 Platform grant，没有或格式不对就返回 null */
export function readDshGrant(file = DEFAULT_CRED_PATH) {
  let text;
  try {
    text = fs.readFileSync(file, 'utf8');
  } catch {
    return null;
  }
  const doc = parseMiniYaml(text);
  const rec = doc?.records?.[ACCOUNT_KEY];
  const token = rec?.payload?.token;
  const issuer = rec?.payload?.issuer;
  if (!token || !issuer) return null;
  return { token: String(token), issuer: String(issuer), file };
}

/**
 * 查 Platform 账户摘要。
 * 返回 { wallets, bonusWallets, totalCosts } 的原始字符串数组，数值转换交给调用方。
 */
export async function fetchDshSummary(grant, timeoutMs = 8000) {
  const base = grant.issuer.replace(/\/+$/, '');
  const res = await fetch(`${base}/api/v0/users/get_user_summary`, {
    method: 'GET',
    headers: {
      // dsh 就是这么带的：不用 Authorization
      'x-dsh-auth-token': grant.token,
      Accept: 'application/json',
    },
    signal: AbortSignal.timeout(timeoutMs),
  });
  if (!res.ok) throw new Error(`get_user_summary HTTP ${res.status}`);
  const body = await res.json();
  if (body?.code !== 0) throw new Error(`get_user_summary code=${body?.code} ${body?.msg || ''}`);
  const d = body?.data?.biz_data || {};
  return {
    wallets: d.normal_wallets || [],
    bonusWallets: d.bonus_wallets || [],
    totalCosts: d.total_costs || [],
  };
}

export function sumWallets(list, currency) {
  const pick = (list || []).filter((w) => !currency || w.currency === currency);
  return pick.reduce((s, w) => s + (Number(w.balance) || 0), 0);
}
