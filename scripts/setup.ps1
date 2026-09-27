# Saints Reborn - build script.
#
# Builds a native Windows version of Saints Row (Xbox 360, 2006) from YOUR OWN
# copy of the game. Nothing from the game is included in this repository; the
# script extracts your disc image, recompiles the game's executable locally and
# places the result in .\dist.
#
# Usage (normally via setup.bat):
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 [-Iso <path>] [-Clean]

param(
    [string]$Iso = "",
    [switch]$Clean,
    [switch]$NoPause
)

# Native tools report failure through exit codes, checked by Run below.
$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Build = Join-Path $Root "build"
$Dist = Join-Path $Root "dist"
$GameDir = Join-Path $Dist "game"
$SdkSrc = Join-Path $Build "rexglue-sdk"
$SdkInstall = Join-Path $Build "sdk"
$GameBuild = Join-Path $Build "game"

$SdkRepo = "https://github.com/rexglue/rexglue-sdk.git"
$SdkCommit = "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518"   # ReXGlue SDK v0.10.0

New-Item -ItemType Directory -Force -Path $Build, $Dist | Out-Null
Start-Transcript -Path (Join-Path $Build "setup.log") -Force | Out-Null

function Step($text) { Write-Host "`n== $text ==" -ForegroundColor Cyan }
function Fail($text) {
    Write-Host "`nERROR: $text" -ForegroundColor Red
    Write-Host "A full log is in build\setup.log"
    Stop-Transcript | Out-Null
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}
function Run($exe, [string[]]$argList) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($argList -join ' ')' failed (exit code $LASTEXITCODE)." }
}

if ($Clean) {
    Step "Removing previous build output"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $GameBuild, (Join-Path $Build "generated"), (Join-Path $Build "stamps")
}
$Stamps = Join-Path $Build "stamps"
New-Item -ItemType Directory -Force -Path $Stamps | Out-Null
# A step is redone when its inputs change (e.g. after pulling an update).
function Done($name, $key) {
    $f = Join-Path $Stamps $name
    (Test-Path $f) -and ((Get-Content -Raw $f).Trim() -eq $key)
}
function MarkDone($name, $key) { Set-Content -Path (Join-Path $Stamps $name) -Value $key }
function FileHash($path) { (Get-FileHash -Algorithm SHA1 $path).Hash }

# ---------------------------------------------------------------------------
# 1. Toolchain: Visual Studio 2022 (with its Clang, CMake and Ninja) + Git
# ---------------------------------------------------------------------------
Step "Checking build tools"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "Git was not found. Install Git for Windows (https://git-scm.com/download/win) and run setup again."
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio 2022 was not found. See README.md (Requirements) for what to install."
}
$vs = & $vswhere -latest -products * -version "[17.0,)" `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang `
    -property installationPath
if (-not $vs) {
    Fail ("Visual Studio 2022 is installed but is missing components. In the Visual Studio Installer, " +
          "select 'Desktop development with C++' and tick 'C++ Clang Compiler for Windows'.")
}
# Load the x64 developer environment (compiler, Windows SDK, CMake, Ninja).
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + $env:PATH
foreach ($tool in "clang", "clang++", "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "'$tool' was not found even after loading the Visual Studio environment. See README.md."
    }
}
Write-Host "Visual Studio: $vs"

# ---------------------------------------------------------------------------
# 2. Extract the game from your disc image
# ---------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $GameDir "default.xex"))) {
    if (-not $Iso) {
        Write-Host "`nSelect your Saints Row (Xbox 360) disc image (.iso)." -ForegroundColor Yellow
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.OpenFileDialog
            $dlg.Filter = "Xbox 360 disc image (*.iso)|*.iso|All files (*.*)|*.*"
            $dlg.Title = "Select your Saints Row disc image"
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Iso = $dlg.FileName }
        } catch { }
        if (-not $Iso) { $Iso = (Read-Host "Path to your Saints Row .iso").Trim('"') }
    }
    if (-not (Test-Path $Iso)) { Fail "Disc image not found: $Iso" }

    Step "Building the disc image extractor"
    $extractor = Join-Path $Build "xiso_extract.exe"
    Run "clang++" @("-std=c++17", "-O2", "-o", $extractor, (Join-Path $Root "tools\xiso_extract\xiso_extract.cpp"))

    Step "Extracting the game from $Iso (about 6 GB)"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $GameDir
    Run $extractor @($Iso, $GameDir,
        "--skip-name", "`$SystemUpdate", "--skip-name", "layer1filler.bin", "--skip-name", "layer2filler.bin")
    if (-not (Test-Path (Join-Path $GameDir "default.xex"))) {
        Fail "The disc image does not contain default.xex - is this an Xbox 360 game disc?"
    }
} else {
    Write-Host "Game files already extracted to dist\game - skipping extraction."
}

# The hooks in this project are tied to one specific build of the game.
$knownXex = "C2646093FF3926141FCF4FD630E876873D1CAECE"   # Saints Row (World), disc version
$xexHash = (Get-FileHash -Algorithm SHA1 (Join-Path $GameDir "default.xex")).Hash
if ($xexHash -ne $knownXex) {
    Write-Host ("WARNING: your default.xex (SHA-1 $xexHash) is not the version this port was made " +
                "for. The build may fail or the game may not run correctly.") -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 3. ReXGlue SDK (runtime + recompiler), patched for Saints Row
# ---------------------------------------------------------------------------
$sdkKey = "$SdkCommit " + (FileHash (Join-Path $Root "patches\rexglue-sdk.patch"))
if (-not (Done "sdk" $sdkKey)) {
    if (-not (Test-Path (Join-Path $SdkSrc ".git"))) {
        Step "Downloading the ReXGlue SDK"
        # autocrlf off so patches/rexglue-sdk.patch (LF) applies cleanly.
        Run "git" @("clone", "-c", "core.autocrlf=false", $SdkRepo, $SdkSrc)
    }
    Push-Location $SdkSrc
    try {
        Step "Checking out ReXGlue SDK $($SdkCommit.Substring(0, 7)) and its dependencies"
        Run "git" @("-c", "advice.detachedHead=false", "checkout", "--force", $SdkCommit)
        # Files an older version of the patch added are not tracked by the SDK,
        # so the checkout leaves them behind and the new patch would not apply.
        Run "git" @("clean", "-fdq", "--", "src", "include", "resources", "cmake")
        # Shallow submodules save several GB; fall back to full history if a
        # server refuses to serve a pinned commit shallowly.
        & git submodule update --init --recursive --force --depth 1
        if ($LASTEXITCODE -ne 0) {
            Run "git" @("submodule", "update", "--init", "--recursive", "--force")
        }

        # Git on Windows checks libmspack's symlinked sources out as small text
        # files containing the link target; replace them with the real files.
        $mspack = Join-Path $SdkSrc "thirdparty\libmspack\cabextract\mspack"
        if (Test-Path $mspack) {
            Get-ChildItem -Path $mspack -File | Where-Object { $_.Length -lt 300 } | ForEach-Object {
                $text = (Get-Content -Raw $_.FullName).Trim()
                if ($text -match '^(\.\./)+[\w./\\-]+$') {
                    $target = [System.IO.Path]::GetFullPath((Join-Path $_.DirectoryName $text))
                    if (Test-Path $target) { Copy-Item -Force $target $_.FullName }
                }
            }
        }

        Step "Applying the Saints Row patch to the SDK"
        $patch = Join-Path $Root "patches\rexglue-sdk.patch"
        & git apply --check $patch 2>$null
        if ($LASTEXITCODE -eq 0) {
            Run "git" @("apply", "--whitespace=nowarn", $patch)
        } else {
            & git apply --reverse --check $patch 2>$null
            if ($LASTEXITCODE -ne 0) { Fail "The SDK patch does not apply. Try deleting the build folder and running setup again." }
            Write-Host "Patch already applied."
        }

        Step "Building the ReXGlue SDK (this takes a while)"
        Run "cmake" @("--preset", "win-amd64", "-DCMAKE_INSTALL_PREFIX=$SdkInstall")
        Run "cmake" @("--build", "out/build/win-amd64", "--target", "install", "--config", "Release")
    } finally {
        Pop-Location
    }
    MarkDone "sdk" $sdkKey
} else {
    Write-Host "ReXGlue SDK already built - skipping."
}

# ---------------------------------------------------------------------------
# 4. Recompile the game executable (PowerPC -> C++)
# ---------------------------------------------------------------------------
$codegenKey = "$sdkKey " + (FileHash (Join-Path $Root "config\saintsrow_manifest.toml"))
if (-not (Done "codegen" $codegenKey)) {
    Step "Recompiling default.xex (PowerPC -> C++)"
    $rexglue = Join-Path $SdkInstall "bin\rexglue.exe"
    Run $rexglue @("codegen", (Join-Path $Root "config\saintsrow_manifest.toml"), "--ignore-stamp")
    MarkDone "codegen" $codegenKey
} else {
    Write-Host "Recompiled code already generated - skipping."
}

# ---------------------------------------------------------------------------
# 5. Build saintsrow.exe
# ---------------------------------------------------------------------------
Step "Building Saints Reborn (compiles ~34,000 functions; expect 15-60 minutes)"
$env:REXSDK = $SdkInstall
Run "cmake" @("-S", (Join-Path $Root "project"), "-B", $GameBuild, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
    "-DSR_GENERATED_DIR=$(Join-Path $Build 'generated')")
Run "cmake" @("--build", $GameBuild)

Step "Copying the game to dist"
try {
    Copy-Item -Force -ErrorAction Stop (Join-Path $GameBuild "saintsrow.exe") $Dist
    Copy-Item -Force -ErrorAction Stop (Join-Path $SdkInstall "bin\*.dll") $Dist
    Copy-Item -Force -ErrorAction Stop (Join-Path $GameBuild "WhompaysModLoader.exe") $Dist

    # Whompay's Mod Loader: the mods folder, bundled mods and examples (off by default).
    $ModsDir = Join-Path $Dist "mods"
    New-Item -ItemType Directory -Force -Path $ModsDir | Out-Null
    $bundled = @(Get-ChildItem -Directory (Join-Path $Root "modding\examples")) +
               @(Get-ChildItem -Directory (Join-Path $Root "modding\mods"))
    foreach ($example in $bundled) {
        $target = Join-Path $ModsDir $example.Name
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -Recurse -Force -ErrorAction Stop -Exclude "*.c","*.cpp","*.ps1" (Join-Path $example.FullName "*") $target
    }
    Copy-Item -Force -ErrorAction Stop (Join-Path $GameBuild "ExampleNative.dll") (Join-Path $ModsDir "ExampleNative")
    Copy-Item -Force -ErrorAction Stop (Join-Path $GameBuild "WhompaysTrainer.dll") (Join-Path $ModsDir "WhompaysTrainer")

    # Built-in parts (the Saints Reborn logo): always on, not in the mod list.
    $CoreDir = Join-Path $Dist "core"
    foreach ($part in Get-ChildItem -Directory (Join-Path $Root "core")) {
        $target = Join-Path $CoreDir $part.Name
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -Recurse -Force -ErrorAction Stop -Exclude "*.png","*.py" (Join-Path $part.FullName "*") $target
    }
} catch {
    Fail "Could not copy the game to dist ($($_.Exception.Message)). If Saints Reborn is running, close it and run setup again."
}

# Keyboard/mouse button pictures, made from your own game files (they contain
# parts of the game's textures, so they are not part of the download).
Step "Making the keyboard/mouse button pictures"
& (Join-Path $GameBuild "glyphgen.exe") (Join-Path $GameDir "packfiles") (Join-Path $Root "tools\glyphgen\art.txt") (Join-Path $Dist "kbm_ui.bin")
if ($LASTEXITCODE -ne 0) {
    Write-Host "Warning: the keyboard/mouse button pictures could not be made; the game will show controller buttons." -ForegroundColor Yellow
}

Stop-Transcript | Out-Null
Write-Host "`nDone! Run dist\WhompaysModLoader.exe to choose mods and play, or dist\saintsrow.exe to play directly." -ForegroundColor Green
Write-Host "F11 toggles fullscreen. See README.md for options."
if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
