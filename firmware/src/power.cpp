#include "power.h"

#include <Arduino.h>

#include "board_config.h"
#include "display.h"

#include <esp_sleep.h>

#include "driver/rtc_io.h"

// ---- 两块板共用的电压 -> 电量曲线 ----
// 单节锂电的粗略曲线，够用来显示百分比。
// 不用 PMIC 自带的电量计：AXP2101 的 0xA4 需要先使能并经过学习周期，
// 出厂状态下读出来恒为 0（界面就会一直显示 0%）。
namespace {
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
      const float t = (v - kCurve[i - 1][0]) / (kCurve[i][0] - kCurve[i - 1][0]);
      return (int8_t)(kCurve[i - 1][1] + t * (kCurve[i][1] - kCurve[i - 1][1]));
    }
  }
  return 100;
}
}  // namespace

#if defined(BOARD_AMOLED216)
// ---------------- AMOLED 2.16：AXP2101 ----------------
// 这块板没有 BAT_EN 保持脚，也没有电池分压 ADC：电压、充电状态都从 PMIC 读。
#include <Wire.h>
#include <XPowersLib.h>

namespace {

XPowersPMU s_pmu;
bool s_pmuOk = false;
bool s_usbCache = false;
uint32_t s_usbAt = 0;

}  // namespace

void powerInit() {
  // 总线已由触摸初始化（XPowersLib 的 begin 若在已初始化总线上再设引脚会失败，
  // 所以这里不自己 Wire.begin）。
  s_pmuOk = s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!s_pmuOk) {
    Serial.println("[dp] pmu=FAILED (AXP2101 not found)");
    return;
  }

  // 只开 ADC 采样通道（用来读电池/VBUS 电压），不碰任何电源轨使能。
  // 不开这两个的话 getBattVoltage() 读到 0，界面会错误显示 "USB 0%"。
  // 这两句写的是 ADC 通道控制寄存器，即便某次读失败也只会影响温度/系统电压的
  // 采样，不会像 enableALDO/BLDO 那样（同一寄存器里混着电源轨使能位）连带熄轨。
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableVbusVoltageMeasure();

  // 只读，不碰任何电源轨寄存器。
  // 之前这里"确保打开" DC1/ALDO/BLDO，是用「读-改-写」实现的：一旦某次读失败
  // 拿到 0，回写时就会把寄存器里其它使能位一起清掉，直接熄掉板上某条轨，
  // 且 PMU 保持供电不会自己复位，表现为整条 I2C 上所有器件同时消失、断电才恢复。
  // 微雪官方示例也从不配置电源轨——板子出厂默认状态就是对的，这里只做观测。
  const bool dc1 = s_pmu.isEnableDC1();
  Serial.printf("[dp] pmu=AXP2101 batt=%d vbus=%dmV dc1=%d/%umV aldo1=%d aldo2=%d bldo1=%d bldo2=%d\n",
                s_pmu.isBatteryConnect() ? 1 : 0, (int)s_pmu.getVbusVoltage(), dc1 ? 1 : 0,
                dc1 ? (unsigned)s_pmu.getDC1Voltage() : 0u, s_pmu.isEnableALDO1() ? 1 : 0,
                s_pmu.isEnableALDO2() ? 1 : 0, s_pmu.isEnableBLDO1() ? 1 : 0,
                s_pmu.isEnableBLDO2() ? 1 : 0);
}

float powerBatteryVoltage() {
  if (!s_pmuOk || !s_pmu.isBatteryConnect()) return -1.0f;
  const uint16_t mv = s_pmu.getBattVoltage();
  return mv > 2000 ? mv / 1000.0f : -1.0f;
}

bool powerOnUsb() {
  uint32_t now = millis();
  if (s_usbAt && now - s_usbAt < 2000) return s_usbCache;
  s_usbAt = now;
  bool usb = false;
  if (s_pmuOk) {
    usb = s_pmu.isVbusIn();
  } else {
    // PMIC 读不到时按"墙插"处理：阈值宽松，不会因为读不到电池就误熄屏
    usb = true;
  }
  s_usbCache = usb;
  return usb;
}

int8_t powerBatteryPercent() {
  if (!s_pmuOk || !s_pmu.isBatteryConnect()) return -1;
  return voltageToPercent(powerBatteryVoltage());
}

#else
// ---------------- LCD 1.54：USB SOF + 电池 ADC ----------------
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

// 分压后读数会抖，取几次中位数更稳
float readBatteryVoltageOnce() {
  uint32_t acc = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
  float pinMv = (float)acc / N;
  return pinMv * BAT_VOLT_DIVIDER / 1000.0f;
}

}  // namespace

void powerInit() {
  // BAT_EN 已经在 setup 最开始拉高，这里只补 ADC 配置
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
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
#endif

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

// ---------------- 关机：深度睡眠 ----------------
// 长按 PWR 触发（见 input.cpp 的 1s 判定）。
//
// 为什么不去拉低 BAT_EN 做"真断电"：这块板的 BAT_EN 是电源保持脚，放掉之后
// 电池供电会真的断电，只能靠再按 PWR 的硬件上电电路重新起来。万一那一路没接好，
// 就变成"关得掉、开不了"，只能拆电池。深度睡眠在观感上一样（黑屏、静止、几乎不耗电），
// 但任何情况下长按 PWR 都能唤醒，不会把板子卡死。
void powerOffDeepSleep() {
#if BTN_PWR >= 0
  // 先清屏再睡：LCD 定在最后一帧会一直亮着背光，AMOLED 更会烧屏
  displayFill(0x0000);
  displayBrightness(0);
  Serial.println("[dp] power off -> deep sleep (long press PWR to wake)");
  Serial.flush();

  // 必须等按键松开。ext0 是电平唤醒，手还按着就睡下去会立刻被自己唤醒，
  // 表现成"按一下黑屏又马上亮回来"。
  const uint32_t t0 = millis();
  while (digitalRead(BTN_PWR) == LOW && millis() - t0 < 3000) delay(20);

  // 睡眠期间 GPIO 交给 RTC 域，上拉要显式打开，否则引脚悬空会被噪声唤醒
  rtc_gpio_pullup_en((gpio_num_t)BTN_PWR);
  rtc_gpio_pulldown_dis((gpio_num_t)BTN_PWR);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PWR, 0);  // 低电平（按下）唤醒
  delay(50);
  esp_deep_sleep_start();  // 不返回；唤醒后等于重新上电，从头跑 setup()
#endif
}
