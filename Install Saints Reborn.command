#!/bin/bash
# Saints Reborn on Mac - installer you can double-click.
# Asks for your Saints Row disc image, installs what is needed and builds the
# game in the "SaintsReborn" folder in your home folder.
REPO_URL="https://github.com/00xJS/SaintsReborn-Mac.git"
TARGET="${SR_DIR:-$HOME/SaintsReborn}"
HERE="$(cd "$(dirname "$0")" && pwd)"

say() { printf '\n%s\n' "$1"; }
ask() { # ask "question" -> 0 for yes
  [ -n "${SR_ASSUME_YES:-}" ] && return 0
  osascript -e "display dialog \"$1\" buttons {\"Cancel\", \"Continue\"} default button \"Continue\" with title \"Saints Reborn\"" >/dev/null 2>&1
}
fail() {
  say "Setup stopped: $1"
  [ -n "${SR_ASSUME_YES:-}" ] || osascript -e "display alert \"Saints Reborn\" message \"$1\"" >/dev/null 2>&1
  exit 1
}

clear
echo "Saints Reborn on Mac"
echo "===================="

[ "$(uname -m)" = "arm64" ] || fail "An Apple silicon Mac (M1 or newer) is needed."

# Rosetta 2 (runs the game's Windows code).
if ! /usr/bin/pgrep -q oahd && [ ! -e /Library/Apple/usr/libexec/oah/libRosettaRuntime ]; then
  ask "Saints Reborn needs Rosetta 2, Apple's free translator for Intel programs. Install it now? This accepts Apple's Rosetta license." || exit 0
  softwareupdate --install-rosetta --agree-to-license || fail "Rosetta 2 could not be installed."
fi

# Homebrew (downloads the free build tools). Its installer also installs
# Apple's command line tools and asks for your Mac password.
if [ -x /opt/homebrew/bin/brew ]; then eval "$(/opt/homebrew/bin/brew shellenv)"; fi
if ! command -v brew >/dev/null; then
  ask "Saints Reborn needs Homebrew, a free tool that downloads the build tools. Install it now? It will ask for your Mac password in this window." || exit 0
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" || fail "Homebrew could not be installed."
  [ -x /opt/homebrew/bin/brew ] && eval "$(/opt/homebrew/bin/brew shellenv)"
  command -v brew >/dev/null || fail "Homebrew was installed but not found. Close this window and open the installer again."
fi
command -v git >/dev/null || brew install git || fail "Git could not be installed."

# The game is built in ~/SaintsReborn. A folder downloaded as a ZIP is only
# used to start: the real copy is fetched there so it can update itself later.
if [ -d "$HERE/.git" ]; then
  ROOT="$HERE"
elif [ -d "$TARGET/.git" ]; then
  ROOT="$TARGET"
  say "Updating $ROOT"
  git -C "$ROOT" pull --ff-only || say "(Could not update; continuing with the version already there.)"
else
  [ -e "$TARGET" ] && fail "The folder $TARGET already exists but is not a Saints Reborn folder. Move or rename it and open the installer again."
  say "Downloading Saints Reborn on Mac to $TARGET"
  git clone "$REPO_URL" "$TARGET" || fail "The download failed. Check your internet connection."
  ROOT="$TARGET"
fi

# Disc image (not needed again once the game files are extracted).
ISO_ARGS=()
if [ ! -f "$ROOT/dist/game/default.xex" ]; then
  ISO="${SR_ISO:-}"
  if [ -z "$ISO" ]; then
    ask "Next, choose your Saints Row (Xbox 360) disc image, the .iso file you made from your own disc." || exit 0
    ISO="$(osascript -e 'POSIX path of (choose file with prompt "Choose your Saints Row (Xbox 360) disc image (.iso)")' 2>/dev/null)"
  fi
  [ -f "$ISO" ] || fail "No disc image was chosen."
  ISO_ARGS=(--iso "$ISO")
fi

ask "Setup will now download the free build tools and build the game. This takes about an hour and needs about 15 GB of disk space.\n\nContinuing accepts Microsoft's Visual Studio license terms for the C++ files the build uses (visualstudio.microsoft.com/license-terms)." || exit 0

if [ -n "${SR_DRY_RUN:-}" ]; then
  echo "DRY RUN: /bin/bash $ROOT/scripts/setup-mac.sh ${ISO_ARGS[*]} --accept-license"
  exit 0
fi

if /bin/bash "$ROOT/scripts/setup-mac.sh" "${ISO_ARGS[@]}" --accept-license; then
  [ -n "${SR_ASSUME_YES:-}" ] || osascript -e 'display dialog "Saints Reborn is ready. The Saints Reborn app is in the SaintsReborn folder in your home folder." buttons {"Show it"} default button "Show it" with title "Saints Reborn"' >/dev/null 2>&1
  open -R "$ROOT/Saints Reborn.app"
else
  fail "Setup did not finish. The window behind this message shows what went wrong; you can open the installer again to continue where it stopped."
fi
