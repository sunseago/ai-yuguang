// AI余光 的数据模型：纯 POD，不依赖 Arduino，方便在电脑上跑 UI 预览。
#pragma once
#include <stdint.h>
#include <string.h>

enum DpState : uint8_t {
  DP_ST_IDLE = 0,
  DP_ST_THINKING,
  DP_ST_TOOL,
  DP_ST_AWAITING,
  DP_ST_DONE,
  DP_ST_ERROR,
  DP_ST_OFFLINE,
};

enum DpHealth : uint8_t {
  DP_H_OK = 0,
  DP_H_WARN,
  DP_H_CRITICAL,
  DP_H_UNKNOWN,
};

enum DpQuotaKind : uint8_t {
  DP_Q_NONE = 0,
  DP_Q_CREDIT,
  DP_Q_BALANCE,
};

struct DpProvider {
  bool present;
  char id[12];
  char name[20];
  uint8_t state;
  uint8_t health;
  uint8_t quotaKind;
  uint64_t stateSinceMs;
  char title[80];
  int16_t step;
  char tool[20];
  bool hasCtx;
  int32_t ctxUsed;
  int32_t ctxSize;
  float remaining;
  float budget;
  float used;
  float spentToday;
  bool hasBurn;
  float burnDays;
  char currency[6];
  int16_t jobs;
  char recent[3][16];
  bool hasQuota;
};

struct DpModel {
  bool bridgeOnline;
  uint64_t bridgeTs;       // 桥接时间（epoch ms），用于校时
  uint64_t localStampMs;   // 收到这份数据时的本机 millis()
  int32_t hostIdleS;        // 电脑距离上次键鼠输入的秒数；-1 = 读不到
  int count;
  DpProvider items[5];
};

inline const char* dpStateLabel(uint8_t s) {
  switch (s) {
    case DP_ST_IDLE: return "IDLE";
    case DP_ST_THINKING: return "THINKING";
    case DP_ST_TOOL: return "TOOL";
    case DP_ST_AWAITING: return "WAIT";
    case DP_ST_DONE: return "DONE";
    case DP_ST_ERROR: return "ERROR";
    default: return "OFFLINE";
  }
}
