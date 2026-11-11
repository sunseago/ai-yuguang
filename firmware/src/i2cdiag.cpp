// 临时诊断：AMOLED 2.16 板上 I2C 全 NAK 时用来定位"到底是总线死了还是器件没电"。
// 只在定义了 DP_I2C_DIAG 时参与编译，正式固件里不出现。
//
// 为什么不用 Wire：ESP32 的 I2C 外设在 SDA 回读不及时会给仲裁丢失/超时，
// 报出来的错分不清是"器件没有"还是"上拉太弱"。位翻转完全由我们控制时序，
// 换个速度再扫一遍就能区分：慢速能通、快速不通 = 上拉电阻太弱（供电轨异常）。
#include <Arduino.h>

#include "board_config.h"

#if defined(BOARD_AMOLED216) && defined(DP_I2C_DIAG)

namespace {

struct Bb {
  int sda, scl, half;

  void begin() {
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    delayMicroseconds(200);
  }
  void sdaLow() {
    digitalWrite(sda, LOW);
    pinMode(sda, OUTPUT);
  }
  void sdaRel() { pinMode(sda, INPUT_PULLUP); }
  void sclLow() {
    digitalWrite(scl, LOW);
    pinMode(scl, OUTPUT);
  }
  void sclRel() { pinMode(scl, INPUT_PULLUP); }

  bool start() {
    sdaRel();
    sclRel();
    delayMicroseconds(half);
    if (!digitalRead(sda)) return false;  // SDA 被从机按住，发不出 START
    sdaLow();
    delayMicroseconds(half);
    sclLow();
    delayMicroseconds(half);
    return true;
  }

  // 返回 true = 收到 ACK
  bool writeByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
      if ((b >> i) & 1) sdaRel();
      else sdaLow();
      delayMicroseconds(half);
      sclRel();
      delayMicroseconds(half * 2);
      sclLow();
      delayMicroseconds(half);
    }
    sdaRel();
    delayMicroseconds(half);
    sclRel();
    delayMicroseconds(half * 2);
    const bool ack = digitalRead(sda) == LOW;
    sclLow();
    delayMicroseconds(half);
    return ack;
  }

  void stop() {
    sdaLow();
    delayMicroseconds(half);
    sclRel();
    delayMicroseconds(half * 2);
    sdaRel();
    delayMicroseconds(half);
  }

  // SCL 打 9 拍 + STOP：把卡在半途、把 SDA 按低的从机顶出去
  void unstick() {
    sdaRel();
    sclRel();
    delayMicroseconds(half);
    if (digitalRead(sda) && digitalRead(scl)) return;
    for (int i = 0; i < 9; i++) {
      sclLow();
      delayMicroseconds(half * 2);
      sclRel();
      delayMicroseconds(half * 2);
    }
    stop();
  }
};

void reportLevels(const char* tag) {
  // 不带上拉先量一次：读 1 说明外部上拉真的存在；读 0 说明有东西在拉低
  pinMode(IIC_SDA, INPUT);
  pinMode(IIC_SCL, INPUT);
  delayMicroseconds(300);
  const int fSda = digitalRead(IIC_SDA), fScl = digitalRead(IIC_SCL);
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  delayMicroseconds(300);
  const int pSda = digitalRead(IIC_SDA), pScl = digitalRead(IIC_SCL);
  Serial.printf("[diag] %-14s noPull sda=%d scl=%d | pullup sda=%d scl=%d\n", tag, fSda, fScl, pSda,
                pScl);
}

void scan(const char* tag, int halfUs, bool verbose) {
  Bb bb{IIC_SDA, IIC_SCL, halfUs};
  bb.begin();
  bb.unstick();

  int found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (!bb.start()) {
      Serial.printf("[diag] %s scan aborted at 0x%02X (sda held low)\n", tag, a);
      break;
    }
    const bool ack = bb.writeByte((uint8_t)(a << 1));
    bb.stop();
    delayMicroseconds(halfUs * 2);
    if (ack) {
      found++;
      if (verbose) Serial.printf("[diag]   %s found 0x%02X\n", tag, a);
    }
  }
  Serial.printf("[diag] %s scan (half=%uus) -> %d device(s)\n", tag, halfUs, found);
}

// SDA/SCL 短路检测。两线短在一起时：START 能"发出去"、任何器件都不可能 ACK，
// 表现和"器件没电"完全一样，但这是硬件故障，改固件永远修不好——必须先排除它。
// 判据：拉低一根，另一根（内部上拉、纯输入）也跟着变低 => 两线导通。
bool shortTest() {
  auto release = [](int pin) { pinMode(pin, INPUT_PULLUP); };
  auto driveLow = [](int pin) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  };

  release(IIC_SDA);
  release(IIC_SCL);
  delay(3);
  const int idleSda = digitalRead(IIC_SDA), idleScl = digitalRead(IIC_SCL);

  driveLow(IIC_SDA);
  delay(3);
  const int sclWhenSdaLow = digitalRead(IIC_SCL);
  release(IIC_SDA);

  driveLow(IIC_SCL);
  delay(3);
  const int sdaWhenSclLow = digitalRead(IIC_SDA);
  release(IIC_SCL);

  // 对照组：驱动一个无关引脚，确认"跟着变低"不是所有脚都有的现象
  pinMode(17, OUTPUT);
  digitalWrite(17, LOW);
  delay(3);
  const int sdaRef = digitalRead(IIC_SDA), sclRef = digitalRead(IIC_SCL);
  pinMode(17, INPUT_PULLUP);

  const bool shorted = (sclWhenSdaLow == 0) || (sdaWhenSclLow == 0);
  Serial.printf("[diag] idle sda=%d scl=%d | sda->0 makes scl=%d | scl->0 makes sda=%d"
                " | ref(gp17->0) sda=%d scl=%d => %s\n",
                idleSda, idleScl, sclWhenSdaLow, sdaWhenSclLow, sdaRef, sclRef,
                shorted ? "SDA/SCL SHORTED (hardware fault)" : "not shorted");
  return shorted;
}

}  // namespace

void i2cDiagRun() {
  Serial.println("[diag] === I2C bus diagnosis (AMOLED 2.16) ===");
  reportLevels("boot");

  // 短路要先查：短了的话后面所有扫描结果都没有意义
  if (shortTest()) {
    Serial.println("[diag] === end diagnosis (short circuit, stop here) ===");
    return;
  }

  // 三种速度：12k / 40k / 400k。慢速只靠芯片内部弱上拉也能跑通，
  // 所以"慢能通、快不能通"就是外部上拉/供电轨没起来的铁证。
  scan("12k", 40, true);
  scan("40k", 12, true);
  scan("400k", 1, true);

  reportLevels("after-scan");
  Serial.println("[diag] === end diagnosis ===");
}

#endif
