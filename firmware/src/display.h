#pragma once
#include <stdint.h>

void displayInit();
uint16_t* displayBuffer();          // PSRAM 里的整屏画布
void displayPush(const uint16_t* b);  // 一次性把整屏推到屏上
void displayBrightness(uint8_t percent);
void displayFill(uint16_t color);

// 画面朝向：0..3 表示推屏时顺时针旋转 90°×n。
// 面板本身始终以 rotation 0 驱动，旋转在软件里做 —— 这块屏是 240x240 正方形，
// 自己转像素比让 Arduino_GFX 处理旋转偏移（这块屏 row_offset=80）更可控。
void displaySetRotation(uint8_t r);
uint8_t displayRotation();

// 把屏幕坐标反变换回 UI 逻辑坐标（触摸用）
void displayToLogical(int& x, int& y);
