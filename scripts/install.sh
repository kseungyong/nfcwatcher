#!/usr/bin/env bash
# nfcwatcher 설치: C 데몬(감시 엔진) + NFCWatcher 메뉴바 앱(제어 UI)
set -euo pipefail

LABEL="co.kdsys.nfcwatcher"
PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"; BIN="$BINDIR/nfcwatcher"
CONFDIR="$HOME/.config/nfcwatcher"; CONF="$CONFDIR/nfcwatcher.conf"
LOGDIR="$HOME/Library/Logs"
LA_DIR="$HOME/Library/LaunchAgents"; PLIST="$LA_DIR/$LABEL.plist"
APPDIR="$HOME/Applications"; APP="$APPDIR/NFCWatcher.app"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> 빌드 (데몬 + 앱 번들)"
bash "$ROOT/scripts/build_app.sh"

echo "==> 데몬 설치: $BIN"
mkdir -p "$BINDIR"; install -m 0755 "$ROOT/build/nfcwatcher" "$BIN"

echo "==> 설정 준비: $CONF"
mkdir -p "$CONFDIR" "$LOGDIR"
[ -f "$CONF" ] || cp "$ROOT/config/nfcwatcher.conf.example" "$CONF"

echo "==> 메뉴바 앱 설치: $APP"
mkdir -p "$APPDIR"; rm -rf "$APP"; cp -R "$ROOT/build/NFCWatcher.app" "$APP"

echo "==> 데몬 launchd 등록 (로그인 시 자동 시작)"
mkdir -p "$LA_DIR"
sed -e "s#__BIN__#$BIN#g" -e "s#__LOGDIR__#$LOGDIR#g" \
    "$ROOT/launchd/$LABEL.plist" > "$PLIST"
launchctl unload "$PLIST" 2>/dev/null || true
launchctl load  "$PLIST"

echo "==> 메뉴바 앱 실행"
open "$APP" || true

cat <<MSG

설치 완료.
  · 감시 데몬이 로그인 시 자동 시작됩니다 (기본: ~/Downloads, ~/Desktop).
  · 메뉴바에 아이콘이 나타납니다. 클릭해서 On/Off·폴더 관리·즉시 변환.
  · 앱의 "로그인 시 자동 시작" 메뉴로 앱도 로그인 시 뜨게 할 수 있습니다.

  상태 : launchctl list | grep nfcwatcher
  로그 : tail -f $LOGDIR/nfcwatcher.log
  설정 : $CONF
MSG
