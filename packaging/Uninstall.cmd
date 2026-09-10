@echo off
rem Removes the installed program and its Start menu entry. Projects in
rem Documents\CoCompose are deliberately left alone.
setlocal
set "TARGET=%LOCALAPPDATA%\Programs\CoCompose"
set "LINK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\CoCompose.lnk"

echo This removes CoCompose from "%TARGET%".
echo Your projects in "%USERPROFILE%\Documents\CoCompose" are kept.
rem /y removes without asking, which is how a check runs it.
if /I not "%~1"=="/y" (
    choice /C YN /M "Remove CoCompose"
    if errorlevel 2 exit /b 0
)

if exist "%LINK%" del "%LINK%"

rem The script lives inside the folder it is deleting, so the removal is handed to a
rem detached PowerShell that waits for this one to let go of the directory first.
start "" /min "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -WindowStyle Hidden -Command "Start-Sleep -Seconds 2; Remove-Item -LiteralPath '%TARGET%' -Recurse -Force -ErrorAction SilentlyContinue"
echo Removed.
exit /b 0
