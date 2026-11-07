#!/bin/zsh
# 把桥接打包成可双击运行的「AI余光」.app（菜单栏程序 + 内置 node）。
#
#   ./build-app.sh              构建到 dist/AI余光.app，并打一个 zip
#   ./build-app.sh --dmg        额外生成 dmg
#   ./build-app.sh --clean      先删掉 dist 再构建
#
# 产物是完全自包含的：不依赖系统 node，也不依赖 WorkBuddy 目录里的那份。
# 代价是体积（node 二进制约 108 MB 打底）。
#
# 命名：.app 文件名用「AI余光」，Finder 里看到的就是它；
# 包内可执行文件仍叫 yuguang（ASCII），免得路径带中文给日志、ps、grep 添麻烦。
set -euo pipefail
cd "$(dirname "$0")"

APP_TITLE="AI余光"               # 显示名 = .app 文件名（全名，别简写成「余光」）
APP_NAME="yuguang"               # 包内可执行文件名（ASCII）
BUNDLE_ID="cn.deskpulse.app"
VERSION="0.1.3"
ROOT="$(cd .. && pwd)"          # deskpulse/
DIST="$(pwd)/dist"
APP="$DIST/$APP_TITLE.app"
EXE="$APP/Contents/MacOS/$APP_NAME"

NODE_BIN="${NODE_BIN:-$HOME/.workbuddy/binaries/node/versions/22.22.2-6/bin/node}"

# 删除辅助：正常环境直接删；某些受限环境（比如被沙箱接管）删不掉就移走，
# 免得 set -e 把整个构建打断。
drop() {
  local f
  for f in "$@"; do
    [[ -e "$f" ]] || continue
    rm -rf "$f" 2>/dev/null || mv "$f" "/tmp/dp-trash-$$-$RANDOM" 2>/dev/null || true
  done
}

MAKE_DMG=0
for a in "$@"; do
  case "$a" in
    --dmg) MAKE_DMG=1 ;;
    --clean) drop "$DIST" ;;
  esac
done

[[ -x "$NODE_BIN" ]] || { echo "找不到可用的 node: $NODE_BIN"; echo "用 NODE_BIN=/path/to/node $0 指定"; exit 1; }
command -v swiftc >/dev/null || { echo "缺 Swift 工具链：xcode-select --install"; exit 1; }

echo "==> 准备目录"
drop "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/bridge"

echo "==> 编译菜单栏程序（$(swiftc --version 2>/dev/null | head -1)）"
swiftc -O -swift-version 5 \
  -framework Cocoa \
  -o "$EXE" \
  main.swift

echo "==> 写 Info.plist"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_TITLE</string>
  <key>CFBundleDisplayName</key><string>$APP_TITLE</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <!-- 不带 .icns 后缀；实际文件是 Contents/Resources/AppIcon.icns -->
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <!-- 常驻菜单栏，不占 Dock、不抢焦点 -->
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "==> 内置 node 与桥接脚本"
cp "$NODE_BIN" "$APP/Contents/Resources/node"
chmod +x "$APP/Contents/Resources/node"
# 图标：icon/AppIcon.icns 由 icon/build-icon.sh 生成（图片素材 → 裁切 → macOS 图标网格 → icns）
if [[ -f icon/AppIcon.icns ]]; then
  cp icon/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
else
  echo "    ⚠️ 缺 icon/AppIcon.icns，先跑 icon/build-icon.sh"
fi
cp "$ROOT/bridge/deskpulse-bridge.mjs" "$APP/Contents/Resources/bridge/"
cp "$ROOT/bridge/config.json" "$APP/Contents/Resources/bridge/"
cp -R "$ROOT/bridge/adapters" "$APP/Contents/Resources/bridge/"
cp -R "$ROOT/bridge/lib" "$APP/Contents/Resources/bridge/"
# 不带 tests / data / install-launchd.sh —— 包里不需要，data 会在用户目录重建

echo "==> 签名（ad-hoc）"
# 先给嵌套的 node 重签，再签外层包：顺序反了 codesign 会抱怨资源封条对不上
codesign --force --sign - "$APP/Contents/Resources/node" >/dev/null 2>&1
codesign --force --sign - "$APP" >/dev/null 2>&1
codesign --verify --strict "$APP" && echo "    签名校验通过"

SIZE=$(du -sh "$APP" | awk '{print $1}')
echo "==> 完成: $APP ($SIZE)"

ZIP="$DIST/$APP_TITLE-$VERSION.zip"
drop "$ZIP"
# COPYFILE_DISABLE + --norsrc：别把 ._ 这类 AppleDouble 附属文件打进去，
# 否则在别的机器上解压容易被误判成「已损坏」。
COPYFILE_DISABLE=1 ditto -c -k --norsrc --keepParent "$APP" "$ZIP"
echo "    安装包: $ZIP ($(du -sh "$ZIP" | awk '{print $1}'))"

if [[ $MAKE_DMG == 1 ]]; then
  DMG="$DIST/$APP_TITLE-$VERSION.dmg"
  drop "$DMG"
  STAGE="/tmp/dp-dmg-$$"
  mkdir -p "$STAGE"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -quiet -volname "$APP_TITLE" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
  mv "$STAGE" "/tmp/dp-dmg-used-$$"
  echo "    dmg: $DMG ($(du -sh "$DMG" | awk '{print $1}'))"
fi

cat <<EOF

用法：
  open "$APP"                     启动（菜单栏出现 ● AI余光）
  "$EXE" --install-login          设为登录自启
  "$EXE" --uninstall-login        取消
  "$EXE" --status                 命令行查状态（0=在线）
  "$EXE" --version                打印显示名与版本

配置： ~/Library/Application Support/DeskPulse/config.json
日志： ~/Library/Logs/DeskPulse.log
EOF
