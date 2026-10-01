param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$compiler = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang++.exe'
$env:PATH = (Split-Path $compiler) + ';' + $env:PATH
$out = Join-Path $root 'build/coop'
$dest = Join-Path $root 'dist/core/WhompaysCoop'
New-Item -ItemType Directory -Force $out,$dest | Out-Null
$common = @('-std=c++23','-O2','-msse4.1','-D_CRT_SECURE_NO_WARNINGS',
    "-I$root/build/sdk/include", "-I$root/modding/include")
# Online sessions (Epic Online Services): the SDK and the product's IDs live
# outside the repository (-Eos folder, or $env:SR_EOS, or ..\..\_work\eos:
# SDK\ + eos.ini). Without them co-op builds without online support.
if ($env:SR_EOS) { $eos = $env:SR_EOS } else { $eos = Join-Path $root '../../_work/eos' }
$eosOn = (Test-Path (Join-Path $eos 'SDK/Include/eos_sdk.h'))
if ($eosOn) { $common += @('-DWHOMPAYS_EOS', "-I$eos/SDK/Include") }
if ($Test) {
    & $compiler @common (Join-Path $PSScriptRoot 'native_test.cpp') '-lws2_32' '-o' (Join-Path $out 'native_test.exe')
    if ($LASTEXITCODE) { throw 'Co-op tests failed to compile.' }
    & (Join-Path $out 'native_test.exe') $dest
    if ($LASTEXITCODE) { throw 'Co-op tests failed.' }
    return
}
& $compiler @common (Join-Path $PSScriptRoot 'coop.cpp') `
    '-shared' '-lws2_32' '-o' (Join-Path $out 'WhompaysCoop.dll')
if ($LASTEXITCODE) { throw 'Co-op mod build failed.' }
Copy-Item (Join-Path $out 'WhompaysCoop.dll'),(Join-Path $PSScriptRoot 'mod.ini'),
    (Join-Path $PSScriptRoot 'README.md') $dest -Force
if ($eosOn) {
    New-Item -ItemType Directory -Force (Join-Path $dest 'eos') | Out-Null
    Copy-Item (Join-Path $eos 'SDK/Bin/EOSSDK-Win64-Shipping.dll') (Join-Path $dest 'eos') -Force
    if (Test-Path (Join-Path $eos 'eos.ini')) { Copy-Item (Join-Path $eos 'eos.ini') $dest -Force }
    Write-Host "Online (EOS) support included"
}
if (-not (Test-Path (Join-Path $dest 'join_ip.txt'))) {
    Copy-Item (Join-Path $PSScriptRoot 'join_ip.txt') $dest
}
Write-Host "Built and installed: $dest"
