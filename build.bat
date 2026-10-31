@echo off
rem Builds waitwhat.exe with MinGW-w64 gcc (e.g. from MSYS2: pacman -S mingw-w64-x86_64-gcc)
cd /d "%~dp0"
gcc -O2 -std=gnu11 -Wall -Wno-unused-function -Ivendor ^
  src\main.c src\ui.c src\data.c src\net.c vendor\microui.c vendor\cJSON.c ^
  -o waitwhat.exe -mwindows -static ^
  -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lwinhttp -lbcrypt
if errorlevel 1 exit /b 1
echo Built waitwhat.exe
