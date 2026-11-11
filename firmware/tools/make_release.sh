#!/bin/zsh
# 生成 Release 用的合并固件镜像（两块板各一份）+ SHA-256。
#
#   ./tools/make_release.sh            两个板级目标都出
#   ./tools/make_release.sh amoled     只出 AMOLED 2.16
#
# 产物落在 dist/：
#   AIYuguang-<版本>-waveshare-amoled-216.bin
#   AIYuguang-<版本>-waveshare-lcd-154.bin
#   SHA256SUMS.txt
#
# 合并镜像 = bootloader(0x0) + 分区表(0x8000) + boot_app0(0xe000) + 应用(0x10000)，
# 所以用户一条命令 write_flash 0x0 <bin> 就能烧，不用记四个偏移。
set -e
cd "$(dirname "$0")/.."

# 先记住当前用的 python3（esptool 依赖的 pyserial 装在它的 site-packages 里）。
# 下面会把 PATH 收敛成一套干净目录，那之后 `python3` 可能就换成系统 python 了。
PY="$(command -v python3)"

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
# PlatformIO 怕 PYTHONPATH 里的 sitecustomize 拦截它的临时文件清理，所以要清掉；
# 但 esptool 依赖的 pyserial 就装在 PYTHONPATH 指的位置，调用前再还回去。
ORIG_PYTHONPATH="${PYTHONPATH:-}"
unset PYTHONPATH
export TMPDIR="${TMPDIR:-/tmp/dppio}"
mkdir -p "$TMPDIR" dist

ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
[[ -f "$ESPTOOL" ]] || { echo "找不到 esptool: $ESPTOOL"; exit 1; }
[[ -f "$BOOT_APP0" ]] || { echo "找不到 boot_app0.bin: $BOOT_APP0"; exit 1; }

VERSION="$("$PY" -c "import json;print(json.load(open('../bridge/version.json'))['version'])")"
echo "版本: $VERSION"

# 环境 -> 产物名后缀
typeset -A NAMES
NAMES[waveshare_amoled_216]="waveshare-amoled-216"
NAMES[waveshare_lcd_154]="waveshare-lcd-154"

# zsh 默认不做单词切分，数组必须直接赋字面量（别用 ${@:-...} 去拼）
ENVS=(waveshare_amoled_216 waveshare_lcd_154)
[[ "$1" == "amoled" ]] && ENVS=(waveshare_amoled_216)
[[ "$1" == "lcd" ]] && ENVS=(waveshare_lcd_154)

for env in "${ENVS[@]}"; do
  echo "── 编译 $env"
  pio run -e "$env" >/dev/null || { echo "编译失败: $env"; exit 1; }
  B=".pio/build/$env"
  OUT="dist/AIYuguang-$VERSION-${NAMES[$env]}.bin"
  PYTHONPATH="$ORIG_PYTHONPATH" "$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o "$OUT" \
    --flash_mode qio --flash_freq 80m --flash_size 16MB \
    0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" \
    0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin" >/dev/null
  printf '  %s  %s 字节\n' "$OUT" "$(stat -f%z "$OUT")"
done

cd dist
shasum -a 256 AIYuguang-$VERSION-*.bin | tee SHA256SUMS.txt
