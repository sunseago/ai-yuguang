#include <Arduino.h>

#include "board_config.h"
#include "canvas.h"
#include "display.h"
#include "imu.h"
#include "input.h"
#include "model.h"
#include "net.h"
#include "power.h"
#include "screenpower.h"
#include "ui.h"

static Canvas gCanvas;
static DpModel gModel;
static int gPage = 0;      // 0 总览 / 1 详情 / 2 信息
static int gSelected = 0;
static int gBrightIdx = 1;
static const uint8_t kBrightSteps[] = {30, 60, 100};

static uint32_t gLastRender = 0;
static uint32_t gLastBeat = 0;
static bool gDirty = true;
static bool gLastOnline = false;
static uint8_t gLastRot = 0;
static bool gImuOk = false;

// 主循环的"最长停顿"。心跳里打出来，这样"卡住"这件事有数字可看，
// 不用靠手感猜——触摸丢点、转屏迟钝都是它的下游症状。
static uint32_t gLastLoopAt = 0;
static uint32_t gMaxGap = 0;


// 定义在下面（render 之后），这里先声明——render 和主循环都要用
uint64_t bridgeNowMs();

static void render() {
  UiInput in{};
  in.model = &gModel;
  in.page = gPage;
  in.selected = gSelected;
  in.pendingIndex = -1;
  // 用「桥接时间 + 本地已过时长」推算，秒数才会自己走，而不是每次拉取才跳一下
  in.nowMs = bridgeNowMs();
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

// 桥接时间 + 本机已过时长。用这个而不是直接拿 device millis()：
// state_since 是 epoch，两边不同基准，只有换成同一套时钟才能算"等了多久"。
uint64_t bridgeNowMs() {
  if (!gModel.bridgeOnline || !gModel.bridgeTs) return 0;
  return gModel.bridgeTs + (uint64_t)(millis() - gModel.localStampMs);
}

static void splash() {
  gCanvas.clear(dpcolor::BG);
  gCanvas.text(16, 96, "AI余光", dpcolor::TEXT, 2);
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
  Serial.printf("\n[dp] AI余光 %s boot\n", DP_FW_VERSION);
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

  // 屏幕电源策略要在首帧渲染前就位，否则开机那一下亮度是未定义的
  screenPowerInit();
  displayBrightness(kBrightSteps[gBrightIdx]);

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
    // 先把这一份交给后台任务，否则任务第一次判离线时会把界面清空
    netPublishInitial(gModel);
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
  netStartPolling();   // 网络从此在 core 0 的独立任务里跑
  render();
}

void loop() {
  inputPoll();

  // 熄屏状态下这一下的触摸只用于唤醒，不能顺带翻页/点卡片 ——
  // 否则会出现"想叫醒屏幕，结果把页面滑走了"。
  const bool screenWasOff = screenPowerIsOff();

  uint32_t now = millis();

  // 网络在后台任务里跑，这里只是取一份快照 —— 主循环再也不会被 HTTP 阻塞。
  // 之前 http.GET() 直接跑在这儿，主机不回应时每 2 秒冻结 2.5 秒，
  // 触摸、转屏、重绘全被饿死（就是用户说的"NO LINK 后卡住"）。
  if (netTakeSnapshot(gModel)) {
    if (gSelected >= gModel.count) gSelected = 0;
    gDirty = true;
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
    n += snprintf(beat + n, sizeof(beat) - n, " gap=%lums", (unsigned long)gMaxGap);
    gMaxGap = 0;
    // 屏幕策略的现场：on/dim/off + 电脑空闲秒数 + 亮多少。
    // 判断"为什么屏黑了"只看这一小段就够。
    n += snprintf(beat + n, sizeof(beat) - n, " scr=%s idle=%lds br=%u",
                  screenPowerIsOff() ? "off" : (screenPowerIsDim() ? "dim" : "on"),
                  (long)gModel.hostIdleS, (unsigned)screenPowerLevel());
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
  if (screenWasOff && g != DP_G_NONE) {
    g = DP_G_NONE;   // 只当作"叫醒"，不导航
  }
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

  // 熄屏时短按也只用来唤醒。长按（重新配网）不拦——那是"不管屏幕怎样都要生效"的操作。
  if (screenWasOff) {
    bootShort = false;
    userShort = false;
  }

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

  // ---- 屏幕电源策略 ----
  // 跟"电脑用不用"走，不跟"插没插电"走：人在电脑前就亮，人走开变暗，电脑停了熄屏。
  // 判据和阈值都在 screenpower.cpp / board_config.h 里，这里只喂输入。
  if (active || screenWasOff) screenPowerNoteInput();
  screenPowerUpdate(gModel, gModel.bridgeOnline, powerUsbHostPresent(),
                    kBrightSteps[gBrightIdx]);

  if (bootShort) {
    if (gPage == 2) {
      // 信息页上 BOOT 短按 = 现场校准屏幕朝向：0→1→2→3 循环，每按一次转 90°，
      // 自动存进 NVS。这样"面板哪边算上"不用再改代码重烧。
      imuSetOffset((uint8_t)((imuOffset() + 1) & 3), true);
      displaySetRotation(imuRotation());
      gLastRot = imuRotation();
      gDirty = true;
    } else {
      // 只改"基础亮度"，实际下发由 screenPowerUpdate 统一做，
      // 免得这里把背光写回 100% 又把暗档/熄屏覆盖掉
      gBrightIdx = (gBrightIdx + 1) % 3;
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

  // 平时 1 秒一帧就够（走表、心跳）。有卡片在"等确认"呼吸时提到 250ms——
  // 1 Hz 的闪烁如果按 1 秒刷新，会变成一顿一顿的，看不出是"呼吸"。
  const uint32_t frameMs = uiWantsFastRedraw(gModel, bridgeNowMs()) ? 250 : 1000;
  if (gDirty || now - gLastRender > frameMs) {
    gDirty = false;
    render();
  }

  // 量一下这一圈花了多久。卡顿的元凶是循环里有阻塞调用，
  // 把它打出来比"感觉卡"有用得多。
  {
    uint32_t t = millis();
    if (gLastLoopAt) {
      uint32_t gap = t - gLastLoopAt;
      if (gap > gMaxGap) gMaxGap = gap;
      // 超过 150ms 就当场报一次：正常一圈约 10~60ms，超过说明有阻塞调用。
      // 只打异常，平时不占串口。
      if (gap > 150) {
        Serial.printf("[dp] LOOP STALL %lums at t=%lus\n", (unsigned long)gap,
                      (unsigned long)(t / 1000));
      }
    }
    gLastLoopAt = t;
  }

  delay(10);
}
