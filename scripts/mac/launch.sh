#!/bin/bash
# Saints Reborn (macOS): starts Whompay's Mod Loader (or the game with "game")
# through the free Game Porting Toolkit build of Wine.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# Inside the app bundle the install folder is recorded next to this script.
if [ -f "$HERE/install_dir" ]; then ROOT="$(cat "$HERE/install_dir")"; else ROOT="$(cd "$HERE/../.." && pwd)"; fi
WINE_BIN="/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin"
if [ ! -x "$WINE_BIN/wine64" ]; then
  osascript -e 'display alert "Saints Reborn" message "Game Porting Toolkit was not found. Run scripts/setup-mac.sh again."' >/dev/null 2>&1
  exit 1
fi
if [ ! -f "$ROOT/dist/saintsrow.exe" ]; then
  osascript -e 'display alert "Saints Reborn" message "The game has not been built yet. Run scripts/setup-mac.sh first."' >/dev/null 2>&1
  exit 1
fi
export WINEPREFIX="$ROOT/prefix"
export WINEDEBUG=-all
export WINEESYNC=1
# Microsoft's own C++ runtime (installed into the prefix by setup): the
# toolkit's Wine lacks parts of it.
export WINEDLLOVERRIDES="msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,msvcp140_codecvt_ids,vcruntime140,vcruntime140_1,vcruntime140_threads,concrt140=n,b"
cd "$ROOT/dist" || exit 1
EXE="WhompaysModLoader.exe"
[ "${1:-}" = "game" ] && EXE="saintsrow.exe"
case "${1:-}" in *.exe) EXE="$1" ;; esac
"$WINE_BIN/wine64" "$EXE" >"$ROOT/build/last_run.log" 2>&1
# The mod loader closes when Play is pressed and the game carries on in the
# same Wine session. Stay until that session ends: when this script (the app's
# main process) exits first, macOS takes the game's parent processes with it
# and the game never appears.
"$WINE_BIN/wineserver" -w >/dev/null 2>&1
