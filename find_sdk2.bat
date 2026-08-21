@echo off
reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" 2>nul
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" 2>nul
echo ---
dir "C:\Program Files (x86)\Windows Kits\10\Include" 2>nul
dir "C:\Program Files\Windows Kits\10\Include" 2>nul
