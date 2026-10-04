#!/bin/bash
# Makes the release download: SaintsReborn-Mac-Setup.zip with the
# double-click installer. Usage: scripts/mac/make_release.sh [output folder]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
STAGE="$(mktemp -d)"
mkdir -p "$OUT"
cp "$ROOT/Install Saints Reborn.command" "$STAGE/"
chmod +x "$STAGE/Install Saints Reborn.command"
rm -f "$OUT/SaintsReborn-Mac-Setup.zip"
( cd "$STAGE" && zip -q -X "$OUT/SaintsReborn-Mac-Setup.zip" "Install Saints Reborn.command" )
echo "$OUT/SaintsReborn-Mac-Setup.zip"
