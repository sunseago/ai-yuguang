// 屏幕电源策略的**纯逻辑**：不依赖 Arduino，可以在电脑上跑单测。
//
// screenpower.cpp 只负责把硬件信号喂进来、把结果变成背光值。
// 决策本身放在这里，是因为分支多，光靠"在板子上看现象"覆盖不到边界。
#pragma once
#include <stdint.h>

enum ScrMode : uint8_t {
  SCR_ON = 0,
  SCR_DIM,
  SCR_FADE,
  SCR_OFF,
};

struct ScrCfg {
  // 两档阈值：墙插（USB/充电器）宽松，电池收紧。
  uint32_t dimAfterIdleWallMs;   // 墙插：多久没键鼠输入 → 变暗
  uint32_t dimAfterIdleBattMs;   // 电池：→ 变暗
  uint32_t offAfterIdleWallMs;   // 墙插：多久没输入 → 熄屏（≈ Mac 显示器休眠）
  uint32_t offAfterIdleBattMs;   // 电池：→ 熄屏
  uint32_t offAfterNolinkMs;     // 断链多久 → 熄屏
  uint32_t peekMs;               // 唤醒后亮多久
  uint32_t fadeMs;               // 熄屏前的淡出时长
};

struct ScrIn {
  bool linkOk;             // 这一轮有没有拿到桥接数据
  bool onBattery;          // 电池供电（决定阈值档）
  int32_t hostIdleS;       // 电脑闲置秒数；-1 = 读不到
  uint32_t linkAgeMs;      // 距最后一次拿到数据
  uint32_t sinceInputMs;   // 距最后一次触摸/按键
  uint32_t modeAgeMs;      // 在当前模式待了多久
};

/**
 * 返回下一个模式。纯函数：同样的输入永远同样的输出，没有隐藏状态。
 *
 * 优先级（从高到低）：
 *   1. 刚被摸过 → 亮一个 peek 周期
 *   2. 断链超过阈值，或电脑闲置超过阈值 → 先 fade 再 off
 *   3. 电脑闲置超过变暗阈值 → dim
 *   4. 其余 → on
 *
 * 注意第 2 条里"熄屏"有两个来源，对应两种"电脑不用了"：
 *   - 断链：电脑关机 / 系统真的睡了 / 只插充电器而电脑没在跑
 *   - 闲置过久：电脑开着但人走开足够久（等价于 Mac 显示器休眠）
 *
 * 注意这里**没有**"从没连上过就一直亮"这一条了。曾经有，本意是保护配网门户别黑屏，
 * 但配网门户其实在 netBegin() 里是阻塞的（死循环跑 DNS/HTTP），根本走不到这里。
 * 留着这条反而有害：板子插充电器或电脑关机时重新上电，永远连不上桥接，
 * 就会因为"没连上过"而永远亮着显示 NO LINK——这正是"关机/充电器不熄屏"的根因。
 */
inline ScrMode scrDecide(ScrMode cur, const ScrIn& in, const ScrCfg& cfg) {
  if (in.sinceInputMs < cfg.peekMs) return SCR_ON;

  const uint32_t dimAfter = in.onBattery ? cfg.dimAfterIdleBattMs : cfg.dimAfterIdleWallMs;
  const uint32_t offAfterIdle =
      in.onBattery ? cfg.offAfterIdleBattMs : cfg.offAfterIdleWallMs;

  const bool linkLost = !in.linkOk && in.linkAgeMs > cfg.offAfterNolinkMs;

  // hostIdleS = -1 表示桥接读不到（老桥接 / ioreg 失败），当作"人在用"：
  // 宁可亮着，也不要因为读不到就把屏压暗/熄掉。
  const uint64_t idleMs = in.hostIdleS >= 0 ? (uint64_t)in.hostIdleS * 1000ULL : 0;
  const bool idleOff = in.hostIdleS >= 0 && idleMs > (uint64_t)offAfterIdle;
  const bool idleDim = in.hostIdleS >= 0 && idleMs > (uint64_t)dimAfter;

  if (linkLost || idleOff) {
    // fade 是"短暂的中间态"：待满 fadeMs 才真的熄
    if (cur == SCR_FADE) return in.modeAgeMs >= cfg.fadeMs ? SCR_OFF : SCR_FADE;
    if (cur == SCR_OFF) return SCR_OFF;
    return SCR_FADE;
  }
  return idleDim ? SCR_DIM : SCR_ON;
}
