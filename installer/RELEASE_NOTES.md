## What's new in 1.2

- **New Setup, no Visual Studio needed.** Setup now downloads a portable
  toolchain (clang, CMake, Ninja and Microsoft's C++ headers, about 1.5 GB in
  one folder) instead of the 6 GB Visual Studio Build Tools. The first build
  takes 10-60 minutes.
- **Install from a disc image or from a folder with your game files** (the
  folder with `default.xex` and `packfiles`).
- **Quick Match, Custom Match and the party lobby work for everyone.** They
  run over Epic and used to need a hidden switch; now they are on whenever
  online play is installed.
- **Faster on 4-core and older CPUs:** shadow volumes, matrix maths and vertex
  setup run as native code (about 10-13 % more fps on 4 threads), and CPUs
  without FMA3 (2012 and older) get a much faster compatible build.
- The depth pre-pass is drawn every frame again, which fixes occasional black
  pixels.
- Mods that replace game files start much faster.
- Co-op and multiplayer are built in. They don't appear in the mod loader's
  list because they are always on.

## Install or update

1. Download **SaintsReborn-Setup.exe** below and run it.
2. New install: choose your Saints Row (Xbox 360) disc image (.iso) or game
   folder and an install folder, then press **Install**.
   Update: choose your Saints Reborn folder and press **Update**. Only what
   changed is rebuilt; your saves and mod list are kept.

Avoid installing inside a OneDrive folder; syncing can break the build.
Linux support is coming soon.
