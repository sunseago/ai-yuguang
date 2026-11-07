#include "canvas.h"

#include <string.h>

#include "font_data.h"

// ---------- 基础图元 ----------

void Canvas::clear(uint16_t c) {
  if (!buf_) return;
  for (int i = 0; i < DP_W * DP_H; i++) buf_[i] = c;
}

void Canvas::px(int x, int y, uint16_t c) {
  if (!buf_ || x < 0 || y < 0 || x >= DP_W || y >= DP_H) return;
  buf_[y * DP_W + x] = c;
}

void Canvas::fillRect(int x, int y, int w, int h, uint16_t c) {
  if (!buf_ || w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > DP_W ? DP_W : x + w;
  int y1 = y + h > DP_H ? DP_H : y + h;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* row = buf_ + yy * DP_W;
    for (int xx = x0; xx < x1; xx++) row[xx] = c;
  }
}

void Canvas::rect(int x, int y, int w, int h, uint16_t c) {
  hline(x, y, w, c);
  hline(x, y + h - 1, w, c);
  vline(x, y, h, c);
  vline(x + w - 1, y, h, c);
}

void Canvas::hline(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }

void Canvas::vline(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }

void Canvas::circle(int cx, int cy, int r, uint16_t c) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    px(cx + x, cy + y, c);
    px(cx + y, cy + x, c);
    px(cx - y, cy + x, c);
    px(cx - x, cy + y, c);
    px(cx - x, cy - y, c);
    px(cx - y, cy - x, c);
    px(cx + y, cy - x, c);
    px(cx + x, cy - y, c);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

void Canvas::fillCircle(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

void Canvas::fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  fillRect(x + r, y, w - 2 * r, h, c);
  fillRect(x, y + r, r, h - 2 * r, c);
  fillRect(x + w - r, y + r, r, h - 2 * r, c);
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      int ddx = r - 1 - dx, ddy = r - 1 - dy;
      if (ddx * ddx + ddy * ddy <= (r - 1) * (r - 1) + (r - 1)) {
        px(x + dx, y + dy, c);
        px(x + w - 1 - dx, y + dy, c);
        px(x + dx, y + h - 1 - dy, c);
        px(x + w - 1 - dx, y + h - 1 - dy, c);
      }
    }
  }
}

void Canvas::roundRect(int x, int y, int w, int h, int r, uint16_t border, uint16_t fill) {
  fillRoundRect(x, y, w, h, r, border);
  fillRoundRect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
}

// ---------- 文字 ----------

static const DpGlyph* findGlyph(uint16_t cp) {
  int lo = 0, hi = DP_GLYPH_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (kDpGlyphs[mid].cp == cp) return &kDpGlyphs[mid];
    if (kDpGlyphs[mid].cp < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return nullptr;
}

static int utf8Next(const char*& p, uint16_t* cp) {
  const unsigned char* s = (const unsigned char*)p;
  if (*s == 0) return 0;
  if (*s < 0x80) {
    *cp = *s;
    p += 1;
    return 1;
  }
  if ((*s & 0xE0) == 0xC0 && s[1]) {
    *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    p += 2;
    return 2;
  }
  if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) {
    *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    p += 3;
    return 3;
  }
  if ((*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    *cp = 0x25A1;  // 超 BMP 的字这里不处理，画方框
    p += 4;
    return 4;
  }
  p += 1;
  *cp = 0x25A1;
  return 1;
}

static int glyphWidth(uint16_t cp) { return cp < 0x80 ? 8 : 16; }

void Canvas::glyph(int x, int y, uint16_t cp, uint16_t c, int scale) {
  const DpGlyph* g = findGlyph(cp);
  if (!g) {
    // 缺字画一个方框，方便一眼看出是字库缺字而不是渲染坏了
    int w = glyphWidth(cp) * scale, h = 16 * scale;
    rect(x, y, w - 1, h - 1, dpcolor::FAINT);
    return;
  }
  const uint8_t* d = kDpGlyphData + g->off;
  int rows = 16;
  int bytesPerRow = g->w / 8;
  for (int r = 0; r < rows; r++) {
    for (int b = 0; b < bytesPerRow; b++) {
      uint8_t byte = d[r * bytesPerRow + b];
      for (int bit = 0; bit < 8; bit++) {
        if (!(byte & (0x80 >> bit))) continue;
        int gx = x + (b * 8 + bit) * scale;
        int gy = y + r * scale;
        if (scale == 1) {
          px(gx, gy, c);
        } else {
          fillRect(gx, gy, scale, scale, c);
        }
      }
    }
  }
}

int Canvas::textWidth(const char* utf8, int scale) const {
  if (!utf8) return 0;
  int w = 0;
  const char* p = utf8;
  uint16_t cp;
  while (utf8Next(p, &cp)) w += glyphWidth(cp) * scale;
  return w;
}

void Canvas::text(int x, int y, const char* utf8, uint16_t c, int scale) {
  if (!utf8) return;
  const char* p = utf8;
  uint16_t cp;
  int cx = x;
  while (utf8Next(p, &cp)) {
    glyph(cx, y, cp, c, scale);
    cx += glyphWidth(cp) * scale;
  }
}

void Canvas::textAligned(int x, int y, int w, const char* utf8, uint16_t c, int scale, DpAlign align) {
  int tw = textWidth(utf8, scale);
  int tx = x;
  if (align == DP_CENTER) tx = x + (w - tw) / 2;
  else if (align == DP_RIGHT) tx = x + w - tw;
  text(tx, y, utf8, c, scale);
}

void Canvas::textEllipsis(int x, int y, int maxW, const char* utf8, uint16_t c, int scale) {
  if (!utf8) return;
  if (textWidth(utf8, scale) <= maxW) {
    text(x, y, utf8, c, scale);
    return;
  }
  int dotsW = 8 * 2 * scale;  // ".."
  const char* p = utf8;
  uint16_t cp;
  int cx = x;
  while (utf8Next(p, &cp)) {
    int gw = glyphWidth(cp) * scale;
    if (cx + gw + dotsW > x + maxW) break;
    glyph(cx, y, cp, c, scale);
    cx += gw;
  }
  text(cx, y, "..", c, scale);
}

int Canvas::textWrap(int x, int y, int maxW, int lineH, const char* utf8, uint16_t c, int scale, int maxLines) {
  if (!utf8 || maxLines <= 0) return 0;
  const char* p = utf8;
  int line = 0;
  int cx = 0;
  int lastSpaceX = -1;
  const char* lastSpaceP = nullptr;
  while (*p && line < maxLines) {
    const char* before = p;
    uint16_t cp;
    utf8Next(p, &cp);
    int gw = glyphWidth(cp) * scale;
    if (cx + gw > maxW) {
      // 折行
      if (cp == ' ' || cp == '\n') {
        cx = 0;
        line++;
        y += lineH;
        continue;
      }
      if (line == maxLines - 1) break;
      cx = 0;
      line++;
      y += lineH;
      if (cp == ' ') continue;
    }
    if (cp == '\n') {
      cx = 0;
      line++;
      y += lineH;
      continue;
    }
    glyph(x + cx, y, cp, c, scale);
    cx += gw;
    (void)before;
    (void)lastSpaceX;
    (void)lastSpaceP;
  }
  if (*p && line >= maxLines - 1) textEllipsis(x + cx, y, maxW - cx, "..", c, scale);
  return line + 1;
}

// ---------- 进度条 ----------

void Canvas::bar(int x, int y, int w, int h, float frac, uint16_t fg, uint16_t bg) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  fillRect(x, y, w, h, bg);
  int fw = (int)(w * frac + 0.5f);
  if (fw > 0) fillRect(x, y, fw, h, fg);
}
