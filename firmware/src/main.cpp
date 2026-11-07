#include <Arduino.h>

#include "board_config.h"
#include "canvas.h"
#include "display.h"
#include "imu.h"
#include "input.h"
#include "model.h"
#include "net.h"
#include "power.h"
#include "ui.h"

static Canvas gCanvas;
static DpModel gModel;
static int gPage = 0;      // 0 总览 / 1 详情 / 2 信息
static int gSelected = 0;
static int gBrightIdx = 1;
static const uint8_t kBrightSteps[] = {30, 60, 100};

static uint32_t gLastFetch = 0;
static uint32_t gLastNet = 0;
static uint32_t gLastOk = 0;
static uint32_t gLastRender = 0;
static uint32_t gLastBeat = 0;
static bool gDirty = true;
static bool gLastOnline = false;
static bool gScreenOn = true;
static uint8_t gLastRot = 0;
static bool gImuOk = false;

// 熄屏/亮屏统一走这里，保证亮度只在这一个地方被清零
static void setScreenOn(bool on) {
  if (gScreenOn == on) return;
  gScreenOn = on;
  displayBrightness(on ? kBrightSteps[gBrightIdx] : 0);
  if (on) gDirty = true;
}

static void render() {
  UiInput in{};
  in.model = &gModel;
  in.page = gPage;
  in.selected = gSelected;
  in.pendingIndex = -1;
  // 用「桥接时间 + 本地已过时长」推算，秒数才会自己走，而不是每次拉取才跳一下
  uint64_t nowMs = 0;
  if (gModel.bridgeOnline && gModel.bridgeTs) {
    nowMs = gModel.bridgeTs + (uint64_t)(millis() - gModel.localStampMs);
  }
  in.nowMs = nowMs;
  in.wifiOk = netOnline();
  in.rssi = netRssi();
  in.bridgeHost = netBridgeHost();
  in.ip = netIp();
  in.fwVersion = DP_FW_VERSION;
  in.brightness = kBrightSteps[gBrightIdx];
  in.clockText = netClockText();
  in.power = powerLabel();
  in.rotation = displayRotation();
  in.rotOffset = imuOffset();
  in.imuOk = imuReady();

  uiRender(gCanvas, in);
  displayPush(gCanvas.buffer());
  gLastRender = millis();
}

static void splash() {
  gCanvas.clear(dpcolor::BG);
  gCanvas.text(16, 96, "DeskPulse", dpcolor::TEXT, 2);
  gCanvas.text(16, 132, "connecting...", dpcolor::MUTED, 1);
  gCanvas.text(16, 176, DP_FW_VERSION, dpcolor::FAINT, 1);
  displayPush(gCanvas.buffer());
}

// 日志一次写完，别拆成十几次小写。
// 注意：HWCDC 在主机没连着的时候会静默丢数据（HWCDC::write 里 flushTXBuffer），
// 所以「串口没输出」不等于固件卡死——看网络侧设备有没有在轮询更靠谱。
static void logLine(const char* buf, int len) {
  if (len > 0) Serial.write((const uint8_t*)buf, (size_t)len);
}

void setup() {
  // 电池供电时必须第一时间拉高电源保持脚，否则会掉电
  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, HIGH);

  Serial.begin(115200);
  delay(150);
  Serial.printf("\n[dp] DeskPulse %s boot\n", DP_FW_VERSION);
  Serial.printf("[dp] psram=%u freeheap=%u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

  displayInit();
  uint16_t* buf = displayBuffer();
  Serial.printf("[dp] canvas=%s ptr=%p\n", buf ? "ok" : "FAILED", (void*)buf);
  gCanvas.attach(buf);
  splash();

  inputInit();
  Serial.println("[dp] touch+buttons ready");

  powerInit();
  Serial.printf("[dp] power=%s batt=%s\n", powerLabel(),
                powerBatteryVoltage() < 0 ? "none" : "detected");

  gImuOk = imuInit();
  if (gImuOk) {
    Serial.printf("[dp] imu=ok addr=0x%02X rot_offset=%u\n", imuAddress(), (unsigned)imuOffset());
  } else {
    // IMU 挂了也不影响朝向：偏移本身就是个静态旋转，照样套上
    Serial.printf("[dp] imu=FAILED (auto-rotate off) rot_offset=%u\n", (unsigned)imuOffset());
  }
  displaySetRotation(imuRotation());
  gLastRot = displayRotation();

  netBegin();
  Serial.printf("[dp] wifi=%d ip=%s bridge=%s\n", (int)netOnline(), netIp(), netBridgeHost());

  if (netFetch(gModel)) {
    gLastOk = millis();
    gModel.bridgeOnline = true;
    Serial.printf("[dp] first fetch ok, providers=%d\n", gModel.count);
    for (int i = 0; i < gModel.count; i++) {
      const DpProvider& p = gModel.items[i];
      // 额度按类型打印实际值，方便不用看屏幕也能核对桥接数据
      if (p.quotaKind == DP_Q_BALANCE) {
        Serial.printf("[dp]   #%d %s state=%s balance=%.2f %s spend/d=%.2f burn=%.1f\n", i, p.name,
                      dpStateLabel(p.state), p.remaining, p.currency, p.spentToday,
                      p.hasBurn ? p.burnDays : -1.0f);
      } else if (p.quotaKind == DP_Q_CREDIT) {
        Serial.printf("[dp]   #%d %s state=%s credit=%.2f/%.2f\n", i, p.name,
                      dpStateLabel(p.state), p.used, p.budget);
      } else {
        Serial.printf("[dp]   #%d %s state=%s quota=none\n", i, p.name, dpStateLabel(p.state));
      }
    }
  } else {
    Serial.println("[dp] first fetch FAILED (bridge unreachable?)");
  }
  gLastFetch = millis();
  render();
}

void loop() {
  inputPoll();

  uint32_t now = millis();

  if (now - gLastNet > 500) {
    gLastNet = now;
    netLoop();
  }

  if (now - gLastFetch >= 2000) {
    gLastFetch = now;
    DpModel fresh;
    if (netFetch(fresh)) {
      gModel = fresh;
      gModel.bridgeOnline = true;
      gLastOk = now;
      if (gSelected >= gModel.count) gSelected = 0;
      gDirty = true;
    } else if (now - gLastOk > 10000) {
      if (gModel.bridgeOnline) {
        gDirty = true;
        Serial.println("[dp] bridge LOST");
      }
      gModel.bridgeOnline = false;
    }
  }

  if (gModel.bridgeOnline != gLastOnline) {
    gLastOnline = gModel.bridgeOnline;
    Serial.printf("[dp] bridge %s\n", gModel.bridgeOnline ? "ONLINE" : "OFFLINE");
  }

  // 30s 一条心跳，方便长时间挂机时从串口判断状态
  if (now - gLastBeat > 30000) {
    gLastBeat = now;
    static char beat[384];
    int n = snprintf(beat, sizeof(beat),
                     "[dp] t=%lus wifi=%d link=%d heap=%u psram_free=%u rot=%u imu=%s pwr=%s",
                     (unsigned long)(now / 1000), (int)netOnline(), (int)gModel.bridgeOnline,
                     (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                     (unsigned)displayRotation(), gImuOk ? "ok" : "--", powerLabel());
    if (gImuOk) {
      float ax, ay, az, ip;
      imuLastSample(ax, ay, az, ip);
      n += snprintf(beat + n, sizeof(beat) - n, " g=%.2f,%.2f,%.2f flat=%d", ax, ay, az,
                    imuIsFlat() ? 1 : 0);
    }
    for (int i = 0; i < gModel.count && n < (int)sizeof(beat) - 1; i++) {
      const DpProvider& p = gModel.items[i];
      // 连额度一起打，不看屏幕也能核对桥接数据对不对
      if (p.quotaKind == DP_Q_BALANCE) {
        n += snprintf(beat + n, sizeof(beat) - n, " | %s:%s %.2f", p.id, dpStateLabel(p.state),
                      p.remaining);
      } else if (p.quotaKind == DP_Q_CREDIT) {
        n += snprintf(beat + n, sizeof(beat) - n, " | %s:%s %.0f/%.0f", p.id,
                      dpStateLabel(p.state), p.used, p.budget);
      } else {
        n += snprintf(beat + n, sizeof(beat) - n, " | %s:%s", p.id, dpStateLabel(p.state));
      }
    }
    if (n < (int)sizeof(beat) - 1) beat[n++] = '\n';
    logLine(beat, n);
  }

  // ---- 触摸手势 ----
  bool active = false;
  int tx = 0, ty = 0;
  DpGesture g = inputTakeGesture(tx, ty);
  if (g == DP_G_TAP) {
    active = true;
    if (gPage == 0) {
      int idx = uiHitCard(gModel, tx, ty);
      if (idx >= 0) {
        gSelected = idx;
        gPage = 1;
        gDirty = true;
      }
    } else if (gPage == 1) {
      gPage = 0;
      gDirty = true;
    } else {
      gPage = 0;
      gDirty = true;
    }
  } else if (g == DP_G_SWIPE_LEFT) {
    active = true;
    if (gPage == 0) {
      gPage = 2;
      gDirty = true;
    }
  } else if (g == DP_G_SWIPE_RIGHT) {
    active = true;
    if (gPage != 0) {
      gPage = 0;
      gDirty = true;
    }
  }

  // ---- 按键 ----
  bool bootShort = inputTakeBootShort();
  bool bootLong = inputTakeBootLong();
  bool userShort = inputTakeUserShort();
  if (bootShort || bootLong || userShort) active = true;

  // ---- 自动转屏：只有明显立起来才切，平放时保持不动 ----
  if (gImuOk) {
    uint8_t r = imuPoll();
    if (r != gLastRot) {
      gLastRot = r;
      displaySetRotation(r);
      gDirty = true;
      float ax, ay, az, ip;
      imuLastSample(ax, ay, az, ip);
      Serial.printf("[dp] rotate=%u (%u deg) ax=%.2f ay=%.2f az=%.2f\n", r, r * 90u, ax, ay, az);
    }
  }

  // ---- 熄屏策略 ----
  // 插在电脑上（USB 供电）永远不熄屏；只有电池供电且设置了超时才会熄。
  if (active) powerNoteActivity();
#if DP_BLANK_AFTER_MS > 0
  if (powerOnUsb() || powerIdleMs() < DP_BLANK_AFTER_MS) {
    setScreenOn(true);
  } else {
    setScreenOn(false);
  }
#else
  (void)active;
  setScreenOn(true);
#endif

  if (bootShort) {
    if (gPage == 2) {
      // 信息页上 BOOT 短按 = 现场校准屏幕朝向：0→1→2→3 循环，每按一次转 90°，
      // 自动存进 NVS。这样"面板哪边算上"不用再改代码重烧。
      imuSetOffset((uint8_t)((imuOffset() + 1) & 3), true);
      displaySetRotation(imuRotation());
      gLastRot = imuRotation();
      gDirty = true;
    } else {
      gBrightIdx = (gBrightIdx + 1) % 3;
      if (gScreenOn) displayBrightness(kBrightSteps[gBrightIdx]);
      gDirty = true;
    }
  }
  if (bootLong) {
    gCanvas.clear(dpcolor::ERR);
    gCanvas.text(20, 100, "resetting...", dpcolor::WHITE, 1);
    displayPush(gCanvas.buffer());
    netClearConfig();
    delay(500);
    ESP.restart();
  }
  if (userShort) {
    gPage = (gPage + 1) % 3;
    gDirty = true;
  }

  // 每秒重绘一次（走表/呼吸），有变化也立刻重绘
  if (gDirty || now - gLastRender > 1000) {
    gDirty = false;
    render();
  }

  delay(10);
}
