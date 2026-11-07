// 屏幕电源策略：跟"电脑用不用"走，而不是跟"插没插电"走。
//
// 四个输入：
//   link      桥接通不通（有数据 = 电脑上的 AI 在跑）
//   usbHost   USB 线另一头有没有一台醒着的电脑（Mac 睡眠时掉下来）
//   hostIdle  电脑多久没键鼠输入（由桥接读 HIDIdleTime 带来，-1 = 读不到）
//   触摸/按键  有人正在看这块板子
//
// 四种状态：
//   ON    正常亮度
//   DIM   暗到 15%：电脑开着但人走开了，扫一眼仍能读
//   FADE  熄屏前的过渡（压到 5%，1.5 秒）
//   OFF   背光关掉
//
// 为什么"变暗"和"熄屏"要分成两档：link 断和"人离开"不是一回事。
// 人离开时 Mac 还在跑、数据是对的，只是没人看 → 变暗即可；
// 电脑停了数据就是死的，留着亮没意义 → 熄屏。
#pragma once
#include <stdint.h>

#include "model.h"

void screenPowerInit();

// 有触摸/按键时调用。熄屏状态下这一下只负责唤醒，不当作翻页/点卡片。
void screenPowerNoteInput();

// 每个主循环调一次。basePercent 是用户用 BOOT 键选的那个亮度档。
void screenPowerUpdate(const DpModel& m, bool linkOk, bool usbHost, uint8_t basePercent);

bool screenPowerIsOff();

// 当前是否处于"电脑在但人走开"的暗档（给心跳日志用）
bool screenPowerIsDim();

// 当前实际下发的背光百分比（0 表示已熄屏）
uint8_t screenPowerLevel();
