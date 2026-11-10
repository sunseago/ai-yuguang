#pragma once
#include "model.h"

void netBegin();          // 读 NVS 配置；没有配置就进配网模式
void netLoop();           // 维护 WiFi 连接
bool netFetch(DpModel& m);  // 拉一次 /v1/state

// 把 netLoop + netFetch 搬到一个独立任务里，UI 循环从此不会被网络阻塞。
// 为什么必须这么做：http.GET() 是同步的，主机可达但不回应时会吃满 2.5 秒超时。
// 之前它跑在 loop() 里，等于每 2 秒冻结界面 2.5 秒——触摸（边沿检测）、
// IMU 轮询（200ms 一次 + 连续 5 次稳定）、重绘全部被饿死。
// 返回 false 表示任务创建失败（此时界面会停在开机快照上，调用方要报出来）
bool netStartPolling();
// setup() 里首次拉取成功后把结果交给任务，避免任务第一次判离线时把界面清空
void netPublishInitial(const DpModel& m);
// 有新数据就拷进 out 并返回 true。内部走临界区，可以随时调用。
bool netTakeSnapshot(DpModel& out);

bool netOnline();
int8_t netRssi();
const char* netIp();
const char* netBridgeHost();
const char* netClockText();  // 桥接给的本地时间 "20:19"
void netClearConfig();       // 清掉 WiFi 配置，重启进配网
void netSetHost(const char* host);
