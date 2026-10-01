# Installs the online pack (online play over Epic: the Epic-enabled runtime and
# co-op DLL, the Epic DLL and eos.ini) from the GitHub release "online-pack"
# into dist, when it was built from exactly this source (see online_stamp.ps1).
# Used by setup.ps1 and by the mod loader (which runs it again when a download
# failed, so online play repairs itself without a new update).
#   dist\online_pack_wanted.txt     stamp this build needs (written by setup)
#   dist\online_pack.txt            stamp of the installed pack
# Exit code: 0 installed / already installed, 2 the pack is for another
# version, 1 download or install failed.
param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$Dist = "",
    [string]$Wanted = ""
)
$ErrorActionPreference = "Stop"
if (-not $Dist) { $Dist = Join-Path $Root "dist" }
$wantedFile = Join-Path $Dist "online_pack_wanted.txt"
$installedFile = Join-Path $Dist "online_pack.txt"
try {
    if (-not $Wanted) {
        if (Test-Path $wantedFile) { $Wanted = (Get-Content -Raw $wantedFile).Trim() }
        else {
            $commit = (Select-String -Path (Join-Path $Root "scripts\setup.ps1") -Pattern '^\$SdkCommit = "([0-9a-f]+)"').Matches[0].Groups[1].Value
            . (Join-Path $Root "scripts\online_stamp.ps1")
            $Wanted = Get-OnlineStamp $Root $commit
        }
    }
    Set-Content -NoNewline -Encoding ascii -Path $wantedFile -Value $Wanted
    if ((Test-Path $installedFile) -and ((Get-Content -Raw $installedFile).Trim() -eq $Wanted) -and
        (Test-Path (Join-Path $Dist "core\WhompaysCoop\eos\EOSSDK-Win64-Shipping.dll"))) {
        Write-Host "Online play: on (online pack already installed)."
        exit 0
    }
    $packUrl = "https://github.com/whompay/SaintsReborn/releases/download/online-pack/SaintsReborn-Online.zip"
    $packZip = Join-Path $env:TEMP "SaintsReborn-Online.zip"
    $packDir = Join-Path $env:TEMP "SaintsReborn-Online"
    Remove-Item -Force -ErrorAction SilentlyContinue $packZip
    & curl.exe -sSfL --retry 3 -o $packZip $packUrl 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $packZip)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $packUrl -OutFile $packZip
    }
    if (Test-Path $packDir) { Remove-Item -Recurse -Force $packDir }
    Expand-Archive -Force $packZip $packDir
    $have = (Get-Content -Raw (Join-Path $packDir "stamp.txt")).Trim()
    if ($have -ne $Wanted) {
        Write-Host "Online play: the online pack is for another version of the source, so online play stays off for now (System Link on a LAN and co-op by IP still work)." -ForegroundColor Yellow
        exit 2
    }
    Get-ChildItem -Recurse -File $packDir | Where-Object { $_.Name -ne "stamp.txt" } | ForEach-Object {
        $dest = Join-Path $Dist $_.FullName.Substring($packDir.Length + 1)
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        Copy-Item -Force $_.FullName $dest
    }
    Set-Content -NoNewline -Encoding ascii -Path $installedFile -Value $Wanted
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $packDir, $packZip
    Write-Host "Online play: on (online pack installed)."
    exit 0
} catch {
    Write-Host "Online play: the online pack could not be installed ($($_.Exception.Message)). System Link on a LAN and co-op by IP still work; starting the game from the mod loader tries again." -ForegroundColor Yellow
    exit 1
}
