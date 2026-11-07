#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "rotate.h"

static Arduino_DataBus* s_bus = nullptr;
static Arduino_GFX* s_gfx = nullptr;
static uint16_t* s_canvas = nullptr;
static uint16_t* s_rotBuf = nullptr;
static uint8_t s_rot = 0;

void displayInit() {
  s_bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
  // 240x240 是 240x320 GRAM 的窗口；rotation 0 时偏移为 0,0，行偏移 80 只影响旋转 2/3。
  // ips=true：这块面板需要反色，与实测配置一致。
  s_gfx = new Arduino_ST7789(s_bus, LCD_RST, 0 /* rotation */, true /* ips */, LCD_W, LCD_H, 0, 0, 0, 80);
  s_gfx->begin(80000000);  // 80MHz 写时钟，实测可用
  s_gfx->fillScreen(0xFFFF);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LCD_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(LCD_BL, 0);
#endif
  displayBrightness(60);
}

uint16_t* displayBuffer() {
  if (!s_canvas) {
    // 整屏 240*240*2 = 115200 字节，放 PSRAM，避免挤占内部 RAM
    s_canvas = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas) s_canvas = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_8BIT);
    if (s_canvas) memset(s_canvas, 0xFF, LCD_W * LCD_H * 2);
  }
  return s_canvas;
}

void displayPush(const uint16_t* b) {
  if (!s_gfx || !b) return;
  if (s_rot == 0) {
    s_gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)b, LCD_W, LCD_H);
    return;
  }

  if (!s_rotBuf) {
    s_rotBuf = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rotBuf) s_rotBuf = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_8BIT);
  }
  if (!s_rotBuf) {  // 内存不够就退化成不旋转，宁可歪着也要有画面
    s_gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)b, LCD_W, LCD_H);
    return;
  }

  // 旋转公式统一放在 rotate.h —— 那里有主机端单元测试兜着。
  dpRotateFrame(s_rotBuf, b, LCD_W, s_rot);
  s_gfx->draw16bitRGBBitmap(0, 0, s_rotBuf, LCD_W, LCD_H);
}

void displaySetRotation(uint8_t r) { s_rot = r & 3; }

uint8_t displayRotation() { return s_rot; }

void displayToLogical(int& x, int& y) {
  if (s_rot == 0) return;
  dpUnrotate(x, y, LCD_W, s_rot);
}

void displayBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint32_t duty = (uint32_t)255 * percent / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LCD_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}

void displayFill(uint16_t color) {
  if (s_gfx) s_gfx->fillScreen(color);
}
