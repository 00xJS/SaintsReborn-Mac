# Building

The easy way is **Setup** (`installer/`, see [installer/README.md](../installer/README.md)).
It uses a portable toolchain (LLVM/clang, CMake, Ninja and Microsoft's C++
headers and libraries through xwin), works on Windows and Linux, and runs the
same steps as below:

```
SaintsReborn-Setup --cli --dir C:\SaintsReborn --iso "D:\Games\Saints Row.iso" --accept-license
```

`setup.bat` is the older developer path. It needs Visual Studio 2022 and runs
`scripts/setup.ps1`, which performs the steps below. Each step
records its completion in `build/stamps`, so running setup again continues
where it stopped.

| Step | Output |
|---|---|
| 1. Find Visual Studio 2022 and load its x64 build environment | – |
| 2. Build `tools/xiso_extract` and extract your `.iso` | `dist/game` |
| 3. Download the ReXGlue SDK at the pinned commit, apply `patches/rexglue-sdk.patch`, build and install it | `build/rexglue-sdk`, `build/sdk` |
| 4. Recompile `dist/game/default.xex` with `config/saintsrow_manifest.toml` | `build/generated` |
| 5. Compile the game and copy it with the SDK's DLLs | `build/game`, `dist/saintsrow.exe` |

A log of the last run is written to `build/setup.log`.

## Options

```
setup.bat -Iso "D:\Games\Saints Row.iso"   # skip the file picker
setup.bat -Clean                           # redo steps 4 and 5
```

To start completely from scratch, delete the `build` and `dist` folders. Note
that `dist/game` also holds your saves.

## Troubleshooting

**"Visual Studio 2022 was not found" / "missing components"**
Open the Visual Studio Installer, choose *Modify*, and make sure *Desktop
development with C++* is selected along with the individual components *C++
Clang Compiler for Windows* and *C++ CMake tools for Windows*.

**"default.xex is not the version this port was made for"**
The port's hooks refer to fixed addresses in one specific build of the game.
Other versions will most likely not work.

**The SDK download fails**
Check your internet connection and run setup again. If the problem persists,
delete `build/rexglue-sdk` and retry.

**The build runs out of memory**
Close other programs. Compiling the recompiled code needs a lot of RAM; 16 GB
is the practical minimum.

**The game starts but shows a black screen or crashes**
Make sure your GPU driver is up to date and supports Direct3D 12. If the
problem persists, open an issue and include `build/setup.log`.
