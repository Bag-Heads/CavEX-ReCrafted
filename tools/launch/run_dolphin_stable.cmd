@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "PROJECT_ROOT=%%~fI"
set "STAGE_ROOT=%PROJECT_ROOT%\build\dolphin-app\CaveWii_v2"
set "DOL=%STAGE_ROOT%\boot.dol"
set "DOLPHIN=%USERPROFILE%\Desktop\Tools\Dolphin\stable-2603\Dolphin-x64\Dolphin.exe"
set "USERROOT=%USERPROFILE%\Desktop\Tools\Dolphin\cavewii_v2-clean-user"

if not exist "%DOLPHIN%" (
	echo Dolphin stable was not found at:
	echo %DOLPHIN%
	exit /b 1
)

if not exist "%USERROOT%" (
	echo Dolphin user root was not found at:
	echo %USERROOT%
	exit /b 1
)

powershell -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\tools\stage_dolphin_app.ps1" -SourceRoot "%PROJECT_ROOT%" -StageRoot "%STAGE_ROOT%"
if errorlevel 1 exit /b 1

powershell -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\tools\sync_dolphin_controls.ps1" -UserRoot "%USERROOT%"
if errorlevel 1 exit /b 1

if not exist "%DOL%" (
	echo Staged boot.dol was not found at:
	echo %DOL%
	exit /b 1
)

start "" /D "%STAGE_ROOT%" "%DOLPHIN%" -u "%USERROOT%" -e "%DOL%"
