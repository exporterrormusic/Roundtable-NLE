@echo off
echo ============================================
echo  ROUNDTABLE - Update Default Workspace Layout
echo ============================================
echo.
echo Reads your "USE_AS_DEFAULT" workspace preset
echo and saves it as the shipped default layout.
echo.
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\update_workspace.ps1"
if %ERRORLEVEL% neq 0 (
    echo.
    echo Update failed. Build the project first, then try again.
    pause
    exit /b 1
)
pause
