// 从 WorkBuddy 的审计日志里判断「现在是不是在等你确认」。
//
// 一次权限弹框在 ~/.workbuddy/audit-log/YYYY-MM-DD.jsonl 里留下两条记录，
// 靠 toolCallId 配对：
//
//   file-safety.needs-approval        ← 弹框出现（decision = info）
//   file-safety.approved / .rejected  ← 你点了允许 / 拒绝
//
// 所以「有 needs-approval、但没有同 id 的 approved/rejected」= 此刻正在等你。
// 实测样例（2026-11-07）：20:11:03 needs-approval → 20:11:15 approved，
// 中间 12 秒就是人读弹框、再点允许的时间。
//
// 这里只做解析，不碰数据库，所以可以在没有 WorkBuddy 的机器上单测
// （见 tests/approval.mjs）。
import fs from 'node:fs';
import path from 'node:path';

// 除了批准/拒绝，取消、超时、过期、中止也都意味着「不用再等了」。
// 少列一种，那张卡片就会白闪到 awaitExpireMs（默认 5 分钟）才消失。
const RESOLVED_SUFFIXES = [
  '.approved',
  '.rejected',
  '.denied',
  '.cancelled',
  '.canceled',
  '.timeout',
  '.timed-out',
  '.timed_out',
  '.expired',
  '.aborted',
];

function dayKey(d = new Date()) {
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(
    d.getDate()
  ).padStart(2, '0')}`;
}

/**
 * 建一个增量读取器。getDir(cfg) 返回审计日志目录的绝对路径。
 * 每个 provider 用一个实例即可，内部按「日期 + 字节 offset」做增量。
 */
export function createApprovalWatcher(getDir) {
  const st = { key: '', file: '', offset: 0, pending: new Map(), seq: 0 };

  function consume(cfg) {
    const key = dayKey();
    const file = path.join(getDir(cfg), `${key}.jsonl`);

    // 跨天要换文件；昨天的框今天不该还闪，所以连 pending 一起清
    if (st.key !== key || st.file !== file) {
      st.key = key;
      st.file = file;
      st.offset = 0;
      st.pending.clear();
    }

    let size;
    try {
      size = fs.statSync(file).size;
    } catch {
      return; // 今天还没有任何审计记录
    }
    // 文件被截断或重建（日志轮转）
    if (size < st.offset) {
      st.offset = 0;
      st.pending.clear();
    }
    if (size === st.offset) return;

    const len = size - st.offset;
    const buf = Buffer.alloc(len);
    const fd = fs.openSync(file, 'r');
    try {
      fs.readSync(fd, buf, 0, len, st.offset);
    } finally {
      fs.closeSync(fd);
    }

    // 只吃到最后一个换行：末尾可能是正在写的半行，留给下一次。
    // 不这么处理的话，一次半行读取会把后面的 offset 也带偏。
    const nl = buf.lastIndexOf(0x0a);
    if (nl < 0) return;
    st.offset += nl + 1;

    for (const line of buf.toString('utf8', 0, nl).split('\n')) {
      if (!line) continue;
      let o;
      try {
        o = JSON.parse(line);
      } catch {
        continue;
      }
      const t = String(o.eventType || '');
      const id = o.toolCallId;
      if (!id) continue;
      if (t.endsWith('.needs-approval')) {
        // seq 用来打破平局：两个事件可能落在同一毫秒，只比 timestamp 会留住先来的那个，
        // 而"最新"应该是后写入的。单测里就撞到过这个。
        st.pending.set(id, { at: Number(o.timestamp) || Date.now(), seq: ++st.seq, kind: t });
      } else if (RESOLVED_SUFFIXES.some((s) => t.endsWith(s))) {
        st.pending.delete(id);
      }
    }
  }

  return {
    /**
     * 返回当前仍在等的那一条（取最新），没有则 null。
     * expireMs：超过这么久还没有 resolution 就当它过期——弹框可能因为你
     * 关了窗口而永远等不到结果，不设上限的话卡片会一直闪下去。
     */
    poll(cfg, expireMs = 300000) {
      consume(cfg);
      const now = Date.now();
      for (const [id, v] of st.pending) {
        if (now - v.at > expireMs) st.pending.delete(id);
      }
      let newest = null;
      for (const v of st.pending.values()) {
        const newer =
          !newest || v.at > newest.at || (v.at === newest.at && v.seq > newest.seq);
        if (newer) newest = v;
      }
      return newest;
    },
  };
}
