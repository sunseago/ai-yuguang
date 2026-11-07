// 余额采样历史：用来算「今日消耗」和「按近 7 日均耗还能撑几天」。
// 余额接口只给瞬时值，日耗必须靠本地采样差分得出。
import fs from 'node:fs';
import path from 'node:path';

export function createQuotaStore(file) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  let entries = [];

  function load() {
    entries = [];
    try {
      const raw = fs.readFileSync(file, 'utf8');
      const cutoff = Date.now() - 30 * 86400_000;
      for (const line of raw.split('\n')) {
        if (!line) continue;
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

  return { load, append, spentToday, avgDaily, seriesFor, get size() { return entries.length; } };
}
