#!/usr/bin/env python3
# 抓串口日志。
#
#     python3 tools/cap.py /dev/cu.usbmodem101 40
#     python3 tools/cap.py /dev/cu.usbmodem101 40 --no-reset   # 不碰 DTR/RTS，不触发复位
#
# 为什么不用 cat / stty / pio device monitor：
#   · HWCDC::write 里判断 isCDC_Connected()，为假时**直接丢弃**数据（不阻塞）。
#     所以"串口空白"往往是主机没把 DTR 拉起来，不是固件卡死。
#   · stty + cat 不会动 DTR；pio device monitor 需要 TTY，管道调用会抛 UserSideException。
#
# 两种模式：
#   默认（pyserial）：LCD-1.54（HWCDC）必须拉 DTR 才会把数据发出来；AMOLED-2.16 的
#     USB-JTAG 反过来，DTR=1 会把它按在复位里（一个字节都收不到），要传 0。
#   --no-reset：完全绕开 pyserial，os.open + select + read，全程不碰 DTR/RTS ——
#     USB CDC 的波特率是摆设，不用 stty。代价是 HWCDC 板（LCD-1.54）没 DTR 就收不到
#     数据，所以这个模式只适合 USB-Serial-JTAG 的板子（AMOLED-2.16）。
#     用来抓运行中的日志，避免每次开串口都把板子复位（rst:0x15 USB_UART_CHIP_RESET）。
#
# 依赖： python3 -m pip install --user pyserial（--no-reset 模式不需要）
import os
import select
import sys
import time

def usage():
    sys.exit("用法: python3 cap.py <串口> [秒数，默认 40] [dtr 0/1，默认 1] [--no-reset]")

argv = sys.argv[1:]
no_reset = "--no-reset" in argv
argv = [a for a in argv if not a.startswith("--")]
if len(argv) < 1:
    usage()

port = argv[0]
seconds = float(argv[1]) if len(argv) > 1 else 40.0
# LCD-1.54（HWCDC）必须拉 DTR 才会把数据发出来；AMOLED-2.16 的 USB-JTAG
# 反过来，DTR=1 会把它按在复位里（一个字节都收不到），要传 0。
dtr = (argv[2] != "0") if len(argv) > 2 else True

def emit(chunk):
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()

if no_reset:
    # 全程不碰 DTR/RTS：不开 pyserial（它 open 时内核会动控制线），
    # 直接裸读文件描述符。USB CDC 不在乎波特率，也无需 termios 配置。
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            ready, _, _ = select.select([fd], [], [], 0.3)
            if ready:
                emit(os.read(fd, 4096))
    finally:
        os.close(fd)
else:
    import serial

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.3
    s.dtr = dtr
    s.rts = False  # 别动 RTS，某些板子的自动复位电路会因此重启
    s.open()
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            emit(s.read(4096))
    finally:
        s.close()
