#!/bin/zsh
# 编译并烧录。封装了两个本机环境的坑：
#   1) PYTHONPATH 里的 sitecustomize 会拦截 PlatformIO 的临时文件清理
#   2) 需要可写的 TMPDIR
#
# 用法：
#   ./flash.sh                         默认板子（waveshare_lcd_154），自动找串口
#   PIO_ENV=waveshare_amoled_216 ./flash.sh     AMOLED 2.16 板
#   ./flash.sh /dev/cu.usbmodem1101     指定串口
#   ./flash.sh build                    只编译不烧录
set -e
cd "$(dirname "$0")"

PIO_ENV="${PIO_ENV:-waveshare_lcd_154}"
BUILD_BIN=".pio/build/$PIO_ENV/firmware.bin"

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
unset PYTHONPATH
export TMPDIR="${TMPDIR:-/tmp/dppio}"
mkdir -p "$TMPDIR"

# 编译"成功"不等于真的重编了：改完源码没触发依赖、或者烧的是上一次的镜像，
# 都会得到一块行为对不上的板子（0.1.2 时就踩过）。这里用 mtime 做一次硬检查。
check_fresh() {
  local bin="$BUILD_BIN"
  if [[ ! -f "$bin" ]]; then
    echo "⚠️  没找到 $bin"
    return 1
  fi
  local stale
  stale=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$bin" 2>/dev/null | head -5)
  if [[ -n "$stale" ]]; then
    echo "⚠️  firmware.bin 比这些源码旧，可能没有真正重编："
    echo "$stale" | sed 's/^/      /'
    return 1
  fi
  echo "✓ 固件新鲜度检查通过（$PIO_ENV, bin: $(date -r "$bin" '+%m-%d %H:%M:%S')）"
}

if [[ "$1" == "build" ]]; then
  pio run -e "$PIO_ENV"
  check_fresh
  exit 0
fi

PORT="$1"
if [[ -z "$PORT" ]]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null | head -1)
fi

if [[ -n "$PORT" ]]; then
  echo "板子: $PIO_ENV   串口: $PORT"
  pio run -e "$PIO_ENV" -t upload --upload-port "$PORT"
  check_fresh
else
  echo "没找到串口，先只编译（$PIO_ENV）。"
  pio run -e "$PIO_ENV"
  check_fresh
  echo
  echo "烧录时请插上板子，或手动指定： PIO_ENV=$PIO_ENV ./flash.sh /dev/cu.usbmodemXXXX"
fi
