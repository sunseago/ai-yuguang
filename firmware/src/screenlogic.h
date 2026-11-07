// 屏幕电源策略的**纯逻辑**：不依赖 Arduino，可以在电脑上跑单测。
//
// screenpower.cpp 只负责把硬件信号喂进来、把结果变成背光值。
// 决策本身放在这里，是因为分支多（配网 / 唤醒 / 淡出 / USB 与电池两套阈值），
// 光靠"在板子上看现象"没法覆盖到边界情况。
#pragma once
#include <stdint.h>

enum ScrMode : uint8_t {
  SCR_ON = 0,
  SCR_DIM,
  SCR_FADE,
  SCR_OFF,
};

struct ScrCfg {
  uint32_t dimAfterIdleUsbMs;
  uint32_t dimAfterIdleBatMs;
  uint32_t offAfterNolinkUsbMs;
  uint32_t offAfterNolinkBatMs;
  uint32_t peekMs;
  uint32_t fadeMs;
};

struct ScrIn {
  bool linkOk;             // 这一轮有没有拿到桥接数据
  bool usbHost;            // USB 线另一头有没有醒着的电脑
  int32_t hostIdleS;       // 电脑闲置秒数；-1 = 读不到
  uint32_t linkAgeMs;      // 距最后一次拿到数据
  uint32_t sinceInputMs;   // 距最后一次触摸/按键
  bool everLinked;         // 开机以来有没有成功连上过
  uint32_t modeAgeMs;      // 在当前模式待了多久
};

/**
 * 返回下一个模式。纯函数：同样的输入永远同样的输出，没有隐藏状态。
 *
 * 优先级（从高到低）：
 *   1. 从没连上过 → 一直亮（配网门户在这条路上，黑屏会让人以为板子坏了）
 *   2. 刚被摸过 / 电脑刚回来 → 亮一个 peek 周期
 *   3. 断链超过阈值 → 先 fade 再 off
 *   4. 电脑闲置超过阈值 → dim
 *   5. 其余 → on
 */
inline ScrMode scrDecide(ScrMode cur, const ScrIn& in, const ScrCfg& cfg) {
  if (!in.everLinked) return SCR_ON;
  if (in.sinceInputMs < cfg.peekMs) return SCR_ON;

  const uint32_t offAfter =
      in.usbHost ? cfg.offAfterNolinkUsbMs : cfg.offAfterNolinkBatMs;
  const uint32_t dimAfter =
      in.usbHost ? cfg.dimAfterIdleUsbMs : cfg.dimAfterIdleBatMs;

  const bool linkLost = !in.linkOk && in.linkAgeMs > offAfter;
  if (linkLost) {
    // fade 是"短暂的中间态"：待满 fadeMs 才真的熄
    if (cur == SCR_FADE) return in.modeAgeMs >= cfg.fadeMs ? SCR_OFF : SCR_FADE;
    if (cur == SCR_OFF) return SCR_OFF;
    return SCR_FADE;
  }

  // hostIdleS = -1 表示桥接读不到（老桥接 / ioreg 失败），当作"人在用"：
  // 宁可亮着，也不要因为读不到就把屏压暗。
  const bool idleLong =
      in.hostIdleS >= 0 && ((uint64_t)in.hostIdleS * 1000ULL) > (uint64_t)dimAfter;
  return idleLong ? SCR_DIM : SCR_ON;
}
