// 供电来源与电池电压。
//
// 目的：判断"是不是插在电脑上"。插在电脑上时屏幕必须一直亮着，
// 电池供电时才允许按策略熄屏省电。
//
// 三条判据，任意一条成立就算 USB 供电：
//   1) USB Serial/JTAG 外设枚举到主机（本板固件就是用这个口打日志的）
//   2) 电池电压高于 4.15V —— 只有充电时才会这么高
//   3) 量不到电池（分压后接近 0）—— 没装电池，纯 USB 供电
#pragma once
#include <stdint.h>

void powerInit();

// 电池电压（V）。没装电池时返回 -1。
float powerBatteryVoltage();

// 是否 USB 供电（含"只插了充电器"的情况）
bool powerOnUsb();


// 给屏幕显示用的短标签："USB" / "BAT 87%" / "BAT ?"
const char* powerLabel();

// 电池百分比（0..100），未知返回 -1
int8_t powerBatteryPercent();

// 关机：长按 PWR 触发。清屏后进深度睡眠，再长按 PWR 唤醒（等于重新上电）。
// 没有 PWR 键的板子（BTN_PWR < 0）是空实现。
void powerOffDeepSleep();
