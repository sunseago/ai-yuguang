// 屏幕电源策略：跟"电脑用不用"走，而不是跟"插没插电"走。
//
// 输入：
//   link       桥接通不通（有数据 = 电脑上的 AI 在跑）
//   hostIdle   电脑多久没键鼠输入（由桥接读 HIDIdleTime 带来，-1 = 读不到）
//   onBattery  是否电池供电（决定阈值档：墙插宽松、电池收紧）
//   触摸/按键  有人正在看这块板子
//
// 四种状态（见 screenlogic.h）：ON / DIM / FADE / OFF。
//
// 除此之外还有一个"手动关机"开关：PWR 长按 1 秒翻转。手动关机时强制熄屏，
// 自动策略暂停，触摸不唤醒——只有再长按一次 PWR 才开机。
#pragma once
#include <stdint.h>

#include "model.h"

void screenPowerInit();

// 有触摸/按键时调用。熄屏状态下这一下只负责唤醒，不当作翻页/点卡片。
void screenPowerNoteInput();

// 每个主循环调一次。basePercent 是用户用 BOOT 键选的那个亮度档。
void screenPowerUpdate(const DpModel& m, bool linkOk, bool onBattery, uint8_t basePercent);

// PWR 长按：手动开机/关机翻转。关机时强制熄屏且不被自动策略唤醒。
void screenPowerToggleManualOff();

// 背光是否已关（手动关机或自动熄屏都算）
bool screenPowerIsOff();

// 是否处于手动关机状态（此时触摸/按键不唤醒，只有 PWR 长按能开机）
bool screenPowerIsManualOff();

// 当前是否处于"电脑在但人走开"的暗档（给心跳日志用）
bool screenPowerIsDim();

// 当前实际下发的背光百分比（0 表示已熄屏）
uint8_t screenPowerLevel();
