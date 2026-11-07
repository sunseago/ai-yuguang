#!/usr/bin/env python3
"""PPM -> PNG（只依赖标准库）。用于把 UI 预览图变成能直接看的文件。"""
import sys
import zlib
import struct


def read_ppm(path):
    with open(path, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline().split()
        while len(line) < 2:
            line += f.readline().split()
        w, h = int(line[0]), int(line[1])
        f.readline()
        data = f.read(w * h * 3)
    return w, h, data


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    scale = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    for name in ['preview_overview', 'preview_detail', 'preview_info', 'preview_states',
                 'preview_offline', 'preview_await_on', 'preview_await_off',
                 'preview_await_steady']:
        try:
            w, h, data = read_ppm(name + '.ppm')
        except FileNotFoundError:
            continue
        if scale > 1:
            out = bytearray()
            for y in range(h):
                row = data[y * w * 3:(y + 1) * w * 3]
                big = bytearray()
                for x in range(w):
                    px = row[x * 3:x * 3 + 3]
                    big += px * scale
                for _ in range(scale):
                    out += big
            write_png(name + '.png', w * scale, h * scale, bytes(out))
        else:
            write_png(name + '.png', w, h, data)
        print('->', name + '.png')


if __name__ == '__main__':
    main()
