#!/bin/bash
# Builds "Saints Reborn.app" in the install folder: a double-clickable launcher
# that opens Whompay's Mod Loader (see launch.sh).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-$(cd "$HERE/../.." && pwd)}"
APP="$ROOT/Saints Reborn.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$HERE/launch.sh" "$APP/Contents/MacOS/SaintsReborn"
chmod +x "$APP/Contents/MacOS/SaintsReborn"
printf '%s' "$ROOT" > "$APP/Contents/MacOS/install_dir"
# Icon from the project's logo (optional).
ICO="$ROOT/project/res/SaintsReborn.ico"
if [ -f "$ICO" ] && command -v iconutil >/dev/null && command -v sips >/dev/null; then
  TMP="$(mktemp -d)"
  SET="$TMP/SaintsReborn.iconset"
  LOGO="$TMP/logo.png"
  mkdir -p "$SET"
  sips -s format png "$ICO" --out "$LOGO" >/dev/null 2>&1 || true
  for s in 16 32 128 256 512; do
    sips -z $s $s "$LOGO" --out "$SET/icon_${s}x${s}.png" >/dev/null 2>&1 || true
    d=$((s * 2))
    sips -z $d $d "$LOGO" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null 2>&1 || true
  done
  iconutil -c icns "$SET" -o "$APP/Contents/Resources/SaintsReborn.icns" 2>/dev/null || true
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Saints Reborn</string>
  <key>CFBundleDisplayName</key><string>Saints Reborn</string>
  <key>CFBundleIdentifier</key><string>io.github.whompay.saintsreborn</string>
  <key>CFBundleExecutable</key><string>SaintsReborn</string>
  <key>CFBundleIconFile</key><string>SaintsReborn</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
echo "Created $APP"
