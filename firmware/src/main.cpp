#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "board_config.h"
#include "canvas.h"
#include "display.h"
#include "imu.h"
#include "audio.h"
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
static uint8_t gSndSlot = 0;    // 设置页正在编辑哪个音效槽：0=弹窗 1=完成
static bool gVolFocus = false;  // 长按 PLUS 进来：高亮音量条
static bool gLastOnline = false;
static uint8_t gLastRot = 0;
// ---- 提示音：只在"状态跨进 WAIT / DONE"的那一下响，不是每轮都响 ----
static uint8_t s_sndPrev[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool s_sndPrimed = false;   // 首次快照只记基线，避免开机就哗一声
static uint32_t s_sndAt = 0;
static uint32_t s_sndCount = 0;

static void checkSoundEvents() {
  if (!s_sndPrimed) {
    for (int i = 0; i < gModel.count && i < 5; i++) s_sndPrev[i] = gModel.items[i].state;
    s_sndPrimed = true;
    return;
  }
  bool ask = false, done = false;
  for (int i = 0; i < gModel.count && i < 5; i++) {
    const uint8_t st = gModel.items[i].state;
    const uint8_t prev = s_sndPrev[i];
    s_sndPrev[i] = st;
    if (st == prev) continue;
    if (st == DP_ST_AWAITING) ask = true;
    else if (st == DP_ST_DONE) done = true;
  }
  if (!ask && !done) return;

  // 同一个 provider 抖一下（WAIT→IDLE→WAIT）会连响，加最小间隔压住
  const uint32_t now = millis();
  if (s_sndAt && now - s_sndAt < 2500) return;
  s_sndAt = now;
  s_sndCount++;
  // 两种同时到：优先提醒"在等你"的那个
  audioPlayEvent(done && !ask);
  Serial.printf("[dp] sound #%u: %s\n", (unsigned)s_sndCount, ask ? "attention" : "done");
}

static bool gImuOk = false;
static uint8_t gImuRetries = 0;
static uint32_t gImuRetryAt = 0;

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
  in.sndSlot = gSndSlot;
  in.alertSnd = audioAlertMode();
  in.doneSnd = audioDoneMode();
  in.volume = audioVolume();
  in.volFocus = gVolFocus;

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

// 心跳是「多次 snprintf 拼一行」。直接累加返回值的写法有个坑：snprintf 返回的
// 是"本来要写多长"，一旦超过缓冲区，后续 sizeof(buf) - n 会下溢成巨大的 size_t，
// 格式化就失去了边界，末尾 logLine 还会按 n 越界读。这里统一收口：
// 每次只在剩余空间里写，len 永远不超过 sizeof(buf)-1。
struct LineBuf {
  char buf[512];
  size_t len = 0;

  void addf(const char* fmt, ...) {
    if (len + 1 >= sizeof(buf)) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    const size_t cap = sizeof(buf) - 1 - len;
    len += ((size_t)n > cap) ? cap : (size_t)n;
    buf[len] = '\0';
  }

  void newline() {
    if (len + 1 < sizeof(buf)) {
      buf[len++] = '\n';
      buf[len] = '\0';
    }
  }
};

void setup() {
#ifdef BAT_EN
  // 电池供电时必须第一时间拉高电源保持脚，否则会掉电。
  // AMOLED 板走 AXP2101，没有这个脚（GPIO2 在那里是 SD 时钟，绝不能拉高）。
  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, HIGH);
#endif

  Serial.begin(115200);
  delay(150);
  Serial.printf("\n[dp] AI余光 %s boot (%s)\n", DP_FW_VERSION, BOARD_NAME);
  Serial.printf("[dp] psram=%u freeheap=%u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

#ifdef DP_I2C_DIAG
  // 必须在任何 I2C 库初始化之前跑，否则拿到的总线状态是别人改过的
  extern void i2cDiagRun();
  i2cDiagRun();
#endif

  displayInit();
  uint16_t* buf = displayBuffer();
  Serial.printf("[dp] canvas=%s ptr=%p\n", buf ? "ok" : "FAILED", (void*)buf);
  gCanvas.attach(buf);
  splash();

  inputInit();
  Serial.println("[dp] touch+buttons ready");

  // 音频放在触摸之后：ES8311 挂在同一条 I2C 上，靠 Wire 已经起好的总线
  audioInit();

  powerInit();
  Serial.printf("[dp] power=%s batt=%s\n", powerLabel(),
                powerBatteryVoltage() < 0 ? "none" : "detected");
  Serial.printf("[dp] batt: %.2fV (%d%%)\n", powerBatteryVoltage(), (int)powerBatteryPercent());

  // 屏幕电源策略要在首帧渲染前就位，否则开机那一下亮度是未定义的。
  // 把初始亮度告诉它，心跳里的 br= 才不会在第一帧前误报 0。
  screenPowerInit(kBrightSteps[gBrightIdx]);
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
      } else if (p.quotaKind == DP_Q_CTX) {
        Serial.printf("[dp]   #%d %s state=%s ctx=%.0f/%.0f\n", i, p.name,
                      dpStateLabel(p.state), p.used, p.budget);
      } else {
        Serial.printf("[dp]   #%d %s state=%s quota=none\n", i, p.name, dpStateLabel(p.state));
      }
    }
  } else {
    Serial.println("[dp] first fetch FAILED (bridge unreachable?)");
  }
  if (!netStartPolling()) {
    // 任务没起来不能装作没事：界面会永远停在开机那一份快照上。
    // 保留 UI 可用，同时在屏上/串口明确报错，避免"数据不动但看着像正常"。
    Serial.println("[dp] ERROR: net task failed to start, state will be stale");
  }
  render();
}

void loop() {
  inputPoll();

  // 熄屏（以及熄屏前的淡出）状态下这一下的触摸只用于唤醒，不能顺带翻页/点卡片 ——
  // 否则会出现"想叫醒屏幕，结果把页面滑走了"。
  const bool screenWasOff = screenPowerIsOff() || screenPowerIsFading();
  // 手动关机（PWR 长按）时连唤醒都不该有：只有再长按一次 PWR 才能开机。
  const bool manualOff = screenPowerIsManualOff();

  uint32_t now = millis();

  // 网络在后台任务里跑，这里只是取一份快照 —— 主循环再也不会被 HTTP 阻塞。
  // 之前 http.GET() 直接跑在这儿，主机不回应时每 2 秒冻结 2.5 秒，
  // 触摸、转屏、重绘全被饿死（就是用户说的"NO LINK 后卡住"）。
  if (netTakeSnapshot(gModel)) {
    if (gSelected >= gModel.count) gSelected = 0;
    gDirty = true;
    checkSoundEvents();
  }

  if (gModel.bridgeOnline != gLastOnline) {
    gLastOnline = gModel.bridgeOnline;
    Serial.printf("[dp] bridge %s\n", gModel.bridgeOnline ? "ONLINE" : "OFFLINE");
  }

  // 30s 一条心跳，方便长时间挂机时从串口判断状态
  if (now - gLastBeat > 30000) {
    gLastBeat = now;
    LineBuf hb;
    hb.addf("[dp] t=%lus wifi=%d link=%d heap=%u psram_free=%u rot=%u imu=%s pwr=%s",
            (unsigned long)(now / 1000), (int)netOnline(), (int)gModel.bridgeOnline,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
            (unsigned)displayRotation(), gImuOk ? "ok" : "--", powerLabel());
    hb.addf(" gap=%lums", (unsigned long)gMaxGap);
    gMaxGap = 0;
    // 屏幕策略的现场：on/dim/fade/off/manual + 电脑空闲秒数 + 亮多少。
    // 判断"为什么屏黑了"只看这一小段就够。manual = PWR 长按的手动关机。
    const char* scr = screenPowerIsManualOff()
                          ? "manual"
                          : (screenPowerIsOff() ? "off"
                             : (screenPowerIsFading() ? "fade"
                                : (screenPowerIsDim() ? "dim" : "on")));
    hb.addf(" scr=%s idle=%lds br=%u", scr, (long)gModel.hostIdleS,
            (unsigned)screenPowerLevel());
    if (gImuOk) {
      float ax, ay, az, ip;
      imuLastSample(ax, ay, az, ip);
      hb.addf(" g=%.2f,%.2f,%.2f flat=%d", ax, ay, az, imuIsFlat() ? 1 : 0);
    }
    for (int i = 0; i < gModel.count && hb.len + 1 < sizeof(hb.buf); i++) {
      const DpProvider& p = gModel.items[i];
      // 连额度一起打，不看屏幕也能核对桥接数据对不对
      if (p.quotaKind == DP_Q_BALANCE) {
        hb.addf(" | %s:%s %.2f", p.id, dpStateLabel(p.state), p.remaining);
      } else if (p.quotaKind == DP_Q_CREDIT) {
        // 余额/总额（老桥接没有 remaining 时退回已用）
        hb.addf(" | %s:%s %.0f/%.0f", p.id, dpStateLabel(p.state),
                p.remaining >= 0 ? p.remaining : p.used, p.budget);
      } else if (p.quotaKind == DP_Q_CTX) {
        hb.addf(" | %s:%s %.0fk/%.0fk", p.id, dpStateLabel(p.state), p.used / 1000.0f,
                p.budget / 1000.0f);
      } else {
        hb.addf(" | %s:%s", p.id, dpStateLabel(p.state));
      }
    }
    hb.newline();
    logLine(hb.buf, (int)hb.len);
  }

  // ---- 触摸手势 ----
  bool active = false;
  int tx = 0, ty = 0;
  DpGesture g = inputTakeGesture(tx, ty);
  // 只要这一圈真的有触摸，就算"有输入"（用于唤醒计时）。手势本身是否触发导航另说：
  // 熄屏时这一下只唤醒，不导航——所以要先把"有输入"记下来，再把手势吞掉。
  if (g != DP_G_NONE) active = true;
  if (screenWasOff && g != DP_G_NONE) {
    g = DP_G_NONE;   // 只当作"叫醒"，不导航
  }
  if (g == DP_G_TAP) {
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
    } else if (gPage == 3) {
      // 设置页：先看槽位胶囊，再看音效格子，最后看音量按钮
      const int slot = uiHitSoundSlot(tx, ty);
      const int cell = uiHitSoundCell(tx, ty);
      const int dv = uiHitVolume(tx, ty);
      if (slot >= 0) {
        gSndSlot = (uint8_t)slot;
        gDirty = true;
      } else if (cell >= 0) {
        if (gSndSlot == 0) audioSetAlertMode((uint8_t)cell);
        else audioSetDoneMode((uint8_t)cell);
        audioPlay((uint8_t)cell);  // 点一下立刻听效果
        gVolFocus = false;
        gDirty = true;
      } else if (dv != 0) {
        int v = (int)audioVolume() + dv * 10;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audioSetVolume((uint8_t)v);
        audioPlay(audioAlertMode());  // 用当前弹窗音试一下响度
        gVolFocus = true;
        gDirty = true;
      }
    } else {
      gPage = 0;
      gDirty = true;
    }
  } else if (g == DP_G_SWIPE_LEFT) {
    if (gPage == 0) {
      gPage = 2;
      gDirty = true;
    } else if (gPage == 2) {
      gPage = 3;
      gDirty = true;
    }
  } else if (g == DP_G_SWIPE_RIGHT) {
    if (gPage != 0) {
      gPage = 0;
      gDirty = true;
    }
  }

  // 不在设置页就别留着音量高亮的框
  if (gPage != 3 && gVolFocus) gVolFocus = false;

  // ---- 按键 ----
  bool bootShort = inputTakeBootShort();
  bool bootLong = inputTakeBootLong();
  bool userShort = inputTakeUserShort();
  bool userLong = inputTakeUserLong();
  bool pwrLong = inputTakePwrLong();
  bool pwrShort = inputTakePwrShort();
  if (bootShort || bootLong || userShort || userLong || pwrShort || pwrLong) active = true;

  // PLUS 长按：打开设置页并聚焦音量（之后用触摸 − / + 调）
  if (userLong) {
    gPage = 3;
    gVolFocus = true;
    audioPlay(audioAlertMode());  // 先响一下当前音量，给个参照
    gDirty = true;
    Serial.printf("[dp] volume focus (vol=%u)\n", (unsigned)audioVolume());
  }

  // PWR 分两档，都不受"屏幕是否已熄"影响 —— 关机键必须随时有效。
  //   短按：手动熄屏 / 亮屏（看完就走，不中断数据）
  //   长按 1s：关机（清屏 + 深度睡眠，再长按 PWR 唤醒重启）
  if (pwrShort) {
    screenPowerToggleManualOff();
    Serial.printf("[dp] manual screen %s\n", screenPowerIsManualOff() ? "OFF" : "ON");
  }
  if (pwrLong) {
    gDirty = false;  // 马上要睡，别再排下一帧
    powerOff();
  }

  // 熄屏时短按也只用来唤醒。长按（重新配网）不拦——那是"不管屏幕怎样都要生效"的操作。
  if (screenWasOff) {
    bootShort = false;
    userShort = false;
  }

  // ---- 自动转屏：只有明显立起来才切，平放时保持不动 ----
  // 开机时 IMU 没起来（外设轨没稳/总线被锁）不代表它坏了，定期再试几次，
  // 能自己长回来，不用整机重启。
  if (!gImuOk && gImuRetries < 12 && (int32_t)(millis() - gImuRetryAt) >= 0) {
    gImuRetries++;
    gImuRetryAt = millis() + 10000;
    gImuOk = imuInit();
    Serial.printf("[dp] imu retry #%u -> %s\n", gImuRetries, gImuOk ? "ok" : "FAILED");
    if (gImuOk) {
      displaySetRotation(imuRotation());
      gLastRot = displayRotation();
      gDirty = true;
    }
  }

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
  // 判据和阈值都在 screenlogic.h / board_config.h 里，这里只喂输入。
  //
  // 只把"这一圈真实发生了输入"记进去。绝不能因为"屏幕正熄着"就持续记输入——
  // 那样会每轮都把空闲计时清零，屏幕刚熄就自己把自己叫醒，永远熄不掉。
  if (active && !manualOff) screenPowerNoteInput();
  screenPowerUpdate(gModel, gModel.bridgeOnline, !powerOnUsb(), kBrightSteps[gBrightIdx]);

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
    // 0 总览 → 2 信息 → 3 设置 → 0（详情页只能点卡片进，不占按键档位）
    gPage = (gPage == 0) ? 2 : (gPage == 2 ? 3 : 0);
    gDirty = true;
  }

  // 平时 1 秒一帧就够（走表、心跳）。有卡片在闪时提到 120ms：
  // 1 Hz 闪烁的半周期是 500ms，250ms 一帧只有 2 帧，边沿会抖成"长短不一"；
  // 120ms 一帧每个半周期约 4 帧，节奏才稳。
  const uint32_t frameMs = uiWantsFastRedraw(gModel, bridgeNowMs()) ? 120 : 1000;
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
