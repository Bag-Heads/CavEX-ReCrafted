@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_ROOT=%%~fI"
set "DOLPHIN=%USERPROFILE%\Desktop\Tools\Dolphin\dev-2603-49\Dolphin-x64\Dolphin.exe"
set "USERROOT=%USERPROFILE%\Desktop\Tools\Dolphin\cavewii_v2-world1-user"
set "DOL=%USERROOT%\Load\WiiSDSync\apps\CavEX\boot.dol"

if not exist "%DOLPHIN%" (
	echo Dolphin dev was not found at:
	echo %DOLPHIN%
	exit /b 1
)

powershell -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\tools\setup_dolphin_ab_test.ps1" -ProjectRoot "%PROJECT_ROOT%"
if errorlevel 1 exit /b 1

if not exist "%DOL%" (
	echo World1 test boot.dol was not found at:
	echo %DOL%
	exit /b 1
)

start "" /D "%USERROOT%\Load\WiiSDSync\apps\CavEX" "%DOLPHIN%" -u "%USERROOT%" -e "%DOL%"
