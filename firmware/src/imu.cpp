#include "imu.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "board_config.h"

namespace {

// ---- QMI8658 寄存器（见 QMI8658A datasheet Rev A, 5.3/5.4 节）----
constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t REG_CTRL1 = 0x02;  // bit6 ADDR_AI 地址自增；bit5 BE1 大端（保持 0 用小端）
constexpr uint8_t REG_CTRL2 = 0x03;  // bits6:4 aFS   bits3:0 aODR
constexpr uint8_t REG_CTRL3 = 0x04;  // 陀螺仪，这边不用
constexpr uint8_t REG_CTRL5 = 0x06;  // 低通
constexpr uint8_t REG_CTRL7 = 0x08;  // bit0 aEN 加速度计使能，bit1 gEN 陀螺仪
constexpr uint8_t REG_RESET = 0x60;  // 写 0xB0 软复位
constexpr uint8_t REG_AX_L = 0x35;   // 0x35..0x3A = ax/ay/az 各 16 位小端

// ±8g、250Hz：重力读数分辨率足够（4096 LSB/g），又不至于太吵
constexpr uint8_t CTRL2_8G_250HZ = 0x25;

// 姿态判定阈值
constexpr float kFlatInPlane = 0.35f;   // in-plane 小于此值算"平放"，不再改方向
constexpr float kTiltInPlane = 0.55f;   // 超过此值才允许改方向（约 33° 倾角）
constexpr float kAxisMargin = 0.25f;    // 两个轴差不多大时视为模糊，不切
constexpr uint8_t kStableNeeded = 5;    // 连续 5 次一致才切换（轮询 200ms → 约 1s）

bool s_ready = false;
uint8_t s_addr = QMI8658_ADDR_HIGH;
uint8_t s_rot = 0;
uint8_t s_cand = 0;
uint8_t s_stable = 0;
int8_t s_lastLogQuad = -1;
bool s_first = true;
uint8_t s_offset = IMU_ROT_OFFSET;  // 整体偏移，可在信息页用 BOOT 键现场调
uint8_t s_base = 0;                 // 最近一次算出的重力象限（不含偏移）
float s_ax = 0, s_ay = 0, s_az = 0, s_inPlane = 0;
uint32_t s_lastPoll = 0;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool probe(uint8_t addr) {
  s_addr = addr;
  uint8_t id = 0;
  if (!readRegs(REG_WHO_AM_I, &id, 1)) return false;
  return id == QMI8658_WHO_AM_I;
}

// 由重力在屏幕平面上的方向决定基础方向。
//
// 推导（屏幕像素坐标 x 向右、y 向下）：
//   · 加速度计静止时输出的是"反作用力"方向，也就是物理上的「上」。
//     所以物理「下」在屏幕里的方向是 d = (-ax, -ay)。
//   · 画面要看着正，逻辑上的"下"就必须落在物理"下"上。
//     dpRotateFrame 把画面顺时针转 90°×r 之后，逻辑"下"落到屏幕的方向 θ(r)：
//         r=0 → (0,+1) 屏下     r=1 → (-1,0) 屏左
//         r=2 → (0,-1) 屏上     r=3 → (+1,0) 屏右
//   · 令 θ(r) = d，解出四个象限：
//         d 指向屏下 → r=0    d 指向屏左 → r=1
//         d 指向屏上 → r=2    d 指向屏右 → r=3
//   反面代入验证：d 指向屏左时芯片 +X 朝上，即设备被逆时针转了 90°，
//   此时画面顺时针补 90°（r=1）正好抵消。
//
// 注意 ax 和 ay 都要取负 —— 早先只给 ay 取了负、漏了 ax，
// 结果两个横向姿态都差 180°，只有竖向的正/倒两种能对上。
uint8_t baseFromGravity(float ax, float ay) {
  const float dx = -ax;  // 物理"下"在屏幕 x 轴上的分量
  const float dy = -ay;  // 物理"下"在屏幕 y 轴上的分量
  if (fabsf(dy) >= fabsf(dx)) return dy >= 0 ? 0 : 2;
  return dx >= 0 ? 3 : 1;
}

}  // namespace

bool imuInit() {
  // 先把现场调好的偏移读回来（就算 IMU 探测失败也要读，信息页要显示）
  {
    Preferences p;
    p.begin("deskpulse", true);
    s_offset = (uint8_t)(p.getUChar("rot_off", IMU_ROT_OFFSET) & 3);
    p.end();
  }

  if (!probe(QMI8658_ADDR_HIGH) && !probe(QMI8658_ADDR_LOW)) {
    s_ready = false;
    return false;
  }

  writeReg(REG_RESET, 0xB0);  // 软复位
  delay(15);
  writeReg(REG_CTRL1, 0x40);              // 地址自增 + 小端读
  writeReg(REG_CTRL2, CTRL2_8G_250HZ);    // ±8g / 250Hz
  writeReg(REG_CTRL3, 0x00);              // 陀螺仪保持关闭
  writeReg(REG_CTRL5, 0x00);              // 不开低通，姿态用不到
  writeReg(REG_CTRL7, 0x01);              // 只使能加速度计
  delay(20);

  // 复位后确认一次芯片还在
  uint8_t id = 0;
  if (!readRegs(REG_WHO_AM_I, &id, 1) || id != QMI8658_WHO_AM_I) {
    s_ready = false;
    return false;
  }

  s_ready = true;
  // 先按"正立"套上偏移，开机画面从一开始就是校准后的方向，
  // 等第一次采样再按真实姿态修正
  s_rot = s_offset & 3;
  s_cand = s_rot;
  s_stable = 0;
  s_lastLogQuad = -1;
  s_first = true;
  return true;
}

bool imuReady() { return s_ready; }
uint8_t imuAddress() { return s_addr; }
uint8_t imuRotation() { return s_rot; }
uint8_t imuOffset() { return s_offset; }

void imuSetOffset(uint8_t off, bool persist) {
  s_offset = off & 3;
  // 立刻用最近一次的象限重算，不等下一个 1 秒去抖窗口
  s_rot = (uint8_t)((s_base + s_offset) & 3);
  s_cand = s_rot;
  s_stable = 0;
  if (persist) {
    Preferences p;
    p.begin("deskpulse", false);
    p.putUChar("rot_off", s_offset);
    p.end();
  }
  Serial.printf("[dp] rot_offset=%u (r=%u)\n", (unsigned)s_offset, (unsigned)s_rot);
}
bool imuIsFlat() { return s_inPlane < kFlatInPlane; }

void imuLastSample(float& ax, float& ay, float& az, float& inPlane) {
  ax = s_ax;
  ay = s_ay;
  az = s_az;
  inPlane = s_inPlane;
}

bool imuReadAccel(float& ax, float& ay, float& az) {
  if (!s_ready) return false;
  uint8_t b[6];
  if (!readRegs(REG_AX_L, b, 6)) return false;
  int16_t rawAx = (int16_t)((b[1] << 8) | b[0]);
  int16_t rawAy = (int16_t)((b[3] << 8) | b[2]);
  int16_t rawAz = (int16_t)((b[5] << 8) | b[4]);
  // ±8g 量程 → 4096 LSB/g
  ax = rawAx / 4096.0f;
  ay = rawAy / 4096.0f;
  az = rawAz / 4096.0f;
  return true;
}

uint8_t imuPoll() {
  if (!s_ready) return s_rot;

  uint32_t now = millis();
  if (now - s_lastPoll < 200) return s_rot;
  s_lastPoll = now;

  float ax, ay, az;
  if (!imuReadAccel(ax, ay, az)) return s_rot;

  s_ax = ax;
  s_ay = ay;
  s_az = az;
  s_inPlane = sqrtf(ax * ax + ay * ay);

  // 平放时"哪边是上"没有意义，保持不动，避免放桌上乱转
  if (s_inPlane < kFlatInPlane) {
    s_cand = s_rot;
    s_stable = 0;
    return s_rot;
  }
  if (s_inPlane < kTiltInPlane) return s_rot;  // 灰色地带，别抖

  // 两个轴接近时方向模糊，先不动
  if (fabsf(fabsf(ax) - fabsf(ay)) < kAxisMargin) return s_rot;

  const uint8_t base = baseFromGravity(ax, ay);
  s_base = base;

  // 主导象限一变就打一行，方便从串口核对「哪个物理姿态对应哪个 r」，
  // 不用盯着屏幕猜。抖动时不打（上面的轴差阈值已经挡掉大部分）。
  if ((int8_t)base != s_lastLogQuad) {
    s_lastLogQuad = (int8_t)base;
    Serial.printf("[dp] imu quad=%u ax=%.2f ay=%.2f az=%.2f -> base=%u off=%u (rot now %u)\n",
                  (unsigned)base, ax, ay, az, (unsigned)base, (unsigned)s_offset,
                  (unsigned)s_rot);
  }

  uint8_t target = (uint8_t)((base + s_offset) & 3);

  // 开机第一次采样直接采用：这时还没画过几帧，没必要等 1 秒稳定
  if (s_first) {
    s_first = false;
    s_rot = target;
    s_cand = target;
    s_stable = 0;
    return s_rot;
  }

  if (target == s_rot) {
    s_cand = target;
    s_stable = 0;
    return s_rot;
  }

  if (target == s_cand) {
    if (s_stable < 255) s_stable++;
  } else {
    s_cand = target;
    s_stable = 1;
  }

  if (s_stable >= kStableNeeded) {
    s_rot = target;
    s_stable = 0;
  }
  return s_rot;
}
