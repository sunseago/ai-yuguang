// Waveshare ESP32-S3-Touch-LCD-1.54 板级定义
// 引脚取自该板在开源固件 Clawdmeter 中已硬件验证的 board.h（其配置又来自 BambuHelper
// 的 ws_lcd_154），显示、触摸、按键、电池 ADC 都被实测过。不要凭印象改这些值。
#pragma once

// ---- 屏幕：ST7789，四线 SPI（不是 QSPI）----
#define LCD_W 240
#define LCD_H 240
#define LCD_CS 21
#define LCD_SCLK 38
#define LCD_MOSI 39
#define LCD_DC 45
#define LCD_RST 40
#define LCD_BL 46  // 背光，LEDC PWM（屏本身没有亮度指令）

// ---- I2C：触摸 / 音频 / IMU 共用 ----
#define IIC_SDA 42
#define IIC_SCL 41

// ---- 触摸：CST816T ----
#define TP_INT 48
#define TP_RST 47
#define CST816_ADDR 0x15

// ---- 电池 ----
// BAT_EN 是电源保持脚，必须在启动早期拉高，否则用电池供电会掉电重启。
#define BAT_EN 2
#define BAT_ADC_PIN 1
#define BAT_VOLT_DIVIDER 3.0f

// ---- IMU：QMI8658，挂在触摸同一条 I2C 上 ----
// 地址来自微雪官方 demo 与原理图：SA0 拉高时为 0x6B，个别批次是 0x6A，初始化时自动探测。
#define QMI8658_ADDR_HIGH 0x6B
#define QMI8658_ADDR_LOW 0x6A
#define QMI8658_WHO_AM_I 0x05
// 芯片 X/Y 轴相对屏幕的方向不一定和直觉一致，实测后在这里补偏移（0..3）。
// 旋转值按 (IMU_ROT_OFFSET + 重力象限) & 3 计算。出厂默认 2。
//
// 为什么默认是 2：象限映射本身已经被真机四姿态验证过（四个方向各差 90°，顺序正确），
// 但整块面板的"正上方"相对 IMU 坐标系的绝对朝向只能实测确定。实测四个方向**一致地**
// 差 180°，所以整体加 2。这只影响绝对朝向，不影响四个方向之间的相对关系。
//
// 这个值可以在设备上直接改：翻到信息页（第 3 页），短按 BOOT 键循环 0→1→2→3，
// 画面立刻转动，选到正的为止，会自动存进 NVS。改这里只是改出厂值。
#define IMU_ROT_OFFSET 2

// ---- 屏幕电源策略（详见 screenpower.cpp）----
// 核心：跟"电脑用不用"走，不跟"插没插电"走。
//
// 电脑在（USB 主机枚举着）时：
//   5 分钟没键鼠输入 → 暗到 15%
//   桥接断了 → 保持亮着显示 NO LINK，15 分钟后熄（给你留时间去修）
// 电脑不在（Mac 睡眠/关机、只插充电器、纯电池）时：
//   桥接一断，15 秒就熄 —— 数据已经是死的，亮着纯粹浪费
// 电池供电时阈值收紧（没有墙插兜底，省电优先）：
//   1 分钟没输入就暗
#define SCR_DIM_AFTER_HOST_IDLE_MS 300000UL      // USB：5 分钟无输入变暗
#define SCR_DIM_AFTER_HOST_IDLE_BAT_MS 60000UL   // 电池：1 分钟无输入变暗
#define SCR_DIM_PERCENT 15                       // 暗档亮度
#define SCR_OFF_AFTER_NOLINK_USB_MS 900000UL     // USB：断链 15 分钟熄屏
#define SCR_OFF_AFTER_NOLINK_BAT_MS 15000UL      // 无 USB 主机：断链 15 秒熄屏
#define SCR_WAKE_PEEK_MS 10000UL                 // 熄屏后点一下，亮 10 秒
#define SCR_OFF_FADE_MS 1500                     // 熄屏前先压到 5% 再灭，别瞬间黑
#define SCR_OFF_FADE_PERCENT 5
#define SCR_MAX_LEVEL 100                        // 亮度上限（百分比）

// ---- 按键（低电平有效）----
#define BTN_BOOT 0   // 短按切亮度，长按 5s 重新配网
#define BTN_USER 5   // 短按切页
#define BTN_PWR 4    // 备用

#define DP_FW_VERSION "0.1.3"
