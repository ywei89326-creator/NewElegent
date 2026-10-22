@echo off
rem Run the target-detection example.
rem The image path is set in main.cpp (hard-coded sample path).
cd /d "%~dp0"
"%~dp0TargetDetectExample.exe"
if errorlevel 1 (
    echo [ERROR] TargetDetectExample.exe failed.
    pause
    exit /b 1
)
echo.
echo Done.
pause
