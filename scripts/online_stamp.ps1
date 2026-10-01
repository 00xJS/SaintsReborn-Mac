# Version stamp of the online pack (Epic-enabled rexruntime.dll + co-op DLL):
# a hash of every source those two DLLs are built from that the game build
# also uses. Setup installs the pack only when its stamp matches the source it
# just built, so the pack's DLLs always fit the rest of the game. Line endings
# are ignored (CR bytes dropped), so a CRLF and an LF checkout match.
function Get-OnlineStamp([string]$Root, [string]$SdkCommit) {
    $files = @(Join-Path $Root "patches\rexglue-sdk.patch")
    $files += @(Get-ChildItem -File (Join-Path $Root "core\WhompaysCoop") |
        Where-Object { $_.Extension -eq ".cpp" -or $_.Extension -eq ".h" } | ForEach-Object { $_.FullName })
    $files += @(Get-ChildItem -File (Join-Path $Root "modding\include") |
        Where-Object { $_.Extension -eq ".h" } | ForEach-Object { $_.FullName })
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text = New-Object Text.StringBuilder
    [void]$text.Append("sdk $SdkCommit`n")
    $rootFull = (Resolve-Path $Root).Path.TrimEnd('\')
    foreach ($f in ($files | Sort-Object { $_.Substring($rootFull.Length).ToLowerInvariant() })) {
        $rel = $f.Substring($rootFull.Length).TrimStart('\').Replace('\', '/').ToLowerInvariant()
        [void]$text.Append("== $rel ==`n")
        [void]$text.Append($latin1.GetString([IO.File]::ReadAllBytes($f)).Replace("`r", ""))
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    ($sha.ComputeHash($latin1.GetBytes($text.ToString())) | ForEach-Object { $_.ToString("x2") }) -join ""
}
