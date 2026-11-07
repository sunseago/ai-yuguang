#include "power.h"

#include <Arduino.h>

#include "board_config.h"

// 本板固件把日志走在原生 USB（ARDUINO_USB_MODE=1 + ARDUINO_USB_CDC_ON_BOOT=1），
// 这时 Serial 就是 HWCDC，它的 isPlugged() 由 USB SOF 中断维护，
// 能真实反映"线另一头有没有主机"。core 2.0.17 没有 usb_serial_jtag_is_connected()。
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && defined(ARDUINO_USB_CDC_ON_BOOT) && \
    ARDUINO_USB_CDC_ON_BOOT
#define DP_HAS_HWCDC 1
#else
#define DP_HAS_HWCDC 0
#endif

namespace {

bool s_usbCache = false;
uint32_t s_usbAt = 0;
uint32_t s_lastActivity = 0;

// 分压后读数会抖，取几次中位数更稳
float readBatteryVoltageOnce() {
  uint32_t acc = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
  float pinMv = (float)acc / N;
  return pinMv * BAT_VOLT_DIVIDER / 1000.0f;
}

// 单节锂电的粗略曲线，够用来显示百分比
int8_t voltageToPercent(float v) {
  if (v < 0) return -1;
  static const float kCurve[][2] = {
      {3.30f, 0},  {3.50f, 12}, {3.65f, 30}, {3.75f, 50},
      {3.85f, 68}, {3.95f, 82}, {4.05f, 92}, {4.20f, 100},
  };
  const int n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (v <= kCurve[0][0]) return 0;
  if (v >= kCurve[n - 1][0]) return 100;
  for (int i = 1; i < n; i++) {
    if (v <= kCurve[i][0]) {
      float t = (v - kCurve[i - 1][0]) / (kCurve[i][0] - kCurve[i - 1][0]);
      return (int8_t)(kCurve[i - 1][1] + t * (kCurve[i][1] - kCurve[i - 1][1]));
    }
  }
  return 100;
}

}  // namespace

void powerInit() {
  // BAT_EN 已经在 setup 最开始拉高，这里只补 ADC 配置
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  s_lastActivity = millis();
  powerBatteryVoltage();  // 预热一次，顺便让 USB 判定有缓存
}

float powerBatteryVoltage() {
  float v = readBatteryVoltageOnce();
  // 完全量不到电压（没装电池）时给 -1，避免显示成 0%
  if (v < 2.0f) return -1.0f;
  return v;
}

bool powerOnUsb() {
  uint32_t now = millis();
  if (s_usbAt && now - s_usbAt < 2000) return s_usbCache;
  s_usbAt = now;

  bool usb = false;
#if DP_HAS_HWCDC
  // 该外设枚举到主机，说明线另一头是电脑
  if (Serial.isPlugged()) usb = true;
#endif
  if (!usb) {
    float v = powerBatteryVoltage();
    if (v < 0) usb = true;             // 没电池 = 一定在吃 USB
    else if (v > 4.15f) usb = true;    // 充到 4.15 以上只可能是充电
  }
  s_usbCache = usb;
  return usb;
}

int8_t powerBatteryPercent() {
  return voltageToPercent(powerBatteryVoltage());
}

const char* powerLabel() {
  static char buf[16];
  if (powerOnUsb()) {
    int8_t pct = powerBatteryPercent();
    if (pct < 0) return "USB";
    snprintf(buf, sizeof(buf), "USB %d%%", pct);
    return buf;
  }
  int8_t pct = powerBatteryPercent();
  if (pct < 0) return "BAT ?";
  snprintf(buf, sizeof(buf), "BAT %d%%", pct);
  return buf;
}

void powerNoteActivity() { s_lastActivity = millis(); }

uint32_t powerIdleMs() { return millis() - s_lastActivity; }
