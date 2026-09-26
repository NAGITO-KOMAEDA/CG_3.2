@echo off
setlocal
cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release
if errorlevel 1 exit /b 1
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 exit /b 1
copy /Y "build\Release\CG_Homework_4.exe" "bin\CG_Homework_4.exe"
if errorlevel 1 exit /b 1
echo Build and tests completed.
