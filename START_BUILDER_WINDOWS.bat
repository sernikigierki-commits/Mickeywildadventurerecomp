@echo off
cd /d "%~dp0"
where py >nul 2>nul
if %ERRORLEVEL%==0 (
  py -3 builder.py
) else (
  python builder.py
)
if not %ERRORLEVEL%==0 pause
