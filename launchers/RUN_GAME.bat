@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "CUE="
for %%F in ("%~dp0disc\*.cue") do if not defined CUE if exist "%%~fF" set "CUE=%%~fF"
if not defined CUE (
  echo No CUE file found in the disc folder.
  pause
  exit /b 1
)
"%~dp0MickeyWildAdventureRecompiled.exe" --game "%~dp0game.toml" --disc "%CUE%"
exit /b %ERRORLEVEL%
