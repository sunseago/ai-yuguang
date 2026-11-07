#pragma once
#include "canvas.h"
#include "model.h"

struct UiInput {
  const DpModel* model;
  int page;          // 0=总览 1..n=详情 2=信息页由 page 偏移决定
  int selected;      // 详情页对应下标
  int pendingIndex;  // 总览页高亮下标（触摸按下时）
  uint64_t nowMs;    // 已校时的 epoch ms
  bool wifiOk;
  int8_t rssi;
  const char* bridgeHost;
  const char* ip;
  const char* fwVersion;
  uint8_t brightness;
  const char* clockText;
  const char* power;   // "USB" / "USB 87%" / "BAT 42%"
  uint8_t rotation;    // 0..3，画面顺时针转了多少个 90°
  uint8_t rotOffset;   // 0..3，整体偏移（信息页现场校准用）
  bool imuOk;          // IMU 是否可用
};

void uiRender(Canvas& c, const UiInput& in);

// 命中测试：返回卡片下标，-1 表示没点到卡片
int uiHitCard(const DpModel& m, int x, int y);

// 是否有卡片正在"等确认"的呼吸闪烁中（据此决定要不要加快重绘）
bool uiWantsFastRedraw(const DpModel& m, uint64_t nowMs);
