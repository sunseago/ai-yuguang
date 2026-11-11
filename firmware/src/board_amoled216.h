// Waveshare ESP32-S3-Touch-AMOLED-2.16 板级定义
// 480x480 AMOLED（CO5300，QSPI）+ CST9220 触摸 + AXP2101 电源 + QMI8658。
// 引脚来自微雪官方示例与产品页，不要凭印象改。
#pragma once

#define BOARD_NAME "ESP32-S3-Touch-AMOLED-2.16"
#define BOARD_AMOLED 1

// ---- 屏幕：CO5300，QSPI（四数据线）----
#define LCD_W 480
#define LCD_H 480
#define LCD_CS 12
#define LCD_SCLK 38
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RST 39
// AMOLED 没有背光引脚，亮度是面板命令（0x51），由 display.cpp 调 setBrightness。
#define LCD_HAS_PANEL_BRIGHTNESS 1

// UI 仍按 240x240 逻辑坐标排版，推屏时整数放大 2 倍。
// 好处：布局/字体代码完全复用；坏处：字是 2 倍块状（16px→32px），但清晰可读。
#define DP_UI_SCALE 2

// 面板圆角半径（物理像素）。这块 AMOLED 是圆角屏，四角会被外壳/玻璃切掉，
// 内容画到最边上会被遮住。推屏时把四角按这个半径涂黑，并要求 UI 不要把
// 关键内容放进这个区域（见 ui.cpp 的 DP_SAFE_INSET）。
// 微雪没有公布半径，实测按 28px（约 1.7mm）覆盖得住。
#define LCD_CORNER_RADIUS 28

// ---- I2C：触摸 / IMU / PMIC / RTC 共用 ----
#define IIC_SDA 15
#define IIC_SCL 14

// ---- 触摸：CST9220（Hynitron，I2C 0x5A）----
#define TP_INT 11
#define TP_RST 40
#define CST92XX_ADDR 0x5A

// ---- 电源：AXP2101（I2C 0x34）----
// 没有 BAT_EN 保持脚，也没有电池分压 ADC —— 电压/充电状态都走 PMIC。
#define AXP2101_ADDR 0x34
#define PMU_I2C_ADDR 0x34

// ---- IMU：QMI8658，挂在同一条 I2C 上 ----
#define QMI8658_ADDR_HIGH 0x6B
#define QMI8658_ADDR_LOW 0x6A
#define QMI8658_WHO_AM_I 0x05
// 面板朝向和 IMU 坐标系的相对关系只能实测；先沿用 2，可用信息页 BOOT 键现场校准。
#define IMU_ROT_OFFSET 2

// ---- 按键 ----
// 板上丝印：BOOT=GPIO0，KEY=GPIO18。PWR 是 AXP2101 的 PWRON 信号（不是普通按键），
// 空闲时电平为低；当成低有效按键读会一直判定"按着"，1 秒后触发手动关机、屏幕变黑。
// 这里先禁用，后续如需开关机应走 AXP2101 的 PKEY 中断。
#define BTN_BOOT 0   // 短按切亮度，长按 5s 重新配网
#define BTN_USER 18  // 短按切页
#define BTN_PWR -1   // 未接入（见上）
