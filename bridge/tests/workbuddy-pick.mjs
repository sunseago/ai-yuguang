// WorkBuddy 适配器「选会话」逻辑单测：
//   node tests/workbuddy-pick.mjs
//
// 背景：适配器原来只取「最近写库的那一个会话」。两个会话并行时，
// 刚结束的那个（completed，last_at 最新）会把还跑着的这个（working，
// 但 last_activity_at 只在轮次开始时写）挤下卡片 —— 板子明明在跑，
// 卡片却显示别人的 IDLE。另外新鲜度必须看 max(last_activity_at, updated_at)：
// WorkBuddy 跑工具的过程中只更新 updated_at。
import { pickLiveSession } from '../adapters/workbuddy.mjs';

let fails = 0;
const ok = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const row = (title, status, freshAt, lastAt = freshAt) => ({
  id: title,
  title,
  status,
  fresh_at: freshAt,
  last_at: lastAt,
});

const NOW = 1000000;
const IDLE = 300000;

// 1) 刚结束的 completed 不能挤掉还活着的 working（本次线上事故的还原）
{
  // 关键：working 的 fresh_at（=updated_at）是新鲜的 60s，只是 last_activity_at 旧（367s）
  const rows = [
    row('调研OpenWrt', 'completed', NOW - 147000, NOW - 147000),  // 最近写库
    row('开发固件', 'working', NOW - 60000, NOW - 367000),        // 还活着
  ];
  ok('completed 新 ≠ 抢卡片：选中 working', pickLiveSession(rows, IDLE, NOW).title === '开发固件');
}

// 2) 新鲜度必须看 updated_at：工具跑 6 分钟但 updated_at 一直在动
{
  const rows = [row('开发固件', 'working', NOW - 60000, NOW - 367000)];
  ok('updated_at 新鲜 → working 判 thinking', pickLiveSession(rows, IDLE, NOW).title === '开发固件');
}

// 3) working 但真的 stale（App 崩了/关了）→ 回退到最近写库的
{
  const rows = [
    row('调研OpenWrt', 'completed', NOW - 40000),
    row('僵死的任务', 'working', NOW - 900000),
  ];
  ok('stale working 回退到最近会话', pickLiveSession(rows, IDLE, NOW).title === '调研OpenWrt');
}

// 4) 没有活动会话 → 按最近写库（原行为，不能退化）
{
  const rows = [
    row('刚完成A', 'completed', NOW - 30000),
    row('更早完成B', 'completed', NOW - 90000),
  ];
  ok('全 completed 时选最近的', pickLiveSession(rows, IDLE, NOW).title === '刚完成A');
}

// 5) 两个都活着 → 挑 fresh_at 更新的那个
{
  // rows 按 fresh_at 降序来（SQL 里 ORDER BY fresh_at DESC），第一个活的就是最新的
  const rows = [
    row('刚开始', 'working', NOW - 30000),
    row('较早开始', 'working', NOW - 200000),
  ];
  ok('双活选更新鲜的', pickLiveSession(rows, IDLE, NOW).title === '刚开始');
}

// 6) 空列表
{
  ok('空列表返回 null', pickLiveSession([], IDLE, NOW) === null);
}

// 7) pending 也算活动状态
{
  const rows = [row('等确认', 'pending', NOW - 10000)];
  ok('pending 视为活动', pickLiveSession(rows, IDLE, NOW).title === '等确认');
}

console.log(fails ? `\n有 ${fails} 项失败` : '\n全部通过');
process.exit(fails ? 1 : 0);
