#!/usr/bin/env python3
# 抓串口日志。
#
#     python3 tools/cap.py /dev/cu.usbmodem101 40
#
# 为什么不用 cat / stty / pio device monitor：
#   · HWCDC::write 里判断 isCDC_Connected()，为假时**直接丢弃**数据（不阻塞）。
#     所以"串口空白"往往是主机没把 DTR 拉起来，不是固件卡死。
#   · stty + cat 不会动 DTR；pio device monitor 需要 TTY，管道调用会抛 UserSideException。
#
# 依赖： python3 -m pip install --user pyserial
import sys
import time

import serial

if len(sys.argv) < 2:
    sys.exit("用法: python3 cap.py <串口> [秒数，默认 40]")

port = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.3
s.dtr = True   # 关键：不拉 DTR 就一个字节都收不到
s.rts = False  # 别动 RTS，某些板子的自动复位电路会因此重启
s.open()

t0 = time.time()
try:
    while time.time() - t0 < seconds:
        chunk = s.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
finally:
    s.close()
