#!/usr/bin/env bash
# NFCWatcher.app 번들 조립: 데몬 + 앱 + Info.plist(LSUIElement)
# 주의: macOS 기본 APFS는 대소문자 구분 안 함 → 느슨한 build/NFCWatcher 를
#       만들면 build/nfcwatcher(도구)와 충돌한다. 앱 바이너리는 번들 안에 직접 빌드.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
APP="build/NFCWatcher.app"

echo "==> 데몬 빌드: build/nfcwatcher"
mkdir -p build
cc -O2 -Wall -o build/nfcwatcher src/nfcwatcher.c -framework CoreServices

echo "==> 앱 번들 구성: $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "==> 앱 바이너리 빌드 (번들 내부로 직접)"
cc -fobjc-arc -O2 -Wall -o "$APP/Contents/MacOS/NFCWatcher" menubar/main.m -framework Cocoa

cp build/nfcwatcher "$APP/Contents/Resources/nfcwatcher"   # 앱 자체 완결성용 번들 사본

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>CFBundleName</key><string>NFCWatcher</string>
    <key>CFBundleDisplayName</key><string>NFCWatcher</string>
    <key>CFBundleIdentifier</key><string>co.kdsys.nfcwatcher.ui</string>
    <key>CFBundleExecutable</key><string>NFCWatcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHumanReadableCopyright</key><string>KDSYS</string>
</dict></plist>
PLIST
printf 'APPL????' > "$APP/Contents/PkgInfo"

echo "==> ad-hoc 코드서명"
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "  (codesign 생략 — 로컬 실행은 대개 가능)"
echo "완료: $APP"
