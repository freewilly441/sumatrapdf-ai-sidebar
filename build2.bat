@echo off
setlocal
set LOG=C:\Users\melan\Documents\Aethermark_Systems_LLC\Code_Projects\sumatrapdf-llm\build_log2.txt
echo Build started %DATE% %TIME% > "%LOG%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 >> "%LOG%" 2>&1
echo vcvarsall exit: %ERRORLEVEL% >> "%LOG%"
"C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe" "C:\Users\melan\Documents\Aethermark_Systems_LLC\Code_Projects\sumatrapdf-llm\vs2022\SumatraPDF.sln" /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m:1 /nologo /v:minimal >> "%LOG%" 2>&1
echo Build exit code: %ERRORLEVEL% >> "%LOG%"
endlocal
