#!/usr/bin/env python3
"""从 Unifont .hex 生成固件用的 1bpp 点阵字库。

用法: python3 tools/gen_font.py /tmp/unifont.hex src/font_data.h

为什么用 Unifont：它本身就是 16px 的像素字体，1bpp 直接可读，
不像矢量字体缩小到 16px 那样糊成一团。ASCII 是 8x16，汉字是 16x16。
"""
import sys

ASCII = list(range(0x20, 0x7F))
SYMBOLS = [
    0x00A5,  # ¥
    0x00B0,  # °
    0x00B7,  # ·
    0x00D7,  # ×
    0x2014,  # —
    0x2018, 0x2019, 0x201C, 0x201D,  # 引号
    0x2026,  # …
    0x2190, 0x2191, 0x2192, 0x2193,  # 箭头
    0x2103,  # ℃
    0x25CF, 0x25CB,  # ● ○
]


def gb2312_codepoints():
    """GB2312 一二级汉字（6763 个），覆盖绝大多数中文文本。"""
    out = []
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('gb2312')
            except UnicodeDecodeError:
                continue
            cp = ord(ch)
            if 0x4E00 <= cp <= 0x9FFF:
                out.append(cp)
    return out


def parse_hex(path):
    glyphs = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp_s, bits = line.split(':', 1)
            try:
                cp = int(cp_s, 16)
            except ValueError:
                continue
            if len(bits) % 32 != 0:
                continue
            nbytes = len(bits) // 2
            if nbytes not in (16, 32):  # 8x16 或 16x16
                continue
            glyphs[cp] = bytes.fromhex(bits)
    return glyphs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    glyphs = parse_hex(src)

    wanted = []
    seen = set()

    def add(cps):
        for cp in cps:
            if cp in seen:
                continue
            if cp in glyphs:
                seen.add(cp)
                wanted.append(cp)

    add(ASCII)
    add(SYMBOLS)
    add(gb2312_codepoints())
    wanted.sort()

    data = bytearray()
    index = []
    for cp in wanted:
        g = glyphs[cp]
        width = 8 if len(g) == 16 else 16
        index.append((cp, width, len(data)))
        data.extend(g)

    with open(dst, 'w', encoding='utf-8') as f:
        f.write('// 自动生成，请勿手改：python3 tools/gen_font.py unifont.hex src/font_data.h\n')
        f.write('// 字型来源：GNU Unifont 16.0.04 (GPL-2.0+ with font embedding exception)\n')
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write('typedef struct { uint16_t cp; uint8_t w; uint32_t off; } DpGlyph;\n\n')
        f.write(f'#define DP_GLYPH_COUNT {len(index)}\n')
        f.write(f'#define DP_GLYPH_BYTES {len(data)}\n\n')
        f.write('static const DpGlyph kDpGlyphs[] = {\n')
        for cp, w, off in index:
            f.write(f'{{0x{cp:04X},{w},{off}}},\n')
        f.write('};\n\n')
        f.write('static const uint8_t kDpGlyphData[] = {\n')
        line = []
        for i, b in enumerate(data):
            line.append(f'{b}')
            if len(line) == 32:
                f.write(','.join(line) + ',\n')
                line = []
        if line:
            f.write(','.join(line) + ',\n')
        f.write('};\n')

    print(f'glyphs={len(index)} data={len(data)}B -> {dst}')


if __name__ == '__main__':
    sys.exit(main())
