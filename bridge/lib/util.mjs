import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const now = () => Date.now();

export function expandHome(p) {
  if (!p) return p;
  return p.startsWith('~') ? path.join(os.homedir(), p.slice(1)) : p;
}

export function readJson(p, fallback = null) {
  try {
    return JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch {
    return fallback;
  }
}

export function pickConfig(baseDir) {
  const base = readJson(path.join(baseDir, 'config.json'), {});
  const local = readJson(path.join(baseDir, 'config.local.json'), null);
  if (!local) return base;
  return {
    ...base,
    ...local,
    providers: { ...(base.providers || {}), ...(local.providers || {}) },
  };
}

export function secretOf(providerCfg, envName) {
  if (envName && process.env[envName]) return process.env[envName];
  return providerCfg?.apiKey || '';
}

export function truncateChars(s, n) {
  if (!s) return '';
  const t = String(s).replace(/\s+/g, ' ').trim();
  const arr = Array.from(t);
  return arr.length <= n ? t : arr.slice(0, n - 1).join('') + '…';
}

export function clamp(n, lo, hi) {
  return Math.max(lo, Math.min(hi, n));
}

/** 把 ms 输出成 “2m14s” 这类紧凑形式，屏幕只有 240px 宽 */
export function humanDuration(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m${String(s % 60).padStart(2, '0')}s`;
  const h = Math.floor(m / 60);
  return `${h}h${String(m % 60).padStart(2, '0')}m`;
}

export function log(...args) {
  const ts = new Date().toISOString().slice(11, 19);
  console.log(`[${ts}]`, ...args);
}
