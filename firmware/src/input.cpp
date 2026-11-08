#include "input.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"
#include "display.h"

namespace {

bool s_touching = false;
int s_startX = 0, s_startY = 0;
int s_lastX = 0, s_lastY = 0;
uint32_t s_touchStart = 0;

DpGesture s_gesture = DP_G_NONE;
int s_gx = 0, s_gy = 0;

// 按键
bool s_bootDown = false;
uint32_t s_bootDownAt = 0;
bool s_bootLongFired = false;
bool s_bootShortFired = false;
bool s_userDown = false;
uint32_t s_userDownAt = 0;
bool s_userShortFired = false;
bool s_pwrDown = false;
uint32_t s_pwrDownAt = 0;
bool s_pwrFired = false;
bool s_pwrArmed = true;

struct TouchPoint {
  bool down;
  int x, y;
};

TouchPoint readTouch() {
  TouchPoint p{false, 0, 0};
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return p;
  if (Wire.requestFrom((int)CST816_ADDR, 6) != 6) return p;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  uint8_t fingers = d[1] & 0x0F;
  if (fingers == 0) return p;
  p.down = true;
  p.x = ((d[2] & 0x0F) << 8) | d[3];
  p.y = ((d[4] & 0x0F) << 8) | d[5];
  return p;
}

}  // namespace

void inputInit() {
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  pinMode(TP_INT, INPUT_PULLUP);

  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(BTN_USER, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLUP);
}

void inputPoll() {
  TouchPoint t = readTouch();
  // 屏幕可能被 IMU 转过，触摸要反变换回 UI 逻辑坐标再做手势判定，
  // 否则转过之后点卡片会点到别处、滑动手势方向也是反的
  if (t.down) displayToLogical(t.x, t.y);
  uint32_t now = millis();

  if (t.down && !s_touching) {
    s_touching = true;
    s_startX = s_lastX = t.x;
    s_startY = s_lastY = t.y;
    s_touchStart = now;
  } else if (t.down) {
    s_lastX = t.x;
    s_lastY = t.y;
  } else if (!t.down && s_touching) {
    s_touching = false;
    int dx = s_lastX - s_startX;
    int dy = s_lastY - s_startY;
    if (abs(dx) > 40 && abs(dx) > abs(dy)) {
      s_gesture = dx < 0 ? DP_G_SWIPE_LEFT : DP_G_SWIPE_RIGHT;
    } else if (abs(dx) < 20 && abs(dy) < 20 && now - s_touchStart < 800) {
      s_gesture = DP_G_TAP;
      s_gx = s_startX;
      s_gy = s_startY;
    }
  }

  // BOOT 键
  bool boot = digitalRead(BTN_BOOT) == LOW;
  if (boot && !s_bootDown) {
    s_bootDown = true;
    s_bootDownAt = now;
    s_bootLongFired = false;
  } else if (boot && s_bootDown) {
    if (!s_bootLongFired && now - s_bootDownAt > 5000) {
      s_bootLongFired = true;
      s_bootShortFired = false;
    }
  } else if (!boot && s_bootDown) {
    s_bootDown = false;
    if (!s_bootLongFired && now - s_bootDownAt > 60) s_bootShortFired = true;
  }

  bool user = digitalRead(BTN_USER) == LOW;
  if (user && !s_userDown) {
    s_userDown = true;
    s_userDownAt = now;
  } else if (!user && s_userDown) {
    s_userDown = false;
    if (now - s_userDownAt > 60 && now - s_userDownAt < 3000) s_userShortFired = true;
  }

  // PWR 键：长按 1 秒触发一次"开机/关机"。armed 锁存保证一次按下只触发一次，
  // 松开重按才会再次触发——否则长按期间会反复翻转（像 BOOT 长按那样靠重启兜底，
  // 这里没有重启可兜，必须自己锁住）。
  bool pwr = digitalRead(BTN_PWR) == LOW;
  if (pwr && !s_pwrDown) {
    s_pwrDown = true;
    s_pwrDownAt = now;
    s_pwrArmed = true;
    s_pwrFired = false;
  } else if (pwr && s_pwrDown && s_pwrArmed) {
    if (now - s_pwrDownAt >= 1000) {
      s_pwrFired = true;
      s_pwrArmed = false;
    }
  } else if (!pwr && s_pwrDown) {
    s_pwrDown = false;
  }
}

DpGesture inputTakeGesture(int& x, int& y) {
  DpGesture g = s_gesture;
  x = s_gx;
  y = s_gy;
  s_gesture = DP_G_NONE;
  return g;
}

bool inputTakeBootShort() {
  bool v = s_bootShortFired;
  s_bootShortFired = false;
  return v;
}

bool inputTakeBootLong() {
  if (s_bootLongFired) {
    s_bootLongFired = false;
    return true;
  }
  return false;
}

bool inputTakeUserShort() {
  bool v = s_userShortFired;
  s_userShortFired = false;
  return v;
}

bool inputTakePwrLong() {
  bool v = s_pwrFired;
  s_pwrFired = false;
  return v;
}
