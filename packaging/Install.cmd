@echo off
rem Copies this folder into the user's own programs directory and puts CoCompose in
rem the Start menu. Needs nothing installed: no admin rights, no developer tools.
setlocal
rem /s installs without waiting for a key, which is how a check runs it.
set "SILENT="
if /I "%~1"=="/s" set "SILENT=1"
set "SOURCE=%~dp0"
set "TARGET=%LOCALAPPDATA%\Programs\CoCompose"
rem A user name with an apostrophe in it would end a PowerShell single-quoted
rem string early, so the copy used inside one has its quotes doubled.
set "TARGET_PS=%TARGET:'=''%"

echo Installing CoCompose into "%TARGET%"
if not exist "%TARGET%" mkdir "%TARGET%" || goto :failed

rem /Y overwrites an older copy in place. Projects live in Documents\CoCompose and
rem are never touched by this, so an update keeps the work.
xcopy "%SOURCE%*" "%TARGET%\" /E /I /Y /Q >nul || goto :failed

rem By full path, so a machine with an unusual PATH still gets its shortcut.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'CoCompose.lnk'));" ^
  "$s.TargetPath = '%TARGET_PS%\CoCompose.exe'; $s.WorkingDirectory = '%TARGET_PS%'; $s.Description = 'CoCompose'; $s.Save()" || goto :failed

echo.
echo Done. CoCompose is in the Start menu.
echo Your projects are kept in "%USERPROFILE%\Documents\CoCompose" and are not part
echo of the install, so replacing this build later leaves them alone.
echo Run Uninstall.cmd in "%TARGET%" to remove the program.
if not defined SILENT pause
exit /b 0

:failed
echo.
echo Install failed.
if not defined SILENT pause
exit /b 1
