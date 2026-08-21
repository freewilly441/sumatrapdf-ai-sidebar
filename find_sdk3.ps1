# Search for winsock2.h within VS installation
$vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Write-Host "Searching for winsock2.h in VS installation..."
$found = Get-ChildItem $vsRoot -Recurse -Filter "winsock2.h" -ErrorAction SilentlyContinue | Select-Object -First 5
foreach ($f in $found) { Write-Host "  $($f.FullName)" }
if (-not $found) { Write-Host "  Not found in VS install" }

# Check if WindowsSDK is installed inside VS
$sdkInVs = Join-Path $vsRoot "Windows Kits"
if (Test-Path $sdkInVs) {
    Write-Host "Windows Kits inside VS: $sdkInVs"
    Get-ChildItem $sdkInVs -Recurse -Filter "winsock2.h" -ErrorAction SilentlyContinue | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.FullName)" }
}

# List what Include directories VS has set up
Write-Host "`nVC Tools Include:"
$msvcInclude = Join-Path $vsRoot "VC\Tools\MSVC"
if (Test-Path $msvcInclude) {
    Get-ChildItem $msvcInclude -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
}
