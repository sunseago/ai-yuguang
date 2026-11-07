#pragma once

enum DpGesture {
  DP_G_NONE = 0,
  DP_G_TAP,
  DP_G_SWIPE_LEFT,
  DP_G_SWIPE_RIGHT,
};

void inputInit();
void inputPoll();  // 每 20ms 调一次
DpGesture inputTakeGesture(int& x, int& y);

// 按键：BOOT 短按切亮度 / 长按 5s 重新配网；USER 短按切页
bool inputTakeBootShort();
bool inputTakeBootLong();
bool inputTakeUserShort();
