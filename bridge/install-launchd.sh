#!/bin/zsh
# 把 AI余光 桥接装成 macOS 常驻服务（LaunchAgent）。
# 用法： ./install-launchd.sh            安装并立即启动
#        ./install-launchd.sh uninstall  卸载
set -e

LABEL="cn.deskpulse.bridge"
DIR="$(cd "$(dirname "$0")" && pwd)"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs"

if [[ "$1" == "uninstall" ]]; then
  launchctl unload "$PLIST" 2>/dev/null || true
  rm -f "$PLIST"
  echo "已卸载"
  exit 0
fi

NODE_BIN="${NODE_BIN:-$(command -v node || true)}"
if [[ -z "$NODE_BIN" ]]; then
  echo "找不到 node。请先安装 Node 22+，或用 NODE_BIN=/path/to/node $0 指定。"
  exit 1
fi

NODE_VER="$("$NODE_BIN" -v)"
echo "使用 Node: $NODE_BIN ($NODE_VER)"

mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$NODE_BIN</string>
    <string>--no-warnings</string>
    <string>$DIR/deskpulse-bridge.mjs</string>
  </array>
  <key>WorkingDirectory</key><string>$DIR</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>$LOG_DIR/DeskPulse.log</string>
  <key>StandardErrorPath</key><string>$LOG_DIR/DeskPulse.err.log</string>
</dict>
</plist>
EOF

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"

sleep 1
PORT=$(python3 -c "import json;print(json.load(open('$DIR/config.json'))['port'])" 2>/dev/null || echo 8787)
if curl -s --max-time 3 "http://127.0.0.1:$PORT/healthz" >/dev/null; then
  echo "桥接已启动： http://127.0.0.1:$PORT/v1/state"
  echo "日志： $LOG_DIR/DeskPulse.log"
else
  echo "启动失败，请查看 $LOG_DIR/DeskPulse.err.log"
  exit 1
fi
