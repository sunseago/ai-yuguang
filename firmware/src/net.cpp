#include "net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <cstring>

#include "board_config.h"
#include "display.h"
#include "canvas.h"

namespace {

Preferences s_prefs;
WebServer* s_server = nullptr;
DNSServer* s_dns = nullptr;

char s_ssid[33] = {0};
char s_pass[65] = {0};
char s_host[40] = {0};
uint16_t s_port = 8787;
char s_ip[20] = "-";
char s_clock[8] = "--:--";
bool s_wifiOk = false;
uint32_t s_lastReconnect = 0;

uint8_t stateFromString(const char* s) {
  if (!s) return DP_ST_IDLE;
  if (!strcmp(s, "thinking")) return DP_ST_THINKING;
  if (!strcmp(s, "tool")) return DP_ST_TOOL;
  if (!strcmp(s, "awaiting")) return DP_ST_AWAITING;
  if (!strcmp(s, "done")) return DP_ST_DONE;
  if (!strcmp(s, "error")) return DP_ST_ERROR;
  if (!strcmp(s, "offline")) return DP_ST_OFFLINE;
  return DP_ST_IDLE;
}

uint8_t healthFromString(const char* s) {
  if (!s) return DP_H_UNKNOWN;
  if (!strcmp(s, "ok")) return DP_H_OK;
  if (!strcmp(s, "warn")) return DP_H_WARN;
  if (!strcmp(s, "critical")) return DP_H_CRITICAL;
  return DP_H_UNKNOWN;
}

void loadConfig() {
  s_prefs.begin("deskpulse", true);
  s_prefs.getString("ssid", "").toCharArray(s_ssid, sizeof(s_ssid));
  s_prefs.getString("pass", "").toCharArray(s_pass, sizeof(s_pass));
  s_prefs.getString("host", "").toCharArray(s_host, sizeof(s_host));
  s_port = (uint16_t)s_prefs.getUShort("port", 8787);
  s_prefs.end();
}

bool saveConfig(const String& ssid, const String& pass, const String& host, uint16_t port) {
  s_prefs.begin("deskpulse", false);
  s_prefs.putString("ssid", ssid);
  s_prefs.putString("pass", pass);
  s_prefs.putString("host", host);
  s_prefs.putUShort("port", port);
  s_prefs.end();
  return true;
}

// ---------- 配网门户：手机连上 Yuguang-xxxx 后打开 192.168.4.1 ----------
const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html><html lang="zh"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI余光 配网</title>
<style>body{font-family:-apple-system,sans-serif;margin:0;padding:24px;background:#f6f7f9;color:#16181d}
h1{font-size:20px;margin:0 0 4px}p{color:#6b7280;font-size:13px;margin:0 0 20px}
label{display:block;font-size:13px;margin:14px 0 6px;color:#6b7280}
input{width:100%;box-sizing:border-box;padding:12px;border:1px solid #e3e5e9;border-radius:10px;font-size:15px}
button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:10px;background:#16181d;color:#fff;font-size:16px}
</style>
<h1>AI余光 配网</h1><p>填好 Wi-Fi 与电脑地址后保存，设备会自动重启。</p>
<form method="post" action="/save">
<label>Wi-Fi 名称</label><input name="ssid" required>
<label>Wi-Fi 密码</label><input name="pass" type="password">
<label>电脑地址（可留空，自动发现）</label><input name="host" placeholder="192.168.1.10">
<button type="submit">保存并重启</button></form></html>)HTML";

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  String apName = "Yuguang-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.softAP(apName.c_str(), "yuguang");
  Serial.printf("[dp] setup AP: %s  -> http://192.168.4.1\n", apName.c_str());
  s_dns = new DNSServer();
  s_dns->start(53, "*", WiFi.softAPIP());
  s_server = new WebServer(80);

  s_server->on("/", HTTP_GET, []() { s_server->send_P(200, "text/html; charset=utf-8", kPortalHtml); });
  s_server->on("/save", HTTP_POST, []() {
    String ssid = s_server->arg("ssid");
    String pass = s_server->arg("pass");
    String host = s_server->arg("host");
    saveConfig(ssid, pass, host, 8787);
    s_server->send(200, "text/html; charset=utf-8",
                   "<meta charset=utf-8><body style='font-family:sans-serif;padding:24px'>"
                   "<h3>已保存</h3><p>设备正在重启…</p></body>");
    delay(800);
    ESP.restart();
  });
  s_server->onNotFound([]() { s_server->send(200, "text/html; charset=utf-8", kPortalHtml); });
  s_server->begin();

  // 屏幕上给出提示，避免用户对着黑屏猜
  uint16_t* buf = displayBuffer();
  if (buf) {
    Canvas c;
    c.attach(buf);
    c.clear(dpcolor::BG);
    c.text(12, 40, "SETUP MODE", dpcolor::TEXT, 2);
    c.text(12, 84, "1. join wifi:", dpcolor::MUTED, 1);
    c.text(12, 102, apName.c_str(), dpcolor::THINK, 1);
    c.text(12, 120, "   pass deskpulse", dpcolor::MUTED, 1);
    c.text(12, 150, "2. open 192.168.4.1", dpcolor::MUTED, 1);
    displayPush(buf);
  }

  while (true) {
    s_dns->processNextRequest();
    s_server->handleClient();
    delay(2);
  }
}

void connectWifi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(s_ssid, s_pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(120);
  }
  s_wifiOk = WiFi.status() == WL_CONNECTED;
  if (s_wifiOk) {
    strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
    Serial.printf("[dp] wifi connected ip=%s rssi=%d\n", s_ip, (int)WiFi.RSSI());
  } else {
    Serial.println("[dp] wifi connect failed");
  }
}

// ---------- UDP 发现：问一句「谁是桥接」，桥接回 IP ----------
bool discoverBridge() {
  WiFiUDP udp;
  udp.begin(8789);
  IPAddress bc = WiFi.localIP();
  bc[3] = 255;
  for (int attempt = 0; attempt < 3; attempt++) {
    udp.beginPacket(bc, 8788);
    udp.write((const uint8_t*)"DESKPULSE?", 10);
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 400) {
      int len = udp.parsePacket();
      if (len > 0) {
        char buf[128] = {0};
        int n = udp.read(buf, sizeof(buf) - 1);
        if (n > 0) {
          JsonDocument doc;
          if (!deserializeJson(doc, buf, n)) {
            const char* host = doc["host"] | "";
            int port = doc["port"] | 8787;
            if (host[0]) {
              strncpy(s_host, host, sizeof(s_host) - 1);
              s_port = (uint16_t)port;
              saveConfig(String(s_ssid), String(s_pass), String(s_host), s_port);
              udp.stop();
              return true;
            }
          }
        }
      }
      delay(20);
    }
  }
  udp.stop();
  return false;
}

}  // namespace

void netBegin() {
  loadConfig();
  if (!s_ssid[0]) startConfigPortal();
  connectWifi(15000);
  if (!s_wifiOk) {
    // 连不上就退回去配网，避免用户面对一块什么都没有的屏
    startConfigPortal();
  }
  if (!s_host[0]) {
    bool found = discoverBridge();
    Serial.printf("[dp] udp discovery %s host=%s\n", found ? "ok" : "failed", s_host);
  }
}

void netLoop() {
  bool ok = WiFi.status() == WL_CONNECTED;
  s_wifiOk = ok;
  if (ok) {
    String ip = WiFi.localIP().toString();
    strncpy(s_ip, ip.c_str(), sizeof(s_ip) - 1);
    return;
  }
  // 断线重连，10s 一次，避免把射频打爆
  if (millis() - s_lastReconnect > 10000) {
    s_lastReconnect = millis();
    WiFi.disconnect();
    WiFi.begin(s_ssid, s_pass);
  }
}

bool netFetch(DpModel& m) {
  if (!s_wifiOk || !s_host[0]) return false;

  char url[96];
  snprintf(url, sizeof(url), "http://%s:%u/v1/state", s_host, (unsigned)s_port);

  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  memset(&m, 0, sizeof(m));
  m.bridgeOnline = true;
  m.bridgeTs = (uint64_t)doc["ts"].as<double>();
  m.localStampMs = millis();
  // 缺这个字段（旧版桥接）时给 -1：设备会当作"人在用"，不会误熄屏
  m.hostIdleS = doc["host"]["idle_s"] | -1;

  const char* lt = doc["bridge"]["localTime"] | "";
  if (lt[0]) {
    strncpy(s_clock, lt, sizeof(s_clock) - 1);
  }

  JsonArray arr = doc["providers"].as<JsonArray>();
  int i = 0;
  for (JsonObject p : arr) {
    if (i >= 5) break;
    DpProvider& d = m.items[i];
    d.present = true;
    strncpy(d.id, p["id"] | "", sizeof(d.id) - 1);
    strncpy(d.name, p["name"] | "", sizeof(d.name) - 1);
    d.state = stateFromString(p["state"] | "");
    d.health = healthFromString(p["health"] | "");
    d.stateSinceMs = (uint64_t)p["state_since"].as<double>();

    JsonObject task = p["task"];
    strncpy(d.title, task["title"] | "", sizeof(d.title) - 1);
    d.step = (int16_t)(task["step"] | 0);
    strncpy(d.tool, task["tool"] | "", sizeof(d.tool) - 1);

    if (p["context"].is<JsonObject>()) {
      d.hasCtx = true;
      d.ctxUsed = p["context"]["used"] | 0;
      d.ctxSize = p["context"]["size"] | 0;
    }

    JsonObject q = p["quota"];
    const char* kind = q["kind"] | "";
    if (!strcmp(kind, "balance") && q["remaining"].is<float>()) {
      d.quotaKind = DP_Q_BALANCE;
      d.remaining = q["remaining"].as<float>();
      d.hasQuota = true;
      d.spentToday = q["spent_today"] | 0.0f;
      if (q["burn_days_left"].is<float>()) {
        d.hasBurn = true;
        d.burnDays = q["burn_days_left"].as<float>();
      }
      strncpy(d.currency, q["currency"] | "CNY", sizeof(d.currency) - 1);
    } else if (!strcmp(kind, "credit") && q["budget"].is<float>()) {
      d.quotaKind = DP_Q_CREDIT;
      d.used = q["used"] | 0.0f;
      d.budget = q["budget"].as<float>();
      d.hasQuota = true;
    }

    d.jobs = (int16_t)(p["jobs"] | 0);
    if (p["subagents"].is<int>()) d.jobs = (int16_t)(p["subagents"].as<int>());

    JsonArray tools = p["recent_tools"].as<JsonArray>();
    int t = 0;
    for (JsonObject rt : tools) {
      if (t >= 3) break;
      const char* nm = rt["name"] | "";
      bool ok = rt["ok"].is<bool>() ? rt["ok"].as<bool>() : true;
      snprintf(d.recent[t], sizeof(d.recent[t]), "%s%s", ok ? "" : "!", nm);
      t++;
    }
    i++;
  }
  m.count = i;
  return true;
}

bool netOnline() { return s_wifiOk; }
int8_t netRssi() { return s_wifiOk ? (int8_t)WiFi.RSSI() : 0; }
const char* netIp() { return s_ip; }
const char* netBridgeHost() { return s_host[0] ? s_host : "-"; }
const char* netClockText() { return s_clock; }

void netSetHost(const char* host) {
  strncpy(s_host, host, sizeof(s_host) - 1);
  saveConfig(String(s_ssid), String(s_pass), String(s_host), s_port);
}

void netClearConfig() {
  s_prefs.begin("deskpulse", false);
  s_prefs.clear();
  s_prefs.end();
}

// ---------- 后台轮询任务 ----------
//
// 为什么要有这个东西：http.GET() 是同步的，一次请求最长能占 2.5 秒
// （connect 2.5s + read 2.5s）。它原来跑在 loop() 里，实测在"主机可达但不回应"
// （比如 Mac 睡过去）时，每 2 秒就冻结界面 2.5 秒，串口里是一串 LOOP STALL 25xx ms。
// 后果是三件事一起坏：
//   · 触摸靠按下/抬起的边沿检测 → 整个手势落在冻结窗口里就丢了
//   · imuPoll 每 200ms 一次、且要连续 5 次一致才转向 → 采样被拉开，转屏要等很久
//   · 重绘被推迟 → 看起来就是"卡死"
//
// 现在网络在 core 0 的任务里跑，UI 只在 loop() 里读一份快照，永不阻塞。

namespace {

struct NetShare {
  DpModel model;
  bool online;
  uint32_t seq;   // 每发布一次自增，UI 侧据此判断"有没有新数据"
};

NetShare s_share;
portMUX_TYPE s_shareMux = portMUX_INITIALIZER_UNLOCKED;

void publishModel(const DpModel& m, bool online) {
  portENTER_CRITICAL(&s_shareMux);
  s_share.model = m;
  s_share.online = online;
  s_share.seq++;
  portEXIT_CRITICAL(&s_shareMux);
}

void publishOffline() {
  portENTER_CRITICAL(&s_shareMux);
  // 只在状态真的变化时自增，免得 UI 每 2 秒都白拷一遍 1KB
  if (s_share.online) {
    s_share.online = false;
    s_share.seq++;
  }
  portEXIT_CRITICAL(&s_shareMux);
}

void netTask(void*) {
  uint32_t lastLoopCheck = 0;
  uint32_t lastFetch = 0;
  uint32_t lastOk = 0;

  for (;;) {
    const uint32_t now = millis();

    if (now - lastLoopCheck > 500) {
      lastLoopCheck = now;
      netLoop();
    }

    if (now - lastFetch >= 2000) {
      lastFetch = now;
      DpModel fresh;
      if (netFetch(fresh)) {
        lastOk = now;
        publishModel(fresh, true);
      } else if (now - lastOk > 10000) {
        // 连续 10 秒拿不到才判离线，避免偶发丢包就闪 NO LINK
        publishOffline();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void netPublishInitial(const DpModel& m) {
  publishModel(m, true);
}

void netStartPolling() {
  // core 0 留给网络；Arduino 的 loopTask（UI、触摸、IMU、SPI）在 core 1，互不抢占。
  // 栈给 10KB：HTTPClient + ArduinoJson 在任务栈上，8KB 偏紧。
  xTaskCreatePinnedToCore(netTask, "dp-net", 10240, nullptr, 1, nullptr, 0);
}

bool netTakeSnapshot(DpModel& out) {
  static uint32_t seen = 0;
  bool changed = false;
  portENTER_CRITICAL(&s_shareMux);
  if (s_share.seq != seen) {
    out = s_share.model;
    out.bridgeOnline = s_share.online;
    seen = s_share.seq;
    changed = true;
  }
  portEXIT_CRITICAL(&s_shareMux);
  return changed;
}
