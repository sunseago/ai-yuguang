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
    SCR_DIM_AFTER_HOST_IDLE_MS,
    SCR_DIM_AFTER_HOST_IDLE_BAT_MS,
    SCR_OFF_AFTER_NOLINK_USB_MS,
    SCR_OFF_AFTER_NOLINK_BAT_MS,
    SCR_WAKE_PEEK_MS,
    SCR_OFF_FADE_MS,
};

ScrMode s_mode = SCR_ON;
uint8_t s_level = 255;      // 上次真正下发的亮度；255 = 还没下发过
uint32_t s_modeAt = 0;      // 进入当前模式的 millis()
uint32_t s_lastInput = 0;   // 最后一次触摸/按键
uint32_t s_lastLinkOk = 0;  // 最后一次拿到数据的时刻
bool s_everLinked = false;  // 开机以来有没有成功连上过桥接
bool s_linkOkPrev = false;

// 只有真正变了才写背光：displayBrightness 内部要动 LEDC，没必要每轮都写
void applyLevel(uint8_t pct) {
  if (pct == s_level) return;
  s_level = pct;
  displayBrightness(pct);
}

}  // namespace

void screenPowerInit() {
  s_mode = SCR_ON;
  s_modeAt = millis();
  s_lastInput = millis();
  s_lastLinkOk = millis();
  s_everLinked = false;
  s_linkOkPrev = false;
  s_level = 255;
}

void screenPowerNoteInput() { s_lastInput = millis(); }

bool screenPowerIsOff() { return s_mode == SCR_OFF; }
bool screenPowerIsDim() { return s_mode == SCR_DIM; }
uint8_t screenPowerLevel() { return s_level == 255 ? 0 : s_level; }

void screenPowerUpdate(const DpModel& m, bool linkOk, bool usbHost, uint8_t basePercent) {
  const uint32_t now = millis();

  if (linkOk) {
    if (!s_everLinked) {
      s_everLinked = true;
    } else if (!s_linkOkPrev) {
      // 链路从断到通：电脑回来了。当作一次"输入"，亮一个 peek 周期打个招呼 ——
      // 你坐下、Mac 醒来的那一刻屏就该是亮的，不用再伸手点一下。
      s_lastInput = now;
    }
    s_lastLinkOk = now;
  }
  s_linkOkPrev = linkOk;

  ScrIn in;
  in.linkOk = linkOk;
  in.usbHost = usbHost;
  in.hostIdleS = m.hostIdleS;
  in.linkAgeMs = now - s_lastLinkOk;
  in.sinceInputMs = now - s_lastInput;
  in.everLinked = s_everLinked;
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
