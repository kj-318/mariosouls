@echo off
rem Package the Remastered build as a drag-and-drop release: copies the built host, adapter and
rem runtime with the player README into work\release\MarioSouls-DSR and zips it.  Build first
rem (tools\build_remastered.bat).  Nothing from the game or a ROM goes into the package.
cd /d "%~dp0.."
set B=work\framework-build
set R=work\release\MarioSouls-DSR
set OUT=work\release\last-package.txt
if not exist work\release mkdir work\release
echo package started %DATE% %TIME% > "%OUT%"
if not exist "%B%\dinput8_x64.dll" (echo missing %B%\dinput8_x64.dll - build first >> "%OUT%" & goto done)
if not exist "%B%\portability\x86_64-windows-gnu\mario64.dll" (echo missing mario64.dll >> "%OUT%" & goto done)
if not exist "%B%\portability\x86_64-windows-gnu\runtime\sm64.dll" (echo missing sm64.dll >> "%OUT%" & goto done)
if exist "%R%" rmdir /s /q "%R%"
mkdir "%R%\MarioSouls"
copy /y "%B%\dinput8_x64.dll" "%R%\dinput8.dll" >> "%OUT%"
copy /y "%B%\portability\x86_64-windows-gnu\mario64.dll" "%R%\MarioSouls\mario64.dll" >> "%OUT%"
copy /y "%B%\portability\x86_64-windows-gnu\runtime\sm64.dll" "%R%\MarioSouls\sm64.dll" >> "%OUT%"
copy /y "framework\hosts\ds1remastered\release\README.txt" "%R%\README - MarioSouls.txt" >> "%OUT%"
copy /y "framework\hosts\ds1remastered\release\MarioSouls-settings.txt" "%R%\MarioSouls\settings-example.txt" >> "%OUT%"
if exist work\release\MarioSouls-DSR.zip del /q work\release\MarioSouls-DSR.zip
powershell -NoProfile -Command "Compress-Archive -Path '%R%\*' -DestinationPath 'work\release\MarioSouls-DSR.zip'" >> "%OUT%" 2>&1
if errorlevel 1 (echo zip FAILED >> "%OUT%" & goto done)
echo packaged: work\release\MarioSouls-DSR.zip >> "%OUT%"
:done
echo package finished %DATE% %TIME% >> "%OUT%"
type "%OUT%"
timeout /t 5
