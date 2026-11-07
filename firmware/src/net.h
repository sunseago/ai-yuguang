#pragma once
#include "model.h"

void netBegin();          // 读 NVS 配置；没有配置就进配网模式
void netLoop();           // 维护 WiFi 连接
bool netFetch(DpModel& m);  // 拉一次 /v1/state
bool netOnline();
int8_t netRssi();
const char* netIp();
const char* netBridgeHost();
const char* netClockText();  // 桥接给的本地时间 "20:19"
void netClearConfig();       // 清掉 WiFi 配置，重启进配网
void netSetHost(const char* host);
