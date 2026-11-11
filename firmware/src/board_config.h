// 板级选择：默认 LCD-1.54，编译时 -DBOARD_AMOLED216 切到 AMOLED-2.16。
#pragma once

#if defined(BOARD_AMOLED216)
#include "board_amoled216.h"
#else
#include "board_lcd154.h"
#endif

// ---- 屏幕电源策略（详见 screenlogic.h / screenpower.cpp）----
// 核心：跟"电脑用不用"走，不跟"插没插电"走。
//
// 三档：ON（正常）→ DIM（15%）→ OFF（熄屏）。
// 进入 OFF 有两个触发：
//   ① 断链 —— 电脑关机 / 系统真睡了 / 只插充电器但电脑没在跑
//   ② 闲置过久 —— 电脑开着但人走开足够久（等价于 Mac 显示器休眠，
//      默认 displaysleep=30 分钟）。很多机器系统被别的 app 拉着根本不睡，
//      断链信号一直不来，只能靠这个兜底。
//
// 墙插（USB 供电或充电器）用宽松档，电池用收紧档。
#define SCR_DIM_AFTER_IDLE_WALL_MS 300000UL    // 墙插：5 分钟没输入 → 变暗
#define SCR_DIM_AFTER_IDLE_BATT_MS 60000UL     // 电池：1 分钟没输入 → 变暗
#define SCR_OFF_AFTER_IDLE_WALL_MS 1800000UL   // 墙插：30 分钟没输入 → 熄屏（≈显示器休眠）
#define SCR_OFF_AFTER_IDLE_BATT_MS 600000UL    // 电池：10 分钟没输入 → 熄屏
#define SCR_OFF_AFTER_NOLINK_MS 60000UL        // 断链 60 秒 → 熄屏（容忍短暂抖动）
#define SCR_DIM_PERCENT 15                     // 暗档亮度
#define SCR_WAKE_PEEK_MS 10000UL               // 熄屏后点一下，亮 10 秒
#define SCR_OFF_FADE_MS 1500                   // 熄屏前先压到 5% 再灭，别瞬间黑
#define SCR_OFF_FADE_PERCENT 5
#define SCR_MAX_LEVEL 100                      // 亮度上限（百分比）

// ---- 圆角屏安全边距（逻辑坐标）----
// 圆角屏的四个角会被玻璃切掉，状态栏文字/图标不能压到角上。
// 半径按逻辑像素换算（物理半径 / 放大倍数）；直角屏为 0。
#ifdef LCD_CORNER_RADIUS
#define DP_SAFE_MARGIN (LCD_CORNER_RADIUS / DP_UI_SCALE)
#else
#define DP_SAFE_MARGIN 0
#endif

// 版本号由 platformio.ini 的 extra_scripts 在编译前生成（来源 bridge/version.json）。
// 保留 fallback 是为了让 IDE/单独编译头文件时不至于报未定义。
#if __has_include("version_gen.h")
#include "version_gen.h"
#endif
#ifndef DP_FW_VERSION
#define DP_FW_VERSION "dev"
#endif
