# Installs a packaged build the way a person would, on a machine with no developer
# tools reachable, then replaces it with another copy and checks the work survived.
# Finally removes it and checks the project is still there.
#
# Usage: .\tools\test_install_and_update.ps1 -Unpacked 'C:\...\unpacked'
param([Parameter(Mandatory = $true)][string]$Unpacked)
$ErrorActionPreference = 'Stop'

$installed = Join-Path $env:LOCALAPPDATA 'Programs\CoCompose'
$shortcut  = Join-Path ([Environment]::GetFolderPath('Programs')) 'CoCompose.lnk'
$work      = Join-Path ([IO.Path]::GetTempPath()) ("cocompose-update-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$project   = Join-Path $work 'project.json'
$state     = Join-Path $work 'state.json'
New-Item -ItemType Directory -Path $work | Out-Null

# Only the stock Windows directories, so nothing here can lean on a developer tool.
$clean = "$env:SystemRoot\system32;$env:SystemRoot;$env:SystemRoot\system32\Wbem"
$saved = $env:PATH

function Use-CleanPath([scriptblock]$Body) {
    $env:PATH = $clean
    try { & $Body } finally { $env:PATH = $saved }
}

function Start-App([string]$Exe, [string]$ProjectPath) {
    $proc = $null
    # One pre-quoted string: PowerShell 5.1 does not quote array arguments itself, so a
    # path with a space in it would reach the app as two arguments.
    $arguments = '--project "' + $ProjectPath + '"'
    Use-CleanPath { $script:proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) `
                                                 -ArgumentList $arguments -PassThru }
    $proc = $script:proc
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline -and -not (Test-Path $state)) {
        if ($proc.HasExited) { throw "the app exited early with code $($proc.ExitCode)" }
        Start-Sleep -Milliseconds 500
    }
    if (-not (Test-Path $state)) { throw 'the app never wrote state.json' }
    Start-Sleep -Seconds 3
    return $proc
}

function Stop-App($Proc) {
    if ($Proc.HasExited) { return }
    $Proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 3
    if (-not $Proc.HasExited) { $Proc.Kill() }
    $Proc.WaitForExit(20000) | Out-Null
}

function Read-Project {
    $s = Get-Content $state -Raw | ConvertFrom-Json
    $notes = ($s.patterns | ForEach-Object { $_.sequences } | ForEach-Object { $_.notes.Count } | Measure-Object -Sum).Sum
    return [pscustomobject]@{ channels = $s.channels.Count; patterns = $s.patterns.Count; notes = $notes; bpm = $s.bpm }
}

$report = [ordered]@{}
try {
    # --- install ------------------------------------------------------------------
    if (Test-Path $installed) { Remove-Item $installed -Recurse -Force }
    Use-CleanPath { & (Join-Path $Unpacked 'Install.cmd') /s | Out-Null }
    if ($LASTEXITCODE -ne 0) { throw "Install.cmd failed: $LASTEXITCODE" }

    $exe = Join-Path $installed 'CoCompose.exe'
    if (-not (Test-Path $exe))      { throw 'the installer left no executable' }
    if (-not (Test-Path $shortcut)) { throw 'the installer left no Start menu shortcut' }
    $report.installed = $true
    $report.shortcut = $true

    # --- make some work worth keeping ---------------------------------------------
    $proc = Start-App $exe $project
    try {
        $before = Read-Project
        if ($before.channels -lt 1) { throw 'the app did not create a project to keep' }
    } finally { Stop-App $proc }
    $report.before = $before

    $installedBuild = (Get-Content (Join-Path $installed 'BUILD-INFO.json') -Raw | ConvertFrom-Json)
    $report.version = $installedBuild.version

    # --- update: the same install run again over the top --------------------------
    Use-CleanPath { & (Join-Path $Unpacked 'Install.cmd') /s | Out-Null }
    if ($LASTEXITCODE -ne 0) { throw "the update install failed: $LASTEXITCODE" }
    if (-not (Test-Path $exe))      { throw 'the update left no executable' }
    if (-not (Test-Path $shortcut)) { throw 'the update lost the Start menu shortcut' }

    $proc = Start-App $exe $project
    try { $after = Read-Project } finally { Stop-App $proc }
    $report.after = $after

    foreach ($field in @('channels', 'patterns', 'notes', 'bpm')) {
        if ($after.$field -ne $before.$field) {
            throw "the update changed $field : $($before.$field) -> $($after.$field)"
        }
    }
    $report.project_survived_update = $true

    # --- a removal that cannot work has to say so ----------------------------------
    # The app holds its own executable open while it runs, so nothing can remove it.
    # Reporting success there would leave a half-removed install looking finished.
    $running = Start-App $exe $project
    try {
        Use-CleanPath { & (Join-Path $installed 'Uninstall.cmd') /y | Out-Null }
        $refused = $LASTEXITCODE
    } finally { Stop-App $running }

    if ($refused -eq 0)       { throw 'removing a running CoCompose reported success' }
    if (-not (Test-Path $exe)) { throw 'the refused removal deleted the program anyway' }
    if (-not (Test-Path $shortcut)) { throw 'the refused removal took the shortcut with it' }
    $report.refused_while_running = $refused

    # --- remove, and keep the work ------------------------------------------------
    Use-CleanPath { & (Join-Path $installed 'Uninstall.cmd') /y | Out-Null }
    if ($LASTEXITCODE -ne 0) { throw "Uninstall.cmd reported $LASTEXITCODE" }
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and (Test-Path $exe)) { Start-Sleep -Milliseconds 500 }
    if (Test-Path $exe)      { throw 'the uninstaller left the program behind' }
    if (Test-Path $shortcut) { throw 'the uninstaller left the Start menu shortcut behind' }
    if (-not (Test-Path $state)) { throw 'the uninstaller took the project with it' }
    $report.uninstalled = $true
    $report.project_survived_uninstall = $true

    [pscustomobject]$report | ConvertTo-Json -Compress
}
finally {
    $env:PATH = $saved
    if (Test-Path $work) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
}
