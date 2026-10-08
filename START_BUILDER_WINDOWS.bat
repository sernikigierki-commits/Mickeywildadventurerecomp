@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 goto use_python
py -3 -c "import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
if errorlevel 1 goto use_python
py -3 builder.py
goto finished
:use_python
python -c "import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
if errorlevel 1 goto missing_python
python builder.py
goto finished
:missing_python
echo Python 3.10 or newer with Tcl/Tk was not found.
echo Install Python from https://www.python.org/downloads/windows/
echo Enable the launcher, Add python.exe to PATH, and Tcl/Tk during installation.
echo Then restart this launcher.
pause
exit /b 1
:finished
if errorlevel 1 (
  echo Build Studio could not start or closed with an error. See the message above.
  pause
  exit /b 1
)
endlocal
