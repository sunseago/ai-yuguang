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

// USB 线另一头是不是一台**醒着的电脑**。
//
// 和 powerOnUsb() 的区别很关键：插充电器 powerOnUsb() 为真，但这里为假。
// 判据只有 Serial.isPlugged()（USB SOF 中断维护）——Mac 睡眠时总线挂起，
// 这个值会掉下来。熄屏策略要的是"电脑在不在"，不是"有没有电"。
bool powerUsbHostPresent();

// 给屏幕显示用的短标签："USB" / "BAT 87%" / "BAT ?"
const char* powerLabel();

// 电池百分比（0..100），未知返回 -1
int8_t powerBatteryPercent();
