@echo off
setlocal
title Ultimate Split-Screen
"%~dp0Enable-Split.exe" --launch
if not errorlevel 1 exit /b 0
pause
exit /b 1
