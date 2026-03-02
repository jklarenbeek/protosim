@echo off
echo ==========================================
echo Starting protosim setup for Windows...
echo ==========================================

:: Check if running with Administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [WARNING] This script may require Administrator privileges for some installations (e.g. via winget).
    echo If the installation fails, please right-click and 'Run as Administrator'.
    echo.
)

:: Run the PowerShell script
PowerShell -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0setup-windows-env.ps1'"

pause
