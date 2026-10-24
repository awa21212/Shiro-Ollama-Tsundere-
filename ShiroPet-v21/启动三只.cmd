@echo off
rem Launch three pets. Requires Python 3.
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo [!] Python 3 is required for the three-pet launcher.
  echo     You can also run: pet.exe --slot N
  pause
  exit /b 1
)
python launch_three.py
pause
