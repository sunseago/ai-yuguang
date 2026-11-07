#!/bin/zsh
# 编译并烧录。封装了两个本机环境的坑：
#   1) PYTHONPATH 里的 sitecustomize 会拦截 PlatformIO 的临时文件清理
#   2) 需要可写的 TMPDIR
# 用法： ./flash.sh            自动找串口并烧录
#        ./flash.sh /dev/cu.usbmodem1101   指定串口
#        ./flash.sh build      只编译不烧录
set -e
cd "$(dirname "$0")"

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
unset PYTHONPATH
export TMPDIR="${TMPDIR:-/tmp/dppio}"
mkdir -p "$TMPDIR"

if [[ "$1" == "build" ]]; then
  pio run
  exit 0
fi

PORT="$1"
if [[ -z "$PORT" ]]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null | head -1)
fi

if [[ -n "$PORT" ]]; then
  echo "串口: $PORT"
  pio run -t upload --upload-port "$PORT"
else
  echo "没找到串口，先只编译。"
  pio run
  echo
  echo "烧录时请插上板子，或手动指定： ./flash.sh /dev/cu.usbmodemXXXX"
fi
