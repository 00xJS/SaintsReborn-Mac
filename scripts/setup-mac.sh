#!/bin/bash
# Saints Reborn - setup for macOS (Apple silicon).
#
# Builds the same Windows game as setup.bat / the Setup app (a cross build with
# clang + lld + Microsoft's headers and libraries through xwin) and runs it with
# the free Game Porting Toolkit build of Wine (Direct3D 12 through D3DMetal,
# x86-64 through Rosetta 2). Nothing here contains game code: the game is built
# on your Mac from your own disc image.
#
#   scripts/setup-mac.sh --iso "/path/Saints Row.iso" --accept-license
#   scripts/setup-mac.sh --game-dir /path/with/default.xex --accept-license
#
# Each step records its completion in build/stamps; running the script again
# continues where it stopped. A log of the last run is in build/setup-mac.log.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO=""
GAME_SRC=""
ACCEPT=0
JOBS=""
NO_ONLINE=0

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  echo "Options: --iso FILE | --game-dir DIR, --accept-license, --jobs N, --no-online"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="${2:?--iso needs a file}"; shift 2 ;;
    --game-dir) GAME_SRC="${2:?--game-dir needs a folder}"; shift 2 ;;
    --accept-license) ACCEPT=1; shift ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
    --no-online) NO_ONLINE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1"; usage; exit 2 ;;
  esac
done

BUILD="$ROOT/build"
DIST="$ROOT/dist"
GAME_DIR="$DIST/game"
STAMPS="$BUILD/stamps"
TOOLS="$BUILD/toolchain"
SDK_SRC="$BUILD/rexglue-sdk"
SDK_HOST_SRC="$BUILD/sdk-host"
SDK_INSTALL="$BUILD/sdk"
GENERATED="$BUILD/generated"
PREFIX="$ROOT/prefix"
GPTK_BIN="/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin"

SDK_REPO="https://github.com/rexglue/rexglue-sdk.git"
SDK_COMMIT="$(sed -n 's/^\$SdkCommit = "\([0-9a-f]*\)".*/\1/p' "$ROOT/scripts/setup.ps1")"
KNOWN_XEX_SHA1="c2646093ff3926141fcf4fd630e876873d1caece"
XWIN_CRT="14.44.17.14"
XWIN_SDK="10.0.26100"
VC_REDIST_URL="https://aka.ms/vs/17/release/vc_redist.x64.exe"
ONLINE_PACK_URL="https://github.com/whompay/SaintsReborn/releases/download/online-pack/SaintsReborn-Online.zip"
MS_LICENSE_URL="https://visualstudio.microsoft.com/license-terms/"
WINE_OVERRIDES="msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait,msvcp140_codecvt_ids,vcruntime140,vcruntime140_1,vcruntime140_threads,concrt140=n,b"

mkdir -p "$BUILD" "$STAMPS" "$DIST"
LOG="$BUILD/setup-mac.log"
: > "$LOG"

step() { printf '\n==> %s\n' "$1" | tee -a "$LOG"; }
note() { printf '    %s\n' "$1" | tee -a "$LOG"; }
die() { printf '\nSetup stopped: %s\n(log: %s)\n' "$1" "$LOG" | tee -a "$LOG" >&2; exit 1; }
# Runs a command with its output in the log; on failure shows the end of it.
run() {
  if ! "$@" >>"$LOG" 2>&1; then
    echo "--- last lines of the log ---" >&2
    tail -25 "$LOG" >&2
    die "a step failed: $1"
  fi
}
done_stamp() { [ -f "$STAMPS/$1" ] && [ "$(cat "$STAMPS/$1")" = "$2" ]; }
mark_stamp() { printf '%s' "$2" > "$STAMPS/$1"; }
sha1_of() { shasum -a 1 "$1" | cut -d' ' -f1; }

# ---------------------------------------------------------------------------
step "Checking this Mac"
[ "$(uname -s)" = "Darwin" ] || die "this script is for macOS."
[ "$(uname -m)" = "arm64" ] || die "an Apple silicon Mac is needed (the game runs through Rosetta 2 and D3DMetal)."
if ! /usr/bin/pgrep -q oahd && [ ! -e /Library/Apple/usr/libexec/oah/libRosettaRuntime ]; then
  die "Rosetta 2 is not installed. Install it with: softwareupdate --install-rosetta"
fi
command -v brew >/dev/null || die "Homebrew is needed (https://brew.sh)."
xcode-select -p >/dev/null 2>&1 || die "the Xcode command line tools are needed: xcode-select --install"
[ -n "$SDK_COMMIT" ] || die "could not read the SDK commit from scripts/setup.ps1."
FREE_GB=$(df -g "$ROOT" | awk 'NR==2 {print $4}')
note "Free disk space: ${FREE_GB} GB"
if [ ! -f "$GAME_DIR/default.xex" ] && [ "$FREE_GB" -lt 14 ]; then
  die "about 14 GB of free disk space is needed (found ${FREE_GB} GB)."
fi
RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
if [ -z "$JOBS" ]; then
  # The recompiled sources need about 2.5 GB of RAM per compiler.
  JOBS=$(( RAM_GB / 3 )); CORES=$(sysctl -n hw.ncpu)
  [ "$JOBS" -gt "$CORES" ] && JOBS=$CORES
  [ "$JOBS" -lt 2 ] && JOBS=2
fi
note "RAM ${RAM_GB} GB, ${JOBS} parallel jobs"

# ---------------------------------------------------------------------------
step "Build tools (Homebrew: LLVM 19, lld, CMake, Ninja, xwin)"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
for f in llvm@19 lld@19 cmake ninja xwin; do
  brew list --formula "$f" >/dev/null 2>&1 || run brew install "$f"
done
LLVM_BIN="$(brew --prefix llvm@19)/bin"
LLD_BIN="$(brew --prefix lld@19)/bin"
[ -x "$LLVM_BIN/clang" ] && [ -x "$LLD_BIN/lld-link" ] || die "LLVM 19 / lld were not installed."

step "Wine (Game Porting Toolkit, free)"
if [ ! -x "$GPTK_BIN/wine64" ]; then
  brew tap | grep -qx "gcenx/wine" || run brew tap gcenx/wine
  run brew install --cask gcenx/wine/game-porting-toolkit
fi
[ -x "$GPTK_BIN/wine64" ] || die "Game Porting Toolkit was not installed."

# ---------------------------------------------------------------------------
step "Microsoft C++ headers and libraries (xwin)"
WINSYSROOT="$TOOLS/winsysroot"
if [ ! -d "$WINSYSROOT/VC" ]; then
  if [ "$ACCEPT" != "1" ]; then
    die "the Microsoft C++ headers and libraries come under Microsoft's Visual Studio license ($MS_LICENSE_URL). Run again with --accept-license to accept it."
  fi
  mkdir -p "$TOOLS"
  run xwin --accept-license --log-level warn --http-retry 3 --cache-dir "$TOOLS/xwin-cache" \
    --crt-version "$XWIN_CRT" --sdk-version "$XWIN_SDK" \
    splat --use-winsysroot-style --preserve-ms-arch-notation --output "$WINSYSROOT"
  rm -rf "${TOOLS:?}/xwin-cache"
fi

# Compiler wrappers: every call targets Windows with the xwin sysroot and lld.
# (macOS file systems ignore case by default, so the Linux case overlay of the
# Setup app is not needed.)
mkdir -p "$TOOLS/bin"
cat > "$TOOLS/win.cfg" <<EOF
-Xmicrosoft-windows-sys-root "$WINSYSROOT"
-fuse-ld=lld
-Wno-unused-command-line-argument
EOF
for t in clang clang++; do
  printf '#!/bin/sh\nexec "%s/%s" --config="%s/win.cfg" "$@"\n' "$LLVM_BIN" "$t" "$TOOLS" > "$TOOLS/bin/$t"
  chmod +x "$TOOLS/bin/$t"
done
for t in llvm-rc llvm-ar llvm-ranlib llvm-mt llvm-lib llvm-profdata; do ln -sf "$LLVM_BIN/$t" "$TOOLS/bin/$t"; done
for t in lld lld-link ld.lld; do ln -sf "$LLD_BIN/$t" "$TOOLS/bin/$t"; done
CROSS="$TOOLS/windows-x64.cmake"
cat > "$CROSS" <<EOF
# Builds Windows x64 programs with clang + lld + the xwin headers/libraries.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER "$TOOLS/bin/clang")
set(CMAKE_CXX_COMPILER "$TOOLS/bin/clang++")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_RC_COMPILER "$TOOLS/bin/llvm-rc")
set(CMAKE_AR "$TOOLS/bin/llvm-ar")
set(CMAKE_RANLIB "$TOOLS/bin/llvm-ranlib")
set(CMAKE_MT "$TOOLS/bin/llvm-mt")
set(CMAKE_LINKER_TYPE LLD)
# Only the release C runtime is downloaded (no debug libraries).
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
EOF
export PATH="$TOOLS/bin:$PATH"
CMAKE_COMMON=(-DCMAKE_TOOLCHAIN_FILE="$CROSS" -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5)

# ---------------------------------------------------------------------------
step "Game files"
if [ -f "$GAME_DIR/default.xex" ]; then
  note "Game files already in dist/game - skipping."
elif [ -n "$GAME_SRC" ]; then
  [ -f "$GAME_SRC/default.xex" ] || die "no default.xex in $GAME_SRC."
  note "Copying the game files from $GAME_SRC"
  mkdir -p "$GAME_DIR"
  run rsync -a --exclude '$SystemUpdate' "$GAME_SRC/" "$GAME_DIR/"
elif [ -n "$ISO" ]; then
  [ -f "$ISO" ] || die "disc image not found: $ISO"
  mkdir -p "$BUILD/host"
  run /usr/bin/clang++ -std=c++20 -O2 -w "$ROOT/tools/xiso_extract/xiso_extract.cpp" -o "$BUILD/host/xiso_extract"
  note "Extracting the game from the disc image (about 6 GB)"
  run "$BUILD/host/xiso_extract" "$ISO" "$GAME_DIR" --skip-name '$SystemUpdate' \
    --skip-name layer1filler.bin --skip-name layer2filler.bin
else
  die "the game files are missing. Use --iso <your disc image> or --game-dir <folder with default.xex>."
fi
[ -f "$GAME_DIR/default.xex" ] || die "the game files have no default.xex - is this an Xbox 360 Saints Row disc?"
XEX_SHA1="$(sha1_of "$GAME_DIR/default.xex")"
if [ "$XEX_SHA1" != "$KNOWN_XEX_SHA1" ]; then
  note "WARNING: default.xex is not the version this port was made for; it will most likely not work."
fi

# ---------------------------------------------------------------------------
step "ReXGlue SDK source ($(printf '%.7s' "$SDK_COMMIT")) + the Saints Row patch"
PATCH="$ROOT/patches/rexglue-sdk.patch"
SDK_KEY="$SDK_COMMIT $(sha1_of "$PATCH") mac1"
if ! done_stamp sdk-source "$SDK_KEY"; then
  if [ ! -d "$SDK_SRC/.git" ]; then
    rm -rf "${SDK_SRC:?}"
    run git clone -c core.autocrlf=false "$SDK_REPO" "$SDK_SRC"
  fi
  run git -C "$SDK_SRC" -c advice.detachedHead=false checkout --force "$SDK_COMMIT"
  run git -C "$SDK_SRC" clean -fdq -- include src resources cmake
  run git -C "$SDK_SRC" submodule update --init --recursive --force --depth 1
  run git -C "$SDK_SRC" apply --whitespace=nowarn "$PATCH"
  # The SDK turns Objective-C on when the build host is a Mac, also for the
  # Windows cross build (where no Objective-C compiler exists).
  run sed -i '' '4s/^if(APPLE)$/if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE)/' "$SDK_SRC/CMakeLists.txt"
  grep -q 'if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE)' "$SDK_SRC/CMakeLists.txt" || die "could not adjust the SDK's CMakeLists.txt for cross building."
  rm -f "$STAMPS/sdk" "$STAMPS/host-rexglue" "$STAMPS/codegen"
  mark_stamp sdk-source "$SDK_KEY"
else
  note "Already prepared - skipping."
fi

# ---------------------------------------------------------------------------
step "Recompiler for this Mac (rexglue)"
# The patched runtime only compiles for Windows, and the recompiler links the
# runtime. So the recompiler is built from the unpatched SDK plus the one part
# of the patch that changes generated code (the codegen templates).
if ! done_stamp host-rexglue "$SDK_KEY"; then
  if [ ! -e "$SDK_HOST_SRC/.git" ]; then
    rm -rf "${SDK_HOST_SRC:?}"
    run git -C "$SDK_SRC" worktree prune
    run git -C "$SDK_SRC" worktree add --detach --force "$SDK_HOST_SRC" "$SDK_COMMIT"
  else
    # An earlier run replaced thirdparty with a link; git refuses to check out
    # over it.
    [ -L "$SDK_HOST_SRC/thirdparty" ] && rm -f "$SDK_HOST_SRC/thirdparty"
    run git -C "$SDK_HOST_SRC" checkout --force "$SDK_COMMIT"
  fi
  # Share the (large) third-party sources with the main checkout.
  rm -rf "${SDK_HOST_SRC:?}/thirdparty"
  ln -s ../rexglue-sdk/thirdparty "$SDK_HOST_SRC/thirdparty"
  run git -C "$SDK_HOST_SRC" apply --whitespace=nowarn --include='resources/templates/*' "$PATCH"
  run env PATH="/usr/bin:/bin:$(dirname "$(command -v cmake)"):$(dirname "$(command -v ninja)")" \
    cmake -S "$SDK_HOST_SRC" -B "$BUILD/host/sdk" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_C_FLAGS=-march=armv8-a -DCMAKE_CXX_FLAGS=-march=armv8-a -DCMAKE_CXX_STANDARD=23 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  note "Building (a few minutes)"
  run cmake --build "$BUILD/host/sdk" --target rexglue -j "$JOBS"
  mark_stamp host-rexglue "$SDK_KEY"
else
  note "Already built - skipping."
fi
REXGLUE="$(find "$SDK_HOST_SRC/out" "$BUILD/host/sdk" -type f -name rexglue -perm +111 2>/dev/null | head -1)"
[ -x "$REXGLUE" ] || die "the recompiler was built but not found."

# ---------------------------------------------------------------------------
step "ReXGlue SDK for Windows (about 5-20 minutes)"
GPU_PGO="$ROOT/pgo/rexgpu.profdata"
SDK_BUILD_KEY="$SDK_KEY $( [ -f "$GPU_PGO" ] && sha1_of "$GPU_PGO" )"
if ! done_stamp sdk "$SDK_BUILD_KEY"; then
  SDK_BDIR="$SDK_SRC/out/build/saintsreborn"
  PGO_ARGS=()
  [ -f "$GPU_PGO" ] && PGO_ARGS=(-DREX_GPU_PGO=use -DREX_GPU_PGO_PROFILE="$GPU_PGO")
  run cmake -S "$SDK_SRC" -B "$SDK_BDIR" -G "Ninja Multi-Config" "${CMAKE_COMMON[@]}" \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_STANDARD=23 \
    "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo" -DCMAKE_DEFAULT_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SDK_INSTALL" "${PGO_ARGS[@]}"
  run cmake --build "$SDK_BDIR" --target install --config Release -j "$JOBS"
  mark_stamp sdk "$SDK_BUILD_KEY"
  rm -f "$STAMPS/codegen"
else
  note "Already built - skipping."
fi

# ---------------------------------------------------------------------------
step "Recompiling default.xex (PowerPC -> C++)"
MANIFEST="$ROOT/config/saintsrow_manifest.toml"
CODEGEN_KEY="$SDK_KEY $(sha1_of "$MANIFEST") $XEX_SHA1"
if ! done_stamp codegen "$CODEGEN_KEY" || [ ! -f "$GENERATED/sources.cmake" ]; then
  ( cd "$ROOT" && run "$REXGLUE" codegen "$MANIFEST" --ignore-stamp ) || exit 1
  mark_stamp codegen "$CODEGEN_KEY"
else
  note "Already generated - skipping."
fi

# ---------------------------------------------------------------------------
step "Building Saints Reborn (compiles ~34,000 functions; 10-60 minutes)"
# The x86-64-v2 build: Rosetta 2 runs it everywhere, and under the Game Porting
# Toolkit the AVX2 (v3) build measured no faster.
GAME_BUILD="$BUILD/game"
GAME_PGO="$ROOT/pgo/saintsrow.profdata"
PGO_ARGS=()
[ -f "$GAME_PGO" ] && PGO_ARGS=(-DSR_PGO=use -DSR_PGO_PROFILE="$GAME_PGO")
export REXSDK="$SDK_INSTALL"
run cmake -S "$ROOT/project" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_COMMON[@]}" \
  -DSR_GENERATED_DIR="$GENERATED" -DSR_CPU_LEVEL=v2 "${PGO_ARGS[@]}"
run cmake --build "$GAME_BUILD" -j "$JOBS"
[ -f "$GAME_BUILD/saintsrow_compat.exe" ] || die "the build finished but saintsrow_compat.exe is missing."

# ---------------------------------------------------------------------------
step "Copying the game to dist"
# SR_CPU_LEVEL=v2 names the program saintsrow_compat.exe; here it is the game.
cp "$GAME_BUILD/saintsrow_compat.exe" "$DIST/saintsrow.exe.new" && mv "$DIST/saintsrow.exe.new" "$DIST/saintsrow.exe"
rm -f "$DIST/saintsrow_compat.exe"
[ -f "$GAME_BUILD/saintsrow.map" ] && cp "$GAME_BUILD/saintsrow.map" "$DIST/saintsrow.map"
ONLINE_OK=0
[ -f "$DIST/online_pack.txt" ] && [ -f "$DIST/core/WhompaysCoop/eos/EOSSDK-Win64-Shipping.dll" ] && ONLINE_OK=1
for d in "$SDK_INSTALL"/bin/*.dll; do
  # The online pack's Epic-enabled runtime stays when it is installed.
  if [ "$ONLINE_OK" = "1" ] && [ "$(basename "$d")" = "rexruntime.dll" ]; then continue; fi
  cp "$d" "$DIST/"
done
cp "$GAME_BUILD/WhompaysModLoader.exe" "$DIST/"
mkdir -p "$DIST/mods" "$DIST/core"
for parent in "$ROOT/modding/examples" "$ROOT/modding/mods"; do
  run rsync -a --exclude '*.c' --exclude '*.cpp' --exclude '*.ps1' "$parent/" "$DIST/mods/"
done
mkdir -p "$DIST/mods/ExampleNative" "$DIST/mods/WhompaysTrainer"
cp "$GAME_BUILD/ExampleNative.dll" "$DIST/mods/ExampleNative/"
cp "$GAME_BUILD/WhompaysTrainer.dll" "$DIST/mods/WhompaysTrainer/"
run rsync -a --exclude '*.png' --exclude '*.py' --exclude '*.c' --exclude '*.cpp' --exclude '*.h' \
  --exclude '*.ps1' --exclude '*.bak*' --exclude 'join_ip.txt' "$ROOT/core/" "$DIST/core/"
[ "$ONLINE_OK" = "1" ] || cp "$GAME_BUILD/WhompaysCoop.dll" "$DIST/core/WhompaysCoop/"
[ -f "$DIST/core/WhompaysCoop/join_ip.txt" ] || cp "$ROOT/core/WhompaysCoop/join_ip.txt" "$DIST/core/WhompaysCoop/" 2>/dev/null || true

step "Keyboard/mouse button pictures"
ZLIB="$GAME_BUILD/_deps/zlib-src"
GG="$BUILD/host/glyphgen.obj.d"
mkdir -p "$GG"
if ( cd "$GG" && /usr/bin/clang -O2 -w -c -I"$ZLIB" "$ZLIB"/{adler32,compress,crc32,deflate,inffast,inflate,inftrees,trees,uncompr,zutil}.c ) >>"$LOG" 2>&1 &&
   /usr/bin/clang++ -std=c++20 -O2 -w -I"$ROOT/project/src/wml" -I"$ZLIB" "$ROOT/tools/glyphgen/glyphgen.cpp" \
     "$ROOT/project/src/wml/packfile.cpp" "$GG"/*.o -o "$BUILD/host/glyphgen" >>"$LOG" 2>&1 &&
   "$BUILD/host/glyphgen" "$GAME_DIR/packfiles" "$ROOT/tools/glyphgen/art.txt" "$DIST/kbm_ui.bin" >>"$LOG" 2>&1; then
  note "Made dist/kbm_ui.bin"
else
  note "WARNING: the button pictures could not be made; the game will show controller buttons."
fi

# ---------------------------------------------------------------------------
step "Online play (Epic)"
if [ "$NO_ONLINE" = "1" ]; then
  note "Skipped (--no-online). System Link on a LAN and co-op by IP still work."
elif ! command -v python3 >/dev/null; then
  note "python3 not found - online pack skipped."
else
  WANTED="$(python3 "$ROOT/scripts/mac/online_stamp.py" "$ROOT" "$SDK_COMMIT")"
  if [ "$(cat "$DIST/online_pack.txt" 2>/dev/null)" = "$WANTED" ] && [ "$ONLINE_OK" = "1" ]; then
    note "On (online pack already installed)."
  else
    mkdir -p "$BUILD/downloads"
    ZIP="$BUILD/downloads/SaintsReborn-Online.zip"
    if curl -fsL -o "$ZIP" "$ONLINE_PACK_URL" &&
       python3 "$ROOT/scripts/mac/online_install.py" "$ZIP" "$DIST" "$WANTED" >>"$LOG" 2>&1
    then
      note "On (online pack installed)."
    else
      rm -f "$DIST/online_pack.txt"
      cp "$SDK_INSTALL/bin/rexruntime.dll" "$DIST/"
      cp "$GAME_BUILD/WhompaysCoop.dll" "$DIST/core/WhompaysCoop/"
      note "The online pack does not match this source (or could not be downloaded): online play stays off."
      note "System Link on a LAN and co-op by IP still work."
    fi
    rm -f "$ZIP"
  fi
fi

# ---------------------------------------------------------------------------
step "Mac defaults"
# The game's own 30 fps limiter runs about 8% slow under Wine (frames of 36 ms,
# sound slowed down with it); the 60 FPS mod hands frame pacing to the port's
# limiter, which is exact. Kept as it is when a mod list already exists.
if [ ! -f "$DIST/mods/modlist.ini" ]; then
  {
    echo "; Whompay's Mod Loader - load order and on/off state."
    echo "+ SixtyFPS"
    for d in "$DIST"/mods/*/; do
      n="$(basename "$d")"
      [ "$n" = "SixtyFPS" ] || echo "- $n"
    done
  } > "$DIST/mods/modlist.ini"
  note "60 FPS mod switched on."
fi
# 1x internal resolution holds 60 fps on an M2 Pro; 2x (the PC default) is
# sharper but ran at 36-55 fps there. Pause > Options changes it in the game.
[ -f "$DIST/res_scale.txt" ] || { echo 1 > "$DIST/res_scale.txt"; note "Resolution scale 1x (res_scale.txt)."; }

# ---------------------------------------------------------------------------
step "Wine prefix and Microsoft's Visual C++ runtime"
export WINEPREFIX="$PREFIX" WINEDEBUG=-all
if [ ! -f "$PREFIX/system.reg" ]; then
  run "$GPTK_BIN/wine64" wineboot -u
fi
# Wine brings its own (incomplete) msvcp140*.dll, so their presence says
# nothing. vcruntime140_threads.dll only comes with Microsoft's runtime.
MS_RUNTIME_MARK="$PREFIX/drive_c/windows/system32/vcruntime140_threads.dll"
if [ ! -f "$MS_RUNTIME_MARK" ]; then
  [ "$ACCEPT" = "1" ] || die "the Visual C++ runtime comes under Microsoft's license ($MS_LICENSE_URL). Run again with --accept-license."
  mkdir -p "$BUILD/downloads"
  run curl -fsL -o "$BUILD/downloads/vc_redist.x64.exe" "$VC_REDIST_URL"
  run "$GPTK_BIN/wine64" "$BUILD/downloads/vc_redist.x64.exe" /install /quiet /norestart
  rm -f "$BUILD/downloads/vc_redist.x64.exe"
fi
"$GPTK_BIN/wineserver" -k >/dev/null 2>&1 || true
[ -f "$MS_RUNTIME_MARK" ] || die "the Visual C++ runtime was not installed into the Wine prefix."

# ---------------------------------------------------------------------------
step "Saints Reborn.app"
run "$ROOT/scripts/mac/make_app.sh" "$ROOT"

printf '\nDone. Open "%s/Saints Reborn.app" to pick mods and play.\n' "$ROOT" | tee -a "$LOG"
printf 'Saves and profile data are in %s\n' "$GAME_DIR" | tee -a "$LOG"
