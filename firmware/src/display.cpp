#include "display.h"

// canvas.h 必须在 Arduino_GFX 之前：GFX 里有 `#define WHITE` 之类的颜色宏，
// 后包含会把 dpcolor::WHITE 这个标识符直接替换掉。
#include "canvas.h"   // DP_W / DP_H（逻辑画布尺寸）

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "rotate.h"

static Arduino_DataBus* s_bus = nullptr;
static Arduino_GFX* s_gfx = nullptr;
static uint16_t* s_canvas = nullptr;   // 逻辑画布 240x240
static uint16_t* s_rotBuf = nullptr;   // 逻辑画布旋转后的中间缓冲
#if defined(BOARD_AMOLED216)
static uint16_t* s_upBuf = nullptr;    // 2x 放大后的物理帧（480x480）
#endif
static uint8_t s_rot = 0;

void displayInit() {
#if defined(BOARD_AMOLED216)
  // CO5300：QSPI 四数据线，40MHz（微雪示例同款参数）
  s_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  // 注意参数顺序：bus, rst, rotation, ips, w, h, ...
  // 少写 ips 会让 480 落到 ips 上、height 变成 0 —— 初始化"成功"但什么都不显示。
  s_gfx = new Arduino_CO5300(s_bus, LCD_RST, 0 /* rotation */, false /* ips */, LCD_W, LCD_H, 0, 0, 0, 0);
  bool ok = s_gfx->begin(40000000);
  if (!ok) {
    Serial.println("[dp] panel begin failed @40MHz, retry @20MHz");
    ok = s_gfx->begin(20000000);
  }
  // 0xA0 = RGB 顺序 + 面板原生方向。不写它颜色会偏、方向也不对。
  s_bus->writeC8D8(0x36, 0xA0);
  s_gfx->displayOn();
  displayBrightness(60);
  Serial.printf("[dp] panel=CO5300 %dx%d begin=%d\n", (int)s_gfx->width(), (int)s_gfx->height(),
                ok ? 1 : 0);

  // 开机自检：闪 400ms 红屏。看到红屏说明 QSPI/面板/供电都通，黑屏则是硬件链路问题。
  s_gfx->fillScreen(0xF800);
  delay(400);
  s_gfx->fillScreen(0x0000);
#else
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
#endif
}

uint16_t* displayBuffer() {
  if (!s_canvas) {
    // 逻辑画布固定 240*240*2 = 115200 字节；AMOLED 板推屏时再放大到 480x480。
    s_canvas = (uint16_t*)heap_caps_malloc(DP_W * DP_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas) s_canvas = (uint16_t*)heap_caps_malloc(DP_W * DP_H * 2, MALLOC_CAP_8BIT);
    if (s_canvas) memset(s_canvas, 0xFF, DP_W * DP_H * 2);
  }
  return s_canvas;
}

#if defined(BOARD_AMOLED216)
// 逻辑 240 -> 物理 480：整数 2 倍最近邻放大。
// 按行成对写，比逐像素查表快，也不需要额外映射表。
//
// 顺手把四角涂黑：这块面板是圆角屏，画到角落的内容会被玻璃切掉。
// 半径用物理像素，逐角按 "圆心距 > r" 判定，只处理角落那个 r×r 的小方块，
// 整帧的开销可以忽略。
static void maskCorners() {
#if LCD_CORNER_RADIUS > 0
  const int r = LCD_CORNER_RADIUS;
  const int w = LCD_W, h = LCD_H;
  const uint16_t black = 0x0000;
  for (int y = 0; y < r; y++) {
    for (int x = 0; x < r; x++) {
      const int dx = r - 1 - x;
      const int dy = r - 1 - y;
      // 四角共用同一组 (dx,dy)：越界处（圆外）涂黑
      if ((long)dx * dx + (long)dy * dy > (long)r * r) {
        s_upBuf[y * w + x] = black;                     // 左上
        s_upBuf[y * w + (w - 1 - x)] = black;           // 右上
        s_upBuf[(h - 1 - y) * w + x] = black;           // 左下
        s_upBuf[(h - 1 - y) * w + (w - 1 - x)] = black; // 右下
      }
    }
  }
#endif
}

static bool upscale2x(const uint16_t* src) {
  if (!s_upBuf) {
    s_upBuf = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_upBuf) s_upBuf = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_8BIT);
  }
  if (!s_upBuf) return false;
  for (int y = 0; y < DP_H; y++) {
    const uint16_t* row = src + y * DP_W;
    uint16_t* d0 = s_upBuf + (y * 2) * LCD_W;
    uint16_t* d1 = d0 + LCD_W;
    for (int x = 0; x < DP_W; x++) {
      const uint16_t p = row[x];
      d0[x * 2] = p;
      d0[x * 2 + 1] = p;
      d1[x * 2] = p;
      d1[x * 2 + 1] = p;
    }
  }
  maskCorners();
  return true;
}
#endif

void displayPush(const uint16_t* b) {
  if (!s_gfx || !b) return;

#if defined(BOARD_AMOLED216)
  const uint16_t* src = b;
  if (s_rot) {
    if (!s_rotBuf) {
      s_rotBuf = (uint16_t*)heap_caps_malloc(DP_W * DP_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!s_rotBuf) s_rotBuf = (uint16_t*)heap_caps_malloc(DP_W * DP_H * 2, MALLOC_CAP_8BIT);
    }
    if (s_rotBuf) {
      dpRotateFrame(s_rotBuf, b, DP_W, s_rot);
      src = s_rotBuf;
    }
  }
  if (upscale2x(src)) {
    s_gfx->draw16bitRGBBitmap(0, 0, s_upBuf, LCD_W, LCD_H);
  } else {
    // 内存不够时退化：左上角原尺寸，至少还有画面
    s_gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)src, DP_W, DP_H);
  }
  return;
#else
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
#endif
}

void displaySetRotation(uint8_t r) { s_rot = r & 3; }

uint8_t displayRotation() { return s_rot; }

void displayToLogical(int& x, int& y) {
  // 触摸坐标是物理像素（AMOLED 480），先折回逻辑坐标（240），再反旋转
  if (DP_UI_SCALE > 1) {
    x /= DP_UI_SCALE;
    y /= DP_UI_SCALE;
  }
  if (s_rot == 0) return;
  dpUnrotate(x, y, DP_W, s_rot);
}

void displayBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
#if defined(BOARD_AMOLED216)
  // AMOLED 没有背光 PWM，亮度是面板命令
  if (s_gfx) ((Arduino_CO5300*)s_gfx)->setBrightness((uint8_t)(255u * percent / 100u));
#else
  uint32_t duty = (uint32_t)255 * percent / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LCD_BL, duty);
#else
  ledcWrite(0, duty);
#endif
#endif
}

void displayFill(uint16_t color) {
  if (s_gfx) s_gfx->fillScreen(color);
}
