// 余额采样历史：用来算「今日消耗」和「按近 7 日均耗还能撑几天」。
// 余额接口只给瞬时值，日耗必须靠本地采样差分得出。
import fs from 'node:fs';
import path from 'node:path';

const KEEP_MS = 30 * 86400_000;
const MAX_BYTES = 1024 * 1024;   // 超过 1MB 就重写一次，别让文件无限长

export function createQuotaStore(file) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  let entries = [];

  function fileSize() {
    try {
      return fs.statSync(file).size;
    } catch {
      return 0;
    }
  }

  // 只保留 30 天，并原子重写（tmp → rename）。
  // 旧实现只在内存里裁剪、文件只追加，跑几个月会变成几十 MB，
  // 每次启动都要全量 parse。
  function compact(force = false) {
    const cutoff = Date.now() - KEEP_MS;
    const kept = entries.filter((e) => e.t >= cutoff);
    const needRewrite = force || kept.length !== entries.length || fileSize() > MAX_BYTES;
    if (!needRewrite) return;
    entries = kept;
    try {
      const tmp = `${file}.tmp`;
      const body = entries.map((e) => JSON.stringify(e)).join('\n');
      fs.writeFileSync(tmp, body ? body + '\n' : '');
      fs.renameSync(tmp, file);
    } catch {
      // 重写失败不影响采样精度，最多是文件继续变大
    }
  }

  function load() {
    entries = [];
    let total = 0;
    try {
      const raw = fs.readFileSync(file, 'utf8');
      const cutoff = Date.now() - KEEP_MS;
      for (const line of raw.split('\n')) {
        if (!line) continue;
        total++;
        try {
          const e = JSON.parse(line);
          if (e.t >= cutoff) entries.push(e);
        } catch {
          /* 跳过坏行 */
        }
      }
    } catch {
      /* 首次运行没有文件 */
    }
    // 启动时顺手把过期行清掉（不用等文件涨到 1MB）
    compact(total !== entries.length);
    return total;
  }
  load();

  function append(id, value) {
    if (!Number.isFinite(value)) return;
    const last = [...entries].reverse().find((e) => e.id === id);
    if (last && Math.abs(last.v - value) < 1e-9) return; // 没变化不写盘
    const e = { t: Date.now(), id, v: Math.round(value * 10000) / 10000 };
    entries.push(e);
    try {
      fs.appendFileSync(file, JSON.stringify(e) + '\n');
    } catch {
      /* 写失败不影响主流程 */
    }
    compact();
  }

  function seriesFor(id) {
    return entries.filter((e) => e.id === id);
  }

  function spentToday(id) {
    const start = new Date();
    start.setHours(0, 0, 0, 0);
    const s = seriesFor(id).filter((e) => e.t >= start.getTime());
    let spent = 0;
    for (let i = 1; i < s.length; i++) {
      const d = s[i - 1].v - s[i].v;
      if (d > 0) spent += d; // 只统计下降（消耗），充值导致的上升忽略
    }
    return Math.round(spent * 100) / 100;
  }

  function avgDaily(id, days = 7) {
    const since = Date.now() - days * 86400_000;
    const s = seriesFor(id).filter((e) => e.t >= since);
    if (s.length < 2) return 0;
    let spent = 0;
    for (let i = 1; i < s.length; i++) {
      const d = s[i - 1].v - s[i].v;
      if (d > 0) spent += d;
    }
    const spanDays = Math.max((s[s.length - 1].t - s[0].t) / 86400_000, 1 / 24);
    return spent / spanDays;
  }

  return { load, append, spentToday, avgDaily, seriesFor, compact, get size() { return entries.length; } };
}
