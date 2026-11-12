// AI余光 WorkBuddy 适配器：只读打开 workbuddy.db，取「最新会话状态 + 上下文占用 + credit 记账」
// 注意：官方没有「剩余额度」接口，这里的额度口径是「本期已用 credit / 用户设定预算」。
import { DatabaseSync } from 'node:sqlite';
import { createApprovalWatcher } from '../lib/approval.mjs';
import { workBuddyCredits, workBuddyCreditsError } from '../lib/workbuddy-credits.mjs';
import { expandHome, truncateChars } from '../lib/util.mjs';

let db = null;
let dbPath = null;
let lastError = null;

// ---------- 权限弹框 / 提问检测 ----------
// JSONL 的解析、增量 offset、半行处理都在 lib/approval.mjs 里，纯文件操作、可单测。
// 这里只负责两件事：告诉它审计日志在哪、把「正在等」翻译成 awaiting 状态。
const approvals = createApprovalWatcher((cfg) =>
  expandHome(cfg.auditLogDir || '~/.workbuddy/audit-log')
);

function open(cfg) {
  const p = expandHome(cfg.dbPath);
  if (db && dbPath === p) return db;
  close();
  dbPath = p;
  db = new DatabaseSync(p, { readOnly: true });
  try {
    db.exec('PRAGMA busy_timeout = 2000');
  } catch {
    /* 只读句柄可能不允许 PRAGMA，忽略 */
  }
  return db;
}

function close() {
  if (db) {
    try {
      db.close();
    } catch {
      /* ignore */
    }
  }
  db = null;
}

function periodStart(period) {
  const d = new Date();
  if (period === 'day') {
    d.setHours(0, 0, 0, 0);
  } else if (period === 'week') {
    d.setHours(0, 0, 0, 0);
    d.setDate(d.getDate() - ((d.getDay() + 6) % 7));
  } else {
    d.setDate(1);
    d.setHours(0, 0, 0, 0);
  }
  return d.getTime();
}

function sumCredits(handle, sinceMs) {
  const rows = handle
    .prepare(
      `SELECT u.credit_json AS cj
         FROM session_usage u
         JOIN sessions s ON s.id = u.session_id
        WHERE u.credit_json IS NOT NULL
          AND COALESCE(s.last_activity_at, s.updated_at) >= ?`
    )
    .all(sinceMs);
  let total = 0;
  for (const r of rows) {
    try {
      const o = JSON.parse(r.cj);
      for (const v of Object.values(o)) {
        const n = Number(v);
        if (Number.isFinite(n)) total += n;
      }
    } catch {
      /* 跳过坏数据 */
    }
  }
  return total;
}

/**
 * 从按 fresh_at 降序的会话列表里挑一张上卡片的会话。
 * 优先「还活着」的（状态在进行中、且按 fresh_at 看没超时），其次才是最近写库的。
 * 抽成纯函数是为了能离线单测 —— 真库要靠 WorkBuddy 在写，测不出并行场景。
 * @returns 选中的行，rows 为空返回 null。
 */
export function pickLiveSession(rows, idleAfterMs, nowMs) {
  if (!rows || !rows.length) return null;
  for (const r of rows) {
    const st = String(r.status || '').toLowerCase();
    const fresh = nowMs - Number(r.fresh_at || 0);
    if ((st === 'working' || st === 'running' || st === 'pending') && fresh <= idleAfterMs) {
      return r;
    }
  }
  return rows[0];
}

export function readWorkBuddy(cfg, opts) {
  const provider = {
    id: 'workbuddy',
    name: 'WorkBuddy',
    state: 'offline',
    state_since: Date.now(),
    task: { title: '', step: null, total: null, tool: null },
    context: { used: 0, size: 0 },
    quota: { kind: 'credit', used: 0, budget: cfg.budgetCredits ?? 100, unit: 'credit' },
    health: 'unknown',
    source: 'local:workbuddy.db',
  };

  try {
    const handle = open(cfg);
    // 审批检测放在最前面：即使库里一条会话都没有（刚清空历史/换账号），
    // 也不能漏掉此刻正在等的弹框 —— 旧实现会在这里提前 return 跳过检测。
    const appr = approvals.poll(cfg, opts?.awaitExpireMs ?? 300000);

    // fresh_at = max(last_activity_at, updated_at)：WorkBuddy 跑工具的过程中
    // 只更新 updated_at，不动 last_activity_at —— 只看后者会把长工具调用
    // 误判成「5 分钟没活动 = idle」，卡片明明在跑却掉回灰色。
    const rows = handle
      .prepare(
        `SELECT s.id, COALESCE(NULLIF(s.custom_title, ''), s.title) AS title,
                s.status, s.model, s.context_window,
                COALESCE(s.last_activity_at, s.updated_at) AS last_at,
                MAX(COALESCE(s.last_activity_at, 0), COALESCE(s.updated_at, 0)) AS fresh_at
           FROM sessions s
          WHERE s.deleted_at IS NULL OR s.deleted_at = 0
          ORDER BY fresh_at DESC
          LIMIT 5`
      )
      .all();

    if (!rows.length) {
      provider.state = appr ? 'awaiting' : 'idle';
      if (appr) provider.state_since = appr.at;
      provider.health = 'ok';
      return provider;
    }

    const idleAfterMs = opts?.idleAfterMs ?? 300000;
    const row = pickLiveSession(rows, idleAfterMs, Date.now());

    const usage = handle
      .prepare('SELECT used, size, updated_at FROM session_usage WHERE session_id = ?')
      .get(row.id);

    const ageMs = Date.now() - Number(row.fresh_at || 0);
    const st = String(row.status || '').toLowerCase();

    if (st === 'working' || st === 'running' || st === 'pending') {
      // 会话状态写着"进行中"，但很久没有新活动了 —— 多半是 WorkBuddy 被关掉、
      // 或者任务中途断了。不降级的话板子会永远卡在 THINKING 上（红标签 + 耗时一直涨），
      // 看着像还在跑。规则和 dsh.mjs 保持一致。
      provider.state = ageMs > idleAfterMs ? 'idle' : 'thinking';
    } else if (st === 'error' || st === 'failed') {
      provider.state = 'error';
    } else if (st === 'completed' || st === 'done') {
      provider.state = ageMs < (opts?.doneAfterMs ?? 180000) ? 'done' : 'idle';
    } else {
      provider.state = ageMs < (opts?.doneAfterMs ?? 180000) ? 'done' : 'idle';
    }
    provider.state_since = Number(row.last_at || Date.now());
    provider.task.title = truncateChars(row.title || '', 30);
    provider.task.tool = row.model || null;

    const used = Number(usage?.used || 0);
    const size = Number(usage?.size || row.context_window || 0);
    provider.context = { used, size };

    const since = periodStart(cfg.budgetPeriod || 'month');
    const localUsed = Math.round(sumCredits(handle, since) * 100) / 100;

    // 额度口径：优先用客户端真实余额（billing 接口，与 App 顶部「剩余积分」一致）；
    // 拿不到（WorkBuddy 没开 / 接口变了）才退回「预算 − 本地已用」的估算。
    const credits = cfg.creditsFromClient === false ? null : workBuddyCredits();
    if (credits && Number.isFinite(credits.total)) {
      const capacity = credits.capacity > 0 ? credits.capacity : Number(cfg.budgetCredits ?? 100);
      provider.quota = {
        kind: 'credit',
        remaining: Math.round(credits.total * 100) / 100,
        budget: Math.round(capacity * 100) / 100,
        used: Math.round((credits.used || 0) * 100) / 100,
        unit: 'credit',
        source: 'billing',
        age_s: Math.round((Date.now() - credits.at) / 1000),
        groups: credits.groups,
      };
    } else {
      const budget = Number(cfg.budgetCredits ?? 100);
      provider.quota.used = localUsed;
      provider.quota.remaining = Math.round(Math.max(0, budget - localUsed) * 100) / 100;
      provider.quota.source = 'local-estimate';
      const err = workBuddyCreditsError();
      if (err) provider.quota.error = err;
    }

    // 有弹框在等你 → 覆盖会话状态。这是当下最紧急的事实：
    // 会话可能还写着 working，其实它早就卡在等人点「允许」了。
    if (appr) {
      provider.state = 'awaiting';
      provider.state_since = appr.at;
    }

    provider.health = 'ok';
    lastError = null;
  } catch (e) {
    lastError = e.message;
    provider.state = 'offline';
    provider.health = 'unknown';
    provider.error = e.message;
    close();
  }

  return provider;
}

export function workbuddyError() {
  return lastError;
}
