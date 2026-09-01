@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
"C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe" "%~dp0vs2022\SumatraPDF.sln" /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m:1 /nologo /v:normal
endlocal
