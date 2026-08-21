# Find Windows SDK location
$kitsRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kitsRoot) {
    $kitsRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" -ErrorAction SilentlyContinue).KitsRoot10
}
Write-Host "KitsRoot10: $kitsRoot"

if ($kitsRoot) {
    $versions = Get-ChildItem (Join-Path $kitsRoot "Include") -Directory -ErrorAction SilentlyContinue | Sort-Object Name
    foreach ($v in $versions) { Write-Host "SDK version: $($v.Name)" }
    $winsock = Get-ChildItem (Join-Path $kitsRoot "Include") -Recurse -Filter "winsock2.h" -ErrorAction SilentlyContinue | Select-Object -First 1
    Write-Host "winsock2.h: $($winsock.FullName)"
} else {
    # Try common locations
    $commonPaths = @(
        "C:\Program Files (x86)\Windows Kits\10",
        "C:\Program Files\Windows Kits\10"
    )
    foreach ($p in $commonPaths) {
        if (Test-Path $p) {
            Write-Host "Found SDK at: $p"
            Get-ChildItem (Join-Path $p "Include") -Directory -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  Version: $($_.Name)" }
        } else {
            Write-Host "Not found: $p"
        }
    }
}
