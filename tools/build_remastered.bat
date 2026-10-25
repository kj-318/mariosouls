@echo off
rem Compile test shaders, then build the Remastered host, recording the result for Claude.
rem Does not install anything or touch the game.
cd /d "%~dp0.."
set OUT=work\framework-build\last-build.txt
echo build started %DATE% %TIME% > "%OUT%"
python tools\compile_test_shaders.py >> "%OUT%" 2>&1
if errorlevel 1 (echo shader compile FAILED >> "%OUT%" & goto done)
python framework\hosts\ds1remastered\build.py >> "%OUT%" 2>&1
echo exit code %ERRORLEVEL% >> "%OUT%"
:done
echo build finished %DATE% %TIME% >> "%OUT%"
type "%OUT%"
timeout /t 5
