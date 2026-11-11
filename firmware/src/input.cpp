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
uint32_t s_touchRetryAt = 0;
uint8_t s_touchRetries = 0;

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// 从机在传输中途被复位打断时，会把 SDA/SCL 一直按住，之后所有通信都失败。
// 9 个时钟 + STOP 是 I2C 规范里标准的解锁序列，先做掉再判断总线。
void i2cUnstick() {
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  delayMicroseconds(50);
  if (digitalRead(IIC_SDA) && digitalRead(IIC_SCL)) return;  // 总线本来就空闲
  for (int i = 0; i < 9; i++) {
    digitalWrite(IIC_SCL, LOW);
    pinMode(IIC_SCL, OUTPUT);
    delayMicroseconds(10);
    pinMode(IIC_SCL, INPUT_PULLUP);
    delayMicroseconds(10);
  }
  digitalWrite(IIC_SDA, LOW);
  pinMode(IIC_SDA, OUTPUT);
  delayMicroseconds(10);
  pinMode(IIC_SCL, INPUT_PULLUP);  // SCL 高时把 SDA 拉起 = STOP
  delayMicroseconds(10);
  pinMode(IIC_SDA, INPUT_PULLUP);
  delayMicroseconds(10);
}

bool touchBegin() {
  // 复位/中断引脚交给 SensorLib；坐标变换按微雪示例：交换 XY + X 镜像。
  s_touch.setPins(TP_RST, TP_INT);
  const bool ok = s_touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (ok) {
    s_touch.setMaxCoordinates(LCD_W, LCD_H);
    s_touch.setSwapXY(true);
    s_touch.setMirrorXY(true, false);
  }
  return ok;
}

// SDA/SCL 短接检测。两线短在一起时：START 能发出去、任何器件都不可能 ACK，
// 表现和"器件没电"一模一样（都是整条总线一个器件都扫不到），但这是硬件故障，
// 改固件永远修不好。启动时花 4ms 查一下，日志里直说，省得下次又从总线时序查起。
bool i2cShortCheck() {
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  delay(2);

  digitalWrite(IIC_SDA, LOW);
  pinMode(IIC_SDA, OUTPUT);  // 拉低 SDA，看 SCL 是否跟着下去
  delay(2);
  const bool sclFollows = digitalRead(IIC_SCL) == 0;
  pinMode(IIC_SDA, INPUT_PULLUP);

  digitalWrite(IIC_SCL, LOW);
  pinMode(IIC_SCL, OUTPUT);  // 反向再确认一次
  delay(2);
  const bool sdaFollows = digitalRead(IIC_SDA) == 0;
  pinMode(IIC_SCL, INPUT_PULLUP);

  return sclFollows || sdaFollows;
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
bool s_userLongFired = false;  // PLUS 长按：切提示音
bool s_pwrDown = false;
uint32_t s_pwrDownAt = 0;
bool s_pwrFired = false;
bool s_pwrArmed = true;
bool s_pwrShortFired = false;  // 短按：手动熄屏/亮屏

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

  // 先查短路：短了的话后面所有探测都没有意义，日志里必须一眼看出来
  if (i2cShortCheck()) {
    Serial.println(
        "[dp] ERROR: SDA(15) 与 SCL(14) 短接 —— I2C 硬件故障，触摸/IMU/PMU 全部不可用");
  }

  // 总线由这里统一起起来，再交给各库使用。
  // 顺序很关键：实测「先 Wire.begin 再让 SensorLib begin」是唯一稳定组合
  // （SensorLib 内部会 setPins 并打一行 "bus already initialized"，但它容错，
  //  真正会失败的是反过来：库自己起总线、PMU 再 begin 时拿不到）。
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.setTimeOut(20);
  delay(5);

  // 探三颗器件确认总线活着。上电初期外设轨可能还没稳，所以带重试 + 总线解锁：
  // 一次探测失败就判死会让"晚 100ms 起来"的板子永远没有触摸。
  int alive = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (attempt) {
      i2cUnstick();
      delay(150);
    }
    alive = 0;
    for (uint8_t a : {AXP2101_ADDR, CST92XX_ADDR, QMI8658_ADDR_HIGH}) {
      if (i2cProbe(a)) alive++;
    }
    if (alive) break;
  }
  Serial.printf("[dp] i2c alive=%d/3 (34=PMU 5A=touch 6B=IMU)\n", alive);

  s_touchOk = touchBegin();
  Serial.printf("[dp] touch=%s\n", s_touchOk ? "CST9220" : "FAILED");
  if (!s_touchOk) {
    // 交给 inputPoll 继续试：如果是外设轨晚起，能自己长回来，不用重启
    s_touchRetryAt = millis() + 3000;
    s_touchRetries = 0;
  }
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
#if defined(BOARD_AMOLED216)
  // 触摸初始化失败时后台慢慢重试：外设电源轨晚起/总线被锁都会自愈，
  // 不用整机重启。20 次之后放弃（大概 100 秒），免得一直空转。
  if (!s_touchOk && s_touchRetries < 20 && (int32_t)(millis() - s_touchRetryAt) >= 0) {
    s_touchRetries++;
    s_touchRetryAt = millis() + 5000;
    if (i2cProbe(CST92XX_ADDR)) {
      s_touchOk = touchBegin();
      Serial.printf("[dp] touch retry #%u -> %s\n", s_touchRetries, s_touchOk ? "CST9220" : "FAILED");
    }
  }
#endif

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
    s_userLongFired = false;
  } else if (user && s_userDown) {
    // 按住 0.9s = 长按（切提示音），松开时不再补一次短按
    if (now - s_userDownAt > 900) s_userLongFired = true;
  } else if (!user && s_userDown) {
    s_userDown = false;
    if (!s_userLongFired && now - s_userDownAt > 60 && now - s_userDownAt < 3000) {
      s_userShortFired = true;
    }
    s_userLongFired = false;
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
    // 没到 1s 就松开 = 短按。太短的（<60ms）算抖动，不认。
    if (!s_pwrFired && now - s_pwrDownAt > 60) s_pwrShortFired = true;
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

bool inputTakeUserLong() {
  // 长按在按住期间只触发一次，取走后清掉；松开时也不会补短按（见 s_userLongFired）
  static bool s_taken = false;
  if (s_userLongFired && !s_taken) {
    s_taken = true;
    return true;
  }
  if (!s_userLongFired) s_taken = false;
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

bool inputTakePwrShort() {
  bool v = s_pwrShortFired;
  s_pwrShortFired = false;
  return v;
}
