#include "screenpower.h"

#include <Arduino.h>

#include "board_config.h"
#include "display.h"
#include "screenlogic.h"

// 决策逻辑在 screenlogic.h（纯函数，电脑上可单测）；这里只做三件事：
// 收拢硬件信号、维护时间戳、把模式翻译成背光百分比。
//
// 两份实现是刻意避免的：之前在 .cpp 里写了一遍状态机、又在头文件里写了一遍，
// 改一处忘了另一处就会让单测通过而固件行为不符。现在只有 screenlogic.h 一份。

namespace {

const ScrCfg kCfg = {
    SCR_DIM_AFTER_IDLE_WALL_MS,
    SCR_DIM_AFTER_IDLE_BATT_MS,
    SCR_OFF_AFTER_IDLE_WALL_MS,
    SCR_OFF_AFTER_IDLE_BATT_MS,
    SCR_OFF_AFTER_NOLINK_MS,
    SCR_WAKE_PEEK_MS,
    SCR_OFF_FADE_MS,
};

ScrMode s_mode = SCR_ON;
uint8_t s_level = 255;      // 上次真正下发的亮度；255 = 还没下发过
uint32_t s_modeAt = 0;      // 进入当前模式的 millis()
uint32_t s_lastInput = 0;   // 最后一次触摸/按键
uint32_t s_lastLinkOk = 0;  // 最后一次拿到数据的时刻
bool s_linkOkPrev = false;
bool s_manualOff = false;   // PWR 长按的手动关机开关

// 只有真正变了才写背光：displayBrightness 内部要动 LEDC，没必要每轮都写
void applyLevel(uint8_t pct) {
  if (pct == s_level) return;
  s_level = pct;
  displayBrightness(pct);
}

}  // namespace

void screenPowerInit(uint8_t initialPercent) {
  s_mode = SCR_ON;
  s_modeAt = millis();
  s_lastInput = millis();
  s_lastLinkOk = millis();
  s_linkOkPrev = false;
  s_manualOff = false;
  // 与 setup 里 displayBrightness(initialPercent) 对齐，避免首帧前 br=0 的误报
  s_level = initialPercent > SCR_MAX_LEVEL ? SCR_MAX_LEVEL : initialPercent;
}

void screenPowerNoteInput() { s_lastInput = millis(); }

void screenPowerToggleManualOff() {
  s_manualOff = !s_manualOff;
  if (s_manualOff) {
    // 关机：立刻熄屏，后面 update 会一直保持 OFF
    s_mode = SCR_OFF;
    applyLevel(0);
  } else {
    // 开机：当作刚交互过，先亮一个 peek，再交给自动策略
    s_lastInput = millis();
    s_mode = SCR_ON;
    s_modeAt = millis();
    // 关机时 s_level 已被写成 0，下一轮 update 会自然把基础亮度写回去，
    // 不需要再用 255 这种"未下发"哨兵（那会让心跳短暂报出 br=255）。
  }
}

bool screenPowerIsOff() { return s_mode == SCR_OFF; }
bool screenPowerIsManualOff() { return s_manualOff; }
bool screenPowerIsDim() { return s_mode == SCR_DIM; }
bool screenPowerIsFading() { return s_mode == SCR_FADE; }
uint8_t screenPowerLevel() { return s_level; }

void screenPowerUpdate(const DpModel& m, bool linkOk, bool onBattery, uint8_t basePercent) {
  const uint32_t now = millis();

  // 手动关机：强制熄屏，自动策略整个停摆。只有 PWR 长按能把它翻回来，
  // 触摸、链路恢复都不该唤醒——那才是"关机"的语义。
  if (s_manualOff) {
    if (s_mode != SCR_OFF) s_mode = SCR_OFF;
    applyLevel(0);
    return;
  }

  if (linkOk) {
    if (!s_linkOkPrev) {
      // 链路从断到通：电脑回来了。当作一次"输入"，亮一个 peek 周期打个招呼 ——
      // 你坐下、Mac 醒来的那一刻屏就该是亮的，不用再伸手点一下。
      s_lastInput = now;
    }
    s_lastLinkOk = now;
  }
  s_linkOkPrev = linkOk;

  ScrIn in;
  in.linkOk = linkOk;
  in.onBattery = onBattery;
  // 断链时 hostIdleS 是上一次成功拉取留下的旧值，不能再拿它判"人走开了"。
  // 统一当作未知（-1 = 人在用），熄屏交给 linkLost 单独裁决。
  in.hostIdleS = linkOk ? m.hostIdleS : -1;
  in.linkAgeMs = now - s_lastLinkOk;
  in.sinceInputMs = now - s_lastInput;
  in.modeAgeMs = now - s_modeAt;

  const ScrMode next = scrDecide(s_mode, in, kCfg);
  if (next != s_mode) {
    s_mode = next;
    s_modeAt = now;
  }

  switch (s_mode) {
    case SCR_OFF:
      applyLevel(0);
      break;
    case SCR_FADE:
      applyLevel(SCR_OFF_FADE_PERCENT);
      break;
    case SCR_DIM: {
      // 暗档不高于用户选的基础亮度：用户选 30% 时，暗档就是 15%
      uint8_t pct = basePercent < SCR_DIM_PERCENT ? basePercent : SCR_DIM_PERCENT;
      applyLevel(pct);
      break;
    }
    case SCR_ON:
    default:
      applyLevel(basePercent > SCR_MAX_LEVEL ? SCR_MAX_LEVEL : basePercent);
      break;
  }
}
