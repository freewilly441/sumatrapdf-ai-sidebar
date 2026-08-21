# Build SumatraPDF-LLM
# Writes a clean temp bat then runs it via cmd so there are zero PS quoting issues.

$LOG    = "C:\Users\melan\Documents\Aethermark_Systems_LLC\Code_Projects\sumatrapdf-llm\build_log.txt"
$TMPBAT = "C:\Users\melan\AppData\Local\Temp\sumatra_build.bat"

$bat = @'
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
echo vcvarsall exit: %ERRORLEVEL%
echo WindowsSdkDir=%WindowsSdkDir%
"C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe" "C:\Users\melan\Documents\Aethermark_Systems_LLC\Code_Projects\sumatrapdf-llm\vs2022\SumatraPDF.sln" /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m:1 /nologo /v:minimal
echo Build exit code: %ERRORLEVEL%
'@

$bat | Out-File $TMPBAT -Encoding ascii

"Build started $(Get-Date -Format 'HH:mm:ss')" | Out-File $LOG -Encoding utf8

$output = & cmd.exe /c $TMPBAT 2>&1
$ec = $LASTEXITCODE

$output | Out-File $LOG -Append -Encoding utf8
"" | Out-File $LOG -Append -Encoding utf8
"DONE (exit=$ec)" | Out-File $LOG -Append -Encoding utf8
