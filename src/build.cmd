@echo off
rem Builds dist\ClawdTray-Setup.exe
rem Needs: the Claude desktop app (for the Clawd art), Node.js, and Visual Studio / Build Tools with the C++ workload.
setlocal
cd /d "%~dp0"

echo [1/2] Extracting the Clawd frames from your Claude app...
node ..\tools\make-frames.js || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo Visual Studio Installer not found. Install Visual Studio or Build Tools with "Desktop development with C++". & exit /b 1 )
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo No Visual Studio with the C++ tools found. & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

echo [2/2] Compiling...
if not exist ..\dist mkdir ..\dist
rc /nologo /fo clawdtray.res clawdtray.rc || exit /b 1
cl /nologo /O1 /Os /MT /GS- /W3 /DUNICODE /D_UNICODE clawdtray.c clawdtray.res /Fe..\dist\ClawdTray-Setup.exe /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:clawdtray.manifest /DELAYLOAD:ole32.dll /DELAYLOAD:oleaut32.dll || exit /b 1
del /q clawdtray.obj clawdtray.res 2>nul
echo Built dist\ClawdTray-Setup.exe
