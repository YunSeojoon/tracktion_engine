@echo off
rem Removes the installed program and its Start menu entry. Projects in
rem Documents\CoCompose are deliberately left alone.
rem
rem cmd reads this file from disk as it goes, so a script that waits for its own folder
rem to be deleted stops being readable half way through. Everything that matters is
rem therefore removed and checked here and now: the executable, the shortcut and every
rem other installed file. The only thing left behind is this script and the folder that
rem holds it, and a separate process takes those away once this one has let go.
rem
rem Everything is called by full path. A machine with other tools on its PATH must not
rem get a different powershell than the one Windows ships.
setlocal
set "TARGET=%LOCALAPPDATA%\Programs\CoCompose"
set "LINK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\CoCompose.lnk"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem A user name with an apostrophe in it would end a PowerShell single-quoted
rem string early, so the copies used inside one have their quotes doubled.
set "TARGET_PS=%TARGET:'=''%"
set "LINK_PS=%LINK:'=''%"

if not exist "%TARGET%\CoCompose.exe" (
    echo CoCompose is not installed in "%TARGET%".
    if /I not "%~1"=="/y" pause
    exit /b 2
)

rem A running copy holds its own executable open, and nothing below can remove it.
"%PS%" -NoProfile -Command "if (Get-Process -Name CoCompose -ErrorAction SilentlyContinue) { exit 1 }; exit 0"
if errorlevel 1 (
    echo CoCompose is still running. Close it first, then run this again.
    if /I not "%~1"=="/y" pause
    exit /b 3
)

echo This removes CoCompose from "%TARGET%".
echo Your projects in "%USERPROFILE%\Documents\CoCompose" are kept.
rem /y removes without asking, which is how a check runs it.
if /I not "%~1"=="/y" (
    choice /C YN /M "Remove CoCompose"
    if errorlevel 2 exit /b 0
)

if exist "%LINK%" del "%LINK%"

rem Remove everything except this script, then say what is actually left. The exit code
rem below is the real answer, not an assumption that the removal worked.
"%PS%" -NoProfile -Command "$target = '%TARGET_PS%'; Get-ChildItem -LiteralPath $target -Force | Where-Object { $_.Name -ne 'Uninstall.cmd' } | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }; $left = @(Get-ChildItem -LiteralPath $target -Force | Where-Object { $_.Name -ne 'Uninstall.cmd' }); if ($left.Count -gt 0) { $left.Name -join ', '; exit 1 }; if (Test-Path -LiteralPath '%LINK_PS%') { 'the Start menu shortcut'; exit 1 }; exit 0"
if errorlevel 1 (
    echo.
    echo Could not remove everything. Files are still in:
    echo   %TARGET%
    echo Close anything using them and delete that folder by hand, or run this again.
    if /I not "%~1"=="/y" pause
    exit /b 4
)

rem All that is left is this script and its folder, which cannot be deleted from inside
rem itself. A detached process does it once this one has exited.
start /min "" "%PS%" -NoProfile -WindowStyle Hidden -Command "Start-Sleep -Seconds 2; Remove-Item -LiteralPath '%TARGET_PS%' -Recurse -Force -ErrorAction SilentlyContinue"

echo Removed. Your projects are still in "%USERPROFILE%\Documents\CoCompose".
if /I not "%~1"=="/y" pause
exit /b 0
