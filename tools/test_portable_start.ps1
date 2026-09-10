# Starts an extracted portable build the way a double-click does, with no developer
# tools reachable on PATH, and confirms the existing default project comes back.
# Usage: .\tools\test_portable_start.ps1 -Exe 'C:\extracted\CoCompose.exe'
param([Parameter(Mandatory = $true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$exe  = $Exe
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'CoCompose'
$state = Join-Path $docs 'state.json'

# Only the stock Windows directories: no Visual Studio, CMake, Python or Git.
$clean = "$env:SystemRoot\system32;$env:SystemRoot;$env:SystemRoot\system32\Wbem"
$saved = $env:PATH
$env:PATH = $clean
foreach ($leaked in @('cmake','python','cl','git','msbuild')) {
    if (Get-Command $leaked -ErrorAction SilentlyContinue) { throw "developer tool still reachable: $leaked" }
}
$env:PATH = $saved

# On a machine that has never run it there is nothing to restore yet, so make the
# default project first and then check that a second run brings it back.
if (-not (Test-Path $state)) {
    $seed = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
    try {
        $deadline = (Get-Date).AddSeconds(90)
        while ((Get-Date) -lt $deadline -and -not (Test-Path $state)) { Start-Sleep -Milliseconds 500 }
        if (-not (Test-Path $state)) { throw 'the app never created a default project' }
        Start-Sleep -Seconds 2
    }
    finally {
        if (-not $seed.HasExited) { $seed.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 3 }
        if (-not $seed.HasExited) { $seed.Kill() }
        $seed.WaitForExit(20000) | Out-Null
    }
}

$before = Get-Content $state -Raw | ConvertFrom-Json
$beforeWrite = (Get-Item $state).LastWriteTimeUtc

$env:PATH = $clean

$proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
$env:PATH = $saved
try {
    $deadline = (Get-Date).AddSeconds(60)
    $title = $null
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { throw "process exited early with code $($proc.ExitCode)" }
        $proc.Refresh()
        if ($proc.MainWindowTitle) { $title = $proc.MainWindowTitle; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $title) { throw 'no main window appeared within 60s' }

    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline -and (Get-Item $state).LastWriteTimeUtc -le $beforeWrite) {
        Start-Sleep -Milliseconds 500
    }
    if ((Get-Item $state).LastWriteTimeUtc -le $beforeWrite) { throw 'state.json was never refreshed' }

    $after = Get-Content $state -Raw | ConvertFrom-Json
    if ($after.channels.Count -ne $before.channels.Count) { throw "channel count changed: $($before.channels.Count) -> $($after.channels.Count)" }
    if ($after.patterns.Count -ne $before.patterns.Count) { throw "pattern count changed: $($before.patterns.Count) -> $($after.patterns.Count)" }
    $countNotes = { param($state) ($state.patterns | ForEach-Object { $_.sequences } | ForEach-Object { $_.notes.Count } | Measure-Object -Sum).Sum }
    $beforeNotes = & $countNotes $before
    $afterNotes = & $countNotes $after
    if ($afterNotes -ne $beforeNotes) { throw "note count changed: $beforeNotes -> $afterNotes" }
    if ($null -eq $after.bpm) { throw 'state.json has no bpm field' }
    if ($after.bpm -ne $before.bpm) { throw "bpm changed: $($before.bpm) -> $($after.bpm)" }

    [pscustomobject]@{
        window       = $title
        channels     = $after.channels.Count
        patterns     = $after.patterns.Count
        notes        = $afterNotes
        bpm          = $after.bpm
        firstChannel = $after.channels[0].name
        restored     = $true
    } | ConvertTo-Json -Compress
}
finally {
    if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 3 }
    if (-not $proc.HasExited) { $proc.Kill(); }
}
