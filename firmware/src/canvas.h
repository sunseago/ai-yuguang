// 极简绘图库：一块 240x240 的 RGB565 内存画布 + 点阵文字。
// 不依赖 Arduino / LVGL，因此可以直接在电脑上编译出 UI 预览图。
#pragma once
#include <stdint.h>

#define DP_W 240
#define DP_H 240

// RGB565
constexpr uint16_t dpRGB(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace dpcolor {
constexpr uint16_t BG = dpRGB(0xF6, 0xF7, 0xF9);
constexpr uint16_t CARD = dpRGB(0xFF, 0xFF, 0xFF);
constexpr uint16_t TEXT = dpRGB(0x16, 0x18, 0x1D);
constexpr uint16_t MUTED = dpRGB(0x6B, 0x72, 0x80);
constexpr uint16_t FAINT = dpRGB(0x9C, 0xA3, 0xAF);
constexpr uint16_t LINE = dpRGB(0xE3, 0xE5, 0xE9);
constexpr uint16_t TRACK = dpRGB(0xE8, 0xEA, 0xEE);
constexpr uint16_t IDLE = dpRGB(0x9C, 0xA3, 0xAF);
constexpr uint16_t THINK = dpRGB(0x3B, 0x82, 0xF6);
constexpr uint16_t TOOL = dpRGB(0x06, 0xB6, 0xD4);
constexpr uint16_t WAIT = dpRGB(0xF5, 0x9E, 0x0B);
constexpr uint16_t DONE = dpRGB(0x10, 0xB9, 0x81);
constexpr uint16_t ERR = dpRGB(0xEF, 0x44, 0x44);
// 状态胶囊：任务进行中用红、结束用绿。单独开一个常量，跟进度条那些语义色分开，
// 免得以后调进度条颜色把状态标签一起改了。
constexpr uint16_t RUN = dpRGB(0xDC, 0x35, 0x35);
constexpr uint16_t RUN_SOFT = dpRGB(0xFD, 0xEC, 0xEC);  // 淡红底，给浅色场景用
constexpr uint16_t DONE_SOFT = dpRGB(0xE7, 0xF8, 0xF1);
// 闪烁的"暗相"边框不能直接用 *_SOFT：那在白卡上几乎等于消失，
// 看起来是"闪没了"而不是"变暗"。用中间调，暗相仍能看出边界。
constexpr uint16_t RUN_MID = dpRGB(0xF7, 0xC5, 0xC5);
constexpr uint16_t DONE_MID = dpRGB(0xA6, 0xE3, 0xCC);
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t DARKPANEL = dpRGB(0x11, 0x14, 0x1A);
}  // namespace dpcolor

enum DpAlign { DP_LEFT = 0, DP_CENTER, DP_RIGHT };

class Canvas {
 public:
  void attach(uint16_t* mem) { buf_ = mem; }
  uint16_t* buffer() { return buf_; }
  bool ready() const { return buf_ != nullptr; }

  void clear(uint16_t c);
  void px(int x, int y, uint16_t c);
  void fillRect(int x, int y, int w, int h, uint16_t c);
  void rect(int x, int y, int w, int h, uint16_t c);
  void hline(int x, int y, int w, uint16_t c);
  void vline(int x, int y, int h, uint16_t c);
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c);
  void roundRect(int x, int y, int w, int h, int r, uint16_t border, uint16_t fill);
  void circle(int cx, int cy, int r, uint16_t c);
  void fillCircle(int cx, int cy, int r, uint16_t c);

  // 点阵文字：ASCII 8x16，汉字 16x16；scale 为整数放大倍率
  int textWidth(const char* utf8, int scale = 1) const;
  int textHeight(int scale = 1) const { return 16 * scale; }
  void text(int x, int y, const char* utf8, uint16_t c, int scale = 1);
  void textAligned(int x, int y, int w, const char* utf8, uint16_t c, int scale, DpAlign align);
  // 超出 maxW 时截断并补 ".."
  void textEllipsis(int x, int y, int maxW, const char* utf8, uint16_t c, int scale = 1);
  // 按宽度折行，返回实际行数；超过 maxLines 时最后一行加省略号
  int textWrap(int x, int y, int maxW, int lineH, const char* utf8, uint16_t c, int scale, int maxLines);

  void bar(int x, int y, int w, int h, float frac, uint16_t fg, uint16_t bg);

 private:
  void glyph(int x, int y, uint16_t cp, uint16_t c, int scale);
  uint16_t* buf_ = nullptr;
};
