@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" > nul 2>&1
echo WindowsSdkDir=%WindowsSdkDir%
echo WindowsSDKVersion=%WindowsSDKVersion%
where msbuild
