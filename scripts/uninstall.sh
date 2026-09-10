#!/usr/bin/env bash
# 제거: 데몬 + 앱 + 로그인 항목 (설정·로그는 보존)
set -euo pipefail
LABEL="co.kdsys.nfcwatcher"; UILABEL="co.kdsys.nfcwatcher-ui"
PREFIX="${PREFIX:-$HOME/.local}"; BIN="$PREFIX/bin/nfcwatcher"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UIPLIST="$HOME/Library/LaunchAgents/$UILABEL.plist"
APP="$HOME/Applications/NFCWatcher.app"

echo "==> 실행 중인 앱 종료"; pkill -f "NFCWatcher.app/Contents/MacOS/NFCWatcher" 2>/dev/null || true

echo "==> launchd 해제"
launchctl unload "$PLIST" 2>/dev/null || true; rm -f "$PLIST"
launchctl bootout "gui/$(id -u)" "$UIPLIST" 2>/dev/null || true; rm -f "$UIPLIST"

echo "==> 파일 삭제"; rm -f "$BIN"; rm -rf "$APP"

echo "제거 완료. (설정 ~/.config/nfcwatcher, 로그는 보존)"
