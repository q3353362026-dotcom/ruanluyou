@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" %*
set "result=%errorlevel%"
if not "%result%"=="0" (
  echo.
  echo Radaer stopped with an error. Review the message above.
  pause
)
exit /b %result%
