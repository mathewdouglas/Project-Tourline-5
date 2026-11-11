@echo off
if not exist "%~dp0build\tourline5.exe" (
    echo Build Project Tourline 5 first:
    echo     scripts\build.cmd -DGT5P_TARGET=BCUS98114-v104
    pause
    exit /b 1
)
"%~dp0build\tourline5.exe"
