# Try to find Windows SDK via MSBuild property evaluation
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe"

# Check VS's own SDK registration
$vsInstances = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build"
)
foreach ($d in $vsInstances) {
    if (Test-Path $d) {
        Write-Host "Found: $d"
        Get-ChildItem $d -Filter "*.props" | Select-Object Name | ForEach-Object { Write-Host "  $($_.Name)" }
    }
}

# Check Windows Kits in non-standard locations
$drives = Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Root -notmatch '^[A-Z]:\\$' -or $_.Name -eq 'C' }
Write-Host "`nSearching for winsock2.h on C: drive (limited search)..."

# Try known install locations
$paths = @(
    "C:\Windows Kits",
    "C:\WinSDK",
    "C:\SDK",
    "${env:ProgramFiles}\Windows Kits\10\Include",
    "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
)
foreach ($p in $paths) {
    $exists = Test-Path $p -ErrorAction SilentlyContinue
    Write-Host "$p => $exists"
}

# Check VS's own default SDK props file
$sdkProps = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\Microsoft.CppBuild.targets"
if (Test-Path $sdkProps) {
    Write-Host "`nFound CppBuild.targets"
    # Look for WindowsSDKDir in there
    $content = Get-Content $sdkProps -ErrorAction SilentlyContinue | Select-String "WindowsSDKDir|KitsRoot" | Select-Object -First 10
    $content | ForEach-Object { Write-Host "  $_" }
}
