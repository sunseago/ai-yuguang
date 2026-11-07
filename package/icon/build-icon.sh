#!/bin/zsh
# 从一张生成的原图做出 AppIcon.icns。
#
#   ./build-icon.sh                     用默认素材和默认裁切
#   ./build-icon.sh src.png 505 505 880 指定素材 + 裁切中心/边长
#
# 为什么要裁、要重画，见 make-icon.swift 顶部的注释。
# 换图标时只要换一张主体居中、右下角留空的原图，再调这三个裁切参数即可。
set -euo pipefail
cd "$(dirname "$0")"

SRC="${1:-raw/应用图标_一只可爱的卡通大眼睛_正视图_关键特征__1_眼白_2026-11-07T14-13-56.png}"
CX="${2:-505}"
CY="${3:-505}"
SIDE="${4:-880}"

[[ -f "$SRC" ]] || { echo "找不到素材: $SRC"; exit 1; }

echo "==> 编译渲染器"
swiftc -O -o make-icon make-icon.swift

echo "==> 裁切 + 套 macOS 图标网格"
./make-icon "$SRC" icon-1024.png "$CX" "$CY" "$SIDE"

echo "==> 生成各尺寸"
# rm 在受限环境会被安全策略拒（EACCES），拒了就挪走，别让 set -e 把流程打断
rm -rf AppIcon.iconset 2>/dev/null || mv AppIcon.iconset "/tmp/dp-iconset-$$" 2>/dev/null || true
mkdir -p AppIcon.iconset
for sz in 16 32 128 256 512; do
  sips -z $sz $sz icon-1024.png --out "AppIcon.iconset/icon_${sz}x${sz}.png" >/dev/null
  sips -z $((sz*2)) $((sz*2)) icon-1024.png --out "AppIcon.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
done

echo "==> 打包 icns"
rm -f AppIcon.icns 2>/dev/null || mv AppIcon.icns "/tmp/dp-old.icns-$$" 2>/dev/null || true
iconutil -c icns AppIcon.iconset -o AppIcon.icns

echo "完成: $(pwd)/AppIcon.icns ($(du -h AppIcon.icns | awk '{print $1}'))"
echo "预览: $(pwd)/icon-1024.png"
