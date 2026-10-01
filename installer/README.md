# Saints Reborn Setup

`SaintsReborn-Setup` installs and updates Saints Reborn on Windows and Linux.
It builds the game on the player's own PC from their own disc image or game
folder. Neither the program nor the release contains game code.

What it does:

1. Downloads a portable build toolchain into the install folder
   (`build/toolchain`, on the same drive as the game): LLVM/clang 19.1.5 (only
   the parts the build uses, about 0.7 GB), CMake, Ninja, and Microsoft's C++
   headers and libraries through [xwin](https://github.com/Jake-Shadle/xwin).
   The player accepts Microsoft's license for those in the window. Every
   download is checked against a SHA-256 hash in `src/config.rs` and unpacked
   by Setup itself. Nothing is installed system-wide, and Visual Studio is not
   needed. On Windows, portable Git is downloaded when Git is missing. Setup
   stops early when the drive has less than 6 GB free.
2. Downloads this repository (a shallow git clone) or updates it.
3. Extracts the disc image, or copies a folder with the game files, to
   `dist/game`.
4. Builds the ReXGlue SDK with `patches/rexglue-sdk.patch`, recompiles
   `default.xex`, builds the game and copies everything to `dist`. Each step
   is skipped when its inputs have not changed.
5. Installs the online pack when it matches the source (the stamp is the
   same as `scripts/online_stamp.ps1`), and creates shortcuts.

On Linux (work in progress) it builds the same Windows game with the same compiler (a cross
build) and the player runs it with Proton. Linux players need `git` and the
C++ standard library headers (`build-essential`, `gcc-c++` or `base-devel`).

## Command line

```
SaintsReborn-Setup --cli --dir <folder> [--iso <file> | --game-dir <folder>]
                   --accept-license [--update] [--force] [--no-shortcut]
SaintsReborn-Setup --sdk-only <source folder> --accept-license
SaintsReborn-Setup --online-stamp <source folder>
```

`--sdk-only` builds the toolchain, SDK and recompiler without game files.
CI uses it to test the Linux build (`.github/workflows/setup-app.yml`).

## Building

```
cd installer
cargo build --release
```

CI builds both programs, and attaches `SaintsReborn-Setup.exe` and
`SaintsReborn-Setup-linux-x64` to every published release.
