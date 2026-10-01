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
    Remove-Item -Force -ErrorAction SilentlyContinue $packZip
    & curl.exe -sSfL --retry 3 -o $packZip $packUrl 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $packZip)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $packUrl -OutFile $packZip
    }
    # Straight from the zip to dist (no temp folder: on some PCs TEMP is a short
    # 8.3 path such as C:\Users\ADMINI~1\..., which broke relative paths).
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($packZip)
    try {
        $stampEntry = $zip.Entries | Where-Object { $_.FullName -eq "stamp.txt" } | Select-Object -First 1
        if (-not $stampEntry) { throw "the online pack has no stamp.txt" }
        $reader = New-Object IO.StreamReader($stampEntry.Open())
        $have = $reader.ReadToEnd().Trim(); $reader.Close()
        if ($have -ne $Wanted) {
            Write-Host "Online play: the online pack is for another version of the source, so online play stays off for now (System Link on a LAN and co-op by IP still work)." -ForegroundColor Yellow
            exit 2
        }
        $distFull = [IO.Path]::GetFullPath($Dist)
        foreach ($entry in $zip.Entries) {
            $rel = $entry.FullName.Replace('/', '\')
            if (-not $entry.Name -or $rel -eq "stamp.txt") { continue }
            $dest = [IO.Path]::GetFullPath((Join-Path $distFull $rel))
            if (-not $dest.StartsWith($distFull + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "bad path in the online pack: $rel" }
            New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
        }
    } finally { $zip.Dispose() }
    if (-not (Test-Path (Join-Path $Dist "core\WhompaysCoop\eos\EOSSDK-Win64-Shipping.dll"))) { throw "the Epic DLL is missing after installing" }
    Set-Content -NoNewline -Encoding ascii -Path $installedFile -Value $Wanted
    Remove-Item -Force -ErrorAction SilentlyContinue $packZip
    Write-Host "Online play: on (online pack installed)."
    exit 0
} catch {
    Write-Host "Online play: the online pack could not be installed ($($_.Exception.Message)). System Link on a LAN and co-op by IP still work; starting the game from the mod loader tries again." -ForegroundColor Yellow
    exit 1
}
