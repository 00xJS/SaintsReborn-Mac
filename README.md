# Saints Reborn on Mac

Play **Saints Row** (Xbox 360, 2006) on an Apple silicon Mac, free.

This is a Mac setup for
[**Saints Reborn by Whompay**](https://github.com/whompay/SaintsReborn). It builds the game on your own Mac from your own disc and adds a
**Saints Reborn** app that opens the mod loader, where you pick your mods and
press Play.

**This repository contains no game code or data.** You build the game yourself,
on your own Mac, from your own disc.

> Saints Reborn is a fan project. It is not affiliated with, endorsed by or
> sponsored by Volition, THQ Nordic, Deep Silver, Plaion or Microsoft. Saints
> Reborn on Mac is not affiliated with, endorsed by or sponsored by Apple
> either.

## What you need

- A Mac with Apple silicon (M1 or newer), macOS 14 or newer
- Your own Saints Row (Xbox 360) disc image (`.iso`)
- About 15 GB of free disk space

## Install

1. Download **SaintsReborn-Mac-Setup.zip** from the
   [latest release](https://github.com/00xJS/SaintsReborn-Mac/releases/latest)
   and open it in your Downloads folder to unpack it.
2. Right-click **Install Saints Reborn** and choose
   **Open**. If macOS says it cannot be opened, go to **System Settings >
   Privacy & Security**, scroll down and click **Open Anyway**.
3. Choose your Saints Row disc image (`.iso`) when asked, then let it work.
   It downloads the free tools it needs and builds the game, which takes
   about an hour. It may ask for your Mac password once.
4. When it finishes, open **Saints Reborn** in the `SaintsReborn` folder in
   your home folder. Tick the mods you want and press **Play**.

You can drag the app to your Dock or Applications folder, and delete the
downloaded ZIP and installer afterwards.

### With Terminal

```bash
git clone https://github.com/00xJS/SaintsReborn-Mac.git ~/SaintsReborn
~/SaintsReborn/scripts/setup-mac.sh --iso "/path/to/Saints Row.iso" --accept-license
```

`--accept-license` accepts Microsoft's license for the C++ files the build
uses. [Homebrew](https://brew.sh) must be installed first.

## Update

Double-click **Update Saints Reborn** in the `SaintsReborn` folder in your
home folder. Your saves and mod list are kept. New versions are announced on
the [releases page](https://github.com/00xJS/SaintsReborn-Mac/releases).

## Playing

An Xbox controller works as on the console, and keyboard and mouse are fully
supported. Controls, mods and co-op are the same as in Saints Reborn: see the
[Saints Reborn README](https://github.com/whompay/SaintsReborn#playing).

Useful keys: **F11** fullscreen / window, **F1** frame rate counter,
**V** first person view.

Saves are in `~/SaintsReborn/dist/game`.

## Legal

This project distributes only original source code, configuration files and a
patch to the BSD-licensed ReXGlue SDK. It does not include, and must not be
used to distribute, any part of Saints Row: no disc images, game files,
recompiled code or built executables. Dump your own disc. Requests for or links
to game files will be removed.

The project's own code is released under the [Apache License 2.0](LICENSE).
If you share, re-upload or build on it, you must keep the [NOTICE](NOTICE) file
and credit it as **Saints Reborn by Whompay**. The SDK
and the libraries it downloads during the build are covered by their own
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). "Saints Row" is
a trademark of its owner and is used here only to name the game this project
works with.

## Credits

- **Saints Reborn by Whompay**: the project itself. Saints Reborn on Mac is
  built on it and changes only what the Mac setup needs.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay and
  contributors.
- [Xenia](https://xenia.jp) by Ben Vanik and contributors, which the SDK is
  derived from.
- [Game Porting Toolkit](https://developer.apple.com/games) by Apple, packaged
  by [Gcenx](https://github.com/Gcenx/game-porting-toolkit).
- Volition, for the game.
