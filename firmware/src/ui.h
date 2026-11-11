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
  const char* sound;   // 提示音摘要，如 "水滴 / 海浪"
  uint8_t sndSlot;     // 设置页正在编辑哪个槽：0=弹窗音 1=完成音
  uint8_t alertSnd;    // 弹窗音编号（0..12）
  uint8_t doneSnd;     // 完成音编号（0..12）
  uint8_t volume;      // 0..100
  bool volFocus;       // 长按 PLUS 进来的：高亮音量条
};

void uiRender(Canvas& c, const UiInput& in);

// 设置页命中测试：音效格子返回编号（0..12），没点到返回 -1
int uiHitSoundCell(int x, int y);
// 设置页命中测试：槽位切换按钮，返回 0=弹窗 1=完成，没点到返回 -1
int uiHitSoundSlot(int x, int y);
// 设置页命中测试：音量，返回 -1 减 / +1 加 / 0 没点到
int uiHitVolume(int x, int y);

// 命中测试：返回卡片下标，-1 表示没点到卡片
int uiHitCard(const DpModel& m, int x, int y);

// 是否有卡片正在"等确认"的呼吸闪烁中（据此决定要不要加快重绘）
bool uiWantsFastRedraw(const DpModel& m, uint64_t nowMs);
