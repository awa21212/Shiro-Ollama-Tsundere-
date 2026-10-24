@echo off
rem Build Shiro Pet. Requires MinGW-w64 gcc + windres in PATH.
setlocal
cd /d "%~dp0src"

where gcc >nul 2>nul
if errorlevel 1 (
  echo [x] gcc not found in PATH. Install MinGW-w64 and add its bin directory.
  exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
  echo [x] windres not found. Check your MinGW-w64 installation.
  exit /b 1
)

echo [*] windres pet.rc
windres -i pet.rc -O coff -o pet_res.res
if errorlevel 1 exit /b 1

echo [*] pet.exe
gcc -O2 -mwindows -o pet.exe pet.c pet_res.res -lgdiplus -liphlpapi -lwinmm
if errorlevel 1 exit /b 1

echo [*] inference_backend.exe
gcc -O2 -o inference_backend.exe inference_backend.c -lwinhttp
if errorlevel 1 exit /b 1

echo [*] ma.exe
gcc -O2 -o ma.exe ma.c -lwinhttp
if errorlevel 1 exit /b 1

echo [*] done
