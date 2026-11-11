#include "input.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"
#include "display.h"

#if defined(BOARD_AMOLED216)
// CST9220 的寄存器协议和 CST816 完全不同（0xD000 读 + ACK 握手），
// 自己实现容易随固件版本翻车，直接用微雪示例同款 SensorLib。
#include <TouchDrv.hpp>

namespace {
TouchDrvCST92xx s_touch;
bool s_touchOk = false;

// 位翻转探测：完全绕开 ESP32 的 I2C 外设，用 GPIO 手动发一个地址字节看有没有 ACK。
// 用来区分「Wire 配置问题」和「器件真的不在总线上」。
bool bbProbe(int sda, int scl, uint8_t addr7) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(50);
  if (!digitalRead(sda) || !digitalRead(scl)) return false;  // 总线被按住

  auto sdaLow = [&] { pinMode(sda, OUTPUT); digitalWrite(sda, LOW); };
  auto sdaRel = [&] { pinMode(sda, INPUT_PULLUP); };
  auto sclLow = [&] { pinMode(scl, OUTPUT); digitalWrite(scl, LOW); };
  auto sclRel = [&] { pinMode(scl, INPUT_PULLUP); };

  // START
  sdaRel(); sclRel(); delayMicroseconds(5);
  sdaLow(); delayMicroseconds(5);
  sclLow(); delayMicroseconds(5);

  // 地址 + 写位
  for (int i = 7; i >= 0; i--) {
    if ((addr7 >> i) & 1) sdaRel(); else sdaLow();
    delayMicroseconds(4);
    sclRel(); delayMicroseconds(6);
    sclLow(); delayMicroseconds(4);
  }

  // 第 9 拍读 ACK（从机拉低 SDA）
  sdaRel(); delayMicroseconds(4);
  sclRel(); delayMicroseconds(6);
  const bool ack = digitalRead(sda) == LOW;
  sclLow(); delayMicroseconds(4);

  // STOP
  sdaLow(); delayMicroseconds(4);
  sclRel(); delayMicroseconds(4);
  sdaRel(); delayMicroseconds(4);
  return ack;
}

// 标准 I2C 总线恢复：某个从机在传输中被复位打断时，会把 SDA 一直按低，
// 让后续所有通信都失败。这里用 SCL 打 9 个时钟把它"顶"出去，再发一个 STOP。
void i2cBusRecover() {
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  delayMicroseconds(50);
  if (digitalRead(IIC_SDA) && digitalRead(IIC_SCL)) return;  // 本来就是空闲的

  pinMode(IIC_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(IIC_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(5);
  }
  // STOP：SCL 高时把 SDA 从低拉到高
  pinMode(IIC_SDA, OUTPUT);
  digitalWrite(IIC_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(IIC_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(IIC_SDA, HIGH);
  delayMicroseconds(5);
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  Serial.printf("[dp] i2c bus recovered, sda=%d scl=%d\n", digitalRead(IIC_SDA),
                digitalRead(IIC_SCL));
}

void bbScan() {
  struct Pair { int sda, scl; const char* name; };
  static const Pair pairs[] = {
      {IIC_SDA, IIC_SCL, "15/14"},   // 微雪官方 pin_config
      {IIC_SCL, IIC_SDA, "14/15"},   // 万一标反
      {47, 48, "47/48"},
      {41, 42, "41/42"},
  };
  for (const auto& p : pairs) {
    Serial.printf("[dp] bb %s: 34=%d 5A=%d 6B=%d\n", p.name, bbProbe(p.sda, p.scl, 0x34) ? 1 : 0,
                  bbProbe(p.sda, p.scl, 0x5A) ? 1 : 0, bbProbe(p.sda, p.scl, 0x6B) ? 1 : 0);
  }
}
}  // namespace
#endif

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
bool s_bootLongLatched = false;  // 长按已触发过：松开时不再补一个短按
bool s_bootShortFired = false;
bool s_userDown = false;
uint32_t s_userDownAt = 0;
bool s_userShortFired = false;
bool s_pwrDown = false;
uint32_t s_pwrDownAt = 0;
bool s_pwrFired = false;
bool s_pwrArmed = true;

// 机械按键的抖动会造出一串假边沿（短按变两次、长按被重置）。
// 状态必须稳定 20ms 才认，够便宜也够用。
struct Debounced {
  bool stable = false;
  bool raw = false;
  uint32_t rawAt = 0;

  bool poll(bool pressed, uint32_t now) {
    if (pressed != raw) {
      raw = pressed;
      rawAt = now;
    } else if (now - rawAt >= 20) {
      stable = raw;
    }
    return stable;
  }
};

Debounced s_bootDeb, s_userDeb, s_pwrDeb;

struct DpTouch {
  bool down;
  int x, y;
};

DpTouch readTouch() {
  DpTouch p{false, 0, 0};
#if defined(BOARD_AMOLED216)
  if (!s_touchOk) return p;
  int16_t xs[2] = {0};
  int16_t ys[2] = {0};
  const uint8_t n = s_touch.getPoint(xs, ys, 2);
  if (n == 0) return p;
  p.down = true;
  p.x = xs[0];
  p.y = ys[0];
  return p;
#else
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
#endif
}

}  // namespace

void inputInit() {
#if defined(BOARD_AMOLED216)
  pinMode(TP_INT, INPUT_PULLUP);

  // 先做一次位翻转体检，输出到串口；正常情况会看到 15/14: 34=1 5A=1 6B=1
  i2cBusRecover();
  bbScan();

  // 不要在这里先 Wire.begin()：SensorLib 的 begin(wire, addr, sda, scl) 会自己
  // 初始化总线，提前初始化会让它内部的 setPins 失败（"bus already initialized"）
  // 从而 begin 直接返回 false —— 实测触摸会消失。总线交给它管。
  s_touch.setPins(TP_RST, TP_INT);
  s_touchOk = s_touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (s_touchOk) {
    s_touch.setMaxCoordinates(LCD_W, LCD_H);
    s_touch.setSwapXY(true);
    s_touch.setMirrorXY(true, false);
  }
  Serial.printf("[dp] touch=%s\n", s_touchOk ? "CST9220" : "FAILED");
#else
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  pinMode(TP_INT, INPUT_PULLUP);
#endif

  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(BTN_USER, INPUT_PULLUP);
  int pwrLevel = -1;
#if BTN_PWR >= 0
  pinMode(BTN_PWR, INPUT_PULLUP);
  pwrLevel = digitalRead(BTN_PWR);
#endif
  Serial.printf("[dp] btn boot=%d user=%d pwr=%d\n", digitalRead(BTN_BOOT), digitalRead(BTN_USER),
                pwrLevel);
}

void inputPoll() {
  DpTouch t = readTouch();
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
  bool boot = s_bootDeb.poll(digitalRead(BTN_BOOT) == LOW, now);
  if (boot && !s_bootDown) {
    s_bootDown = true;
    s_bootDownAt = now;
    s_bootLongFired = false;
    s_bootLongLatched = false;
  } else if (boot && s_bootDown) {
    if (!s_bootLongFired && now - s_bootDownAt > 5000) {
      s_bootLongFired = true;
      s_bootLongLatched = true;
      s_bootShortFired = false;
    }
  } else if (!boot && s_bootDown) {
    s_bootDown = false;
    // 用 latched 而不是 longFired：longFired 会被 inputTakeBootLong() 立刻取走清零，
    // 那样松开时就会误判成短按，多排一次"切亮度"。
    if (!s_bootLongLatched && now - s_bootDownAt > 60) s_bootShortFired = true;
    s_bootLongLatched = false;
  }

  bool user = s_userDeb.poll(digitalRead(BTN_USER) == LOW, now);
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
#if BTN_PWR >= 0
  bool pwr = s_pwrDeb.poll(digitalRead(BTN_PWR) == LOW, now);
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
#endif
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
