// Kimi 桌面版的本地会话 + 额度。
//
// 结论（实测得出，不是猜的）：
//   1) Kimi 桌面版把登录态放在 Chromium 的 Local Storage（LevelDB），值没加密；
//      token-store.json 反而是 Electron safeStorage 的密文，走 Keychain，不碰它。
//   2) access_token 只有 15 分钟，但 refresh_token 有 90 天，
//      拿 refresh_token 打 GET /api/auth/token/refresh（Bearer）能换到 30 天的新 access_token。
//      —— 所以完全不需要用户填 MOONSHOT_API_KEY，也不需要 app 一直开着。
//   3) 额度在 POST /api/user/usage：每日深度研究次数、图像生成次数、会员等级。
//      这是 Kimi 会员的"剩余额度"语义（它是订阅制，没有 ¥ 余额）。
import os from 'node:os';
import path from 'node:path';
import { readLocalStorage, getLocalStorage, decodeJwtPayload } from './leveldb-log.mjs';

export const KIMI_ORIGIN = 'https://www.kimi.com';
export const DEFAULT_APP_DATA = path.join(os.homedir(), 'Library', 'Application Support', 'kimi-desktop');

/** 从 Local Storage 里读出 Kimi 的登录态；读不到返回 null */
export function readKimiSession(appDataDir = DEFAULT_APP_DATA) {
  const dir = path.join(appDataDir, 'Local Storage', 'leveldb');
  const map = readLocalStorage(dir);
  if (map.size === 0) return null;
  const access = getLocalStorage(map, KIMI_ORIGIN, 'access_token');
  const refresh = getLocalStorage(map, KIMI_ORIGIN, 'refresh_token');
  if (!refresh) return null;
  const claims = decodeJwtPayload(refresh) || {};
  return {
    access,
    refresh,
    accessExp: decodeJwtPayload(access)?.exp || 0,
    refreshExp: claims.exp || 0,
    userId: claims.sub || null,
    membershipLevel: claims.membership?.level ?? claims.code_membership?.level ?? null,
  };
}

/** 用 refresh_token 换一个新的 access_token */
export async function refreshAccessToken(refreshToken, timeoutMs = 8000) {
  const res = await fetch(`${KIMI_ORIGIN}/api/auth/token/refresh`, {
    method: 'GET',
    headers: {
      Authorization: `Bearer ${refreshToken}`,
      Accept: 'application/json',
    },
    signal: AbortSignal.timeout(timeoutMs),
  });
  if (!res.ok) throw new Error(`token/refresh HTTP ${res.status}`);
  const body = await res.json();
  const at = body?.access_token;
  if (!at) throw new Error('token/refresh 未返回 access_token');
  return at;
}

/** 查每日额度 */
export async function fetchKimiUsage(accessToken, timeoutMs = 8000) {
  const res = await fetch(`${KIMI_ORIGIN}/api/user/usage`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${accessToken}`,
      'Content-Type': 'application/json',
      Accept: 'application/json',
    },
    body: '{}',
    signal: AbortSignal.timeout(timeoutMs),
  });
  if (res.status === 401) {
    const e = new Error('usage HTTP 401');
    e.unauthorized = true;
    throw e;
  }
  if (!res.ok) throw new Error(`usage HTTP ${res.status}`);
  const b = await res.json();
  const pick = (o) =>
    o && Number.isFinite(Number(o.total)) && Number(o.total) > 0
      ? { used: Number(o.used) || 0, total: Number(o.total), remain: Number(o.remain), date: o.date || null }
      : null;
  return {
    research: pick(b.research_usage),
    image: pick(b.image_gen),
    anonymous: pick(b.anonymous_chat),
    hasMembership: !!b.membership?.has_permission,
  };
}

/**
 * 一站式取额度，带内存缓存。token 快过期就自动换。
 * 返回 { usage, membershipLevel, userId, refreshedAt, error }
 */
export function createKimiQuota(cfg = {}) {
  const appData = cfg.appDataPath ? cfg.appDataPath.replace(/^~/, os.homedir()) : DEFAULT_APP_DATA;
  let cache = { access: null, exp: 0, refresh: null };
  let state = { usage: null, membershipLevel: null, userId: null, refreshedAt: 0, error: null };

  return {
    get last() {
      return state;
    },
    async refresh(force = false) {
      try {
        // 每次都重读 localStorage：桌面端自己会续期，跟着它走最省事
        const s = readKimiSession(appData);
        if (s) {
          cache = { access: s.access, exp: s.accessExp, refresh: s.refresh };
          state.membershipLevel = s.membershipLevel;
          state.userId = s.userId;
        }
        if (!cache.refresh) {
          state.error = '未找到 Kimi 登录态（桌面端需登录过一次）';
          return state;
        }

        const now = Math.floor(Date.now() / 1000);
        const stale = force || !cache.access || (cache.exp && cache.exp - now < 120);
        if (stale) {
          const fresh = await refreshAccessToken(cache.refresh);
          const claims = decodeJwtPayload(fresh) || {};
          cache.access = fresh;
          cache.exp = claims.exp || now + 1800;
          if (claims.membership?.level != null) state.membershipLevel = claims.membership.level;
        }

        try {
          state.usage = await fetchKimiUsage(cache.access);
        } catch (e) {
          if (!e.unauthorized) throw e;
          // access 被提前作废：强刷一次再试
          const fresh = await refreshAccessToken(cache.refresh);
          cache.access = fresh;
          cache.exp = decodeJwtPayload(fresh)?.exp || Math.floor(Date.now() / 1000) + 1800;
          state.usage = await fetchKimiUsage(cache.access);
        }
        state.refreshedAt = Date.now();
        state.error = null;
      } catch (e) {
        state.error = e.message;
      }
      return state;
    },
  };
}
