@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" > nul 2>&1
echo WindowsSdkDir=[%WindowsSdkDir%]
echo WindowsSDKVersion=[%WindowsSDKVersion%]
echo INCLUDE=[%INCLUDE%]
reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2>nul
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2>nul
