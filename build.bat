@echo off
setlocal
cd /d "%~dp0"
g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Wno-cast-function-type -municode -mwindows -static -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 src\main.cpp -o BatteryDock.exe -lhid -lsetupapi -lshell32 -luser32 -lgdi32 -ladvapi32
if errorlevel 1 exit /b 1
echo Built BatteryDock.exe

