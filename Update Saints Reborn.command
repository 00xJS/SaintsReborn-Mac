#!/bin/bash
# Saints Reborn on Mac - update you can double-click. Fetches the newest
# version and rebuilds what changed. Saves and the mod list are kept.
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -x /opt/homebrew/bin/brew ] && eval "$(/opt/homebrew/bin/brew shellenv)"
clear
echo "Updating Saints Reborn on Mac"
echo "============================="
if [ -d "$HERE/.git" ]; then
  git -C "$HERE" pull --ff-only || echo "(Could not fetch the newest version; rebuilding the one already here.)"
fi
if /bin/bash "$HERE/scripts/setup-mac.sh" --accept-license; then
  osascript -e 'display dialog "Saints Reborn is up to date." buttons {"OK"} default button "OK" with title "Saints Reborn"' >/dev/null 2>&1
else
  osascript -e 'display alert "Saints Reborn" message "The update did not finish. This window shows what went wrong; you can run the update again to continue."' >/dev/null 2>&1
fi
