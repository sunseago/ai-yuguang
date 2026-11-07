// 在电脑上把界面渲染成图片，用来核对排版（不需要开发板）。
// 编译：bash tools/preview.sh
#include <stdio.h>
#include <string.h>

#include "../src/canvas.h"
#include "../src/ui.h"

static uint16_t gBuf[DP_W * DP_H];
static Canvas gCanvas;

static void writePPM(const char* path, const uint16_t* buf) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    printf("cannot write %s\n", path);
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", DP_W, DP_H);
  for (int i = 0; i < DP_W * DP_H; i++) {
    uint16_t v = buf[i];
    unsigned char rgb[3] = {
        (unsigned char)(((v >> 11) & 0x1F) * 255 / 31),
        (unsigned char)(((v >> 5) & 0x3F) * 255 / 63),
        (unsigned char)((v & 0x1F) * 255 / 31),
    };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static void setup(DpModel& m) {
  memset(&m, 0, sizeof(m));
  m.bridgeOnline = true;
  m.bridgeTs = 1794053962840ULL;
  m.localStampMs = 1000;
  m.count = 3;

  DpProvider& wb = m.items[0];
  wb.present = true;
  strcpy(wb.id, "workbuddy");
  strcpy(wb.name, "WorkBuddy");
  wb.state = DP_ST_THINKING;
  wb.health = DP_H_OK;
  wb.quotaKind = DP_Q_CREDIT;
  wb.stateSinceMs = m.bridgeTs - 214000;
  strcpy(wb.title, "开发 ESP32-S3 显示额度与进度的固件");
  wb.step = 3;
  strcpy(wb.tool, "write_file");
  wb.hasCtx = true;
  wb.ctxUsed = 127004;
  wb.ctxSize = 300000;
  wb.hasQuota = true;
  wb.used = 34.34f;
  wb.budget = 100;
  strcpy(wb.currency, "CNY");
  strcpy(wb.recent[0], "write_file");
  strcpy(wb.recent[1], "bash");
  strcpy(wb.recent[2], "!read_file");

  DpProvider& ds = m.items[1];
  ds.present = true;
  strcpy(ds.id, "deepseek");
  strcpy(ds.name, "DeepSeek");
  ds.state = DP_ST_TOOL;
  ds.health = DP_H_WARN;
  ds.quotaKind = DP_Q_BALANCE;
  ds.stateSinceMs = m.bridgeTs - 134000;
  strcpy(ds.title, "ADB 升级 com.gathin.tvhome 至 3.3.0 官方版");
  ds.step = 7;
  strcpy(ds.tool, "bash");
  ds.hasQuota = true;
  ds.remaining = 18.35f;
  ds.spentToday = 2.61f;
  ds.hasBurn = true;
  ds.burnDays = 33;
  ds.jobs = 2;
  strcpy(ds.currency, "CNY");
  strcpy(ds.recent[0], "bash");
  strcpy(ds.recent[1], "bash");
  strcpy(ds.recent[2], "job_output");

  DpProvider& km = m.items[2];
  km.present = true;
  strcpy(km.id, "kimi");
  strcpy(km.name, "Kimi");
  km.state = DP_ST_IDLE;
  km.health = DP_H_OK;
  km.quotaKind = DP_Q_CREDIT;
  km.stateSinceMs = m.bridgeTs - 400000;
  strcpy(km.title, "Lv.10");
  km.hasQuota = true;
  km.used = 0;
  km.budget = 50;
  strcpy(km.currency, "CNY");
}

int main() {
  gCanvas.attach(gBuf);
  DpModel m;
  setup(m);

  UiInput in{};
  in.model = &m;
  in.nowMs = m.bridgeTs;
  in.wifiOk = true;
  in.rssi = -58;
  in.bridgeHost = "192.168.100.151";
  in.ip = "192.168.100.142";
  in.fwVersion = "0.1.0";
  in.brightness = 60;
  in.clockText = "20:19";
  in.power = "USB";
  in.rotation = 0;
  in.rotOffset = 2;
  in.imuOk = true;

  in.page = 0;
  in.selected = -1;
  uiRender(gCanvas, in);
  writePPM("preview_overview.ppm", gBuf);

  in.page = 1;
  in.selected = 1;
  uiRender(gCanvas, in);
  writePPM("preview_detail.ppm", gBuf);

  in.page = 2;
  uiRender(gCanvas, in);
  writePPM("preview_info.ppm", gBuf);

  // 第四张：三种"结局"状态摆在一起，专门用来看状态胶囊的配色对不对。
  // 改 badgeStyle() 之后先看这张，别直接上机。
  in.page = 0;
  m.items[0].state = DP_ST_DONE;
  m.items[1].state = DP_ST_AWAITING;
  m.items[2].state = DP_ST_ERROR;
  uiRender(gCanvas, in);
  writePPM("preview_states.ppm", gBuf);

  // 第五张：桥接断掉时的样子（关掉电脑/退出 WorkBuddy 后就是这个）
  in.page = 0;
  m.bridgeOnline = false;
  uiRender(gCanvas, in);
  writePPM("preview_offline.ppm", gBuf);

  printf("wrote 5 previews\n");
  return 0;
}
