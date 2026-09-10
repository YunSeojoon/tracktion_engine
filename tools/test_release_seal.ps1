# The seal is the last thing between a build and the people who download it, so it gets
# its own check. Every case here uses throwaway fixtures; the real distribution is never
# touched. What has to hold: only a passing report, made against the binary that is
# actually in this ZIP, gets a distribution sealed.
param([string]$Work)
$ErrorActionPreference = 'Stop'
$seal = Join-Path $PSScriptRoot 'finish_release.ps1'
if (-not $Work) { $Work = Join-Path ([IO.Path]::GetTempPath()) ("cocompose-seal-" + [guid]::NewGuid().ToString('N').Substring(0, 8)) }
New-Item -ItemType Directory -Path $Work -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-Distribution([string]$Name, [byte[]]$Executable) {
    $staging = Join-Path $Work "stage-$Name"
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $staging 'CoCompose.exe'), $Executable)
    $zip = Join-Path $Work "$Name.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    [IO.Compression.ZipFile]::CreateFromDirectory($staging, $zip)
    return $zip
}

function New-Report([string]$Name, $Body) {
    $path = Join-Path $Work "$Name.json"
    $Body | ConvertTo-Json -Depth 5 | Set-Content $path -Encoding utf8
    return $path
}

function Digest([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    return (($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('X2') }) -join '')
}

function Should-Refuse([string]$Why, [string]$ZipPath, [string]$ReportPath, [string[]]$Extra = @()) {
    $before = if (Test-Path $ZipPath) { (Get-FileHash $ZipPath -Algorithm SHA256).Hash } else { '' }
    $refused = $false
    try { & $seal -Zip $ZipPath -Report $ReportPath -Extra $Extra -MinimumChecks 20 2>&1 | Out-Null }
    catch { $refused = $true }
    if (-not $refused) { throw "The seal accepted $Why" }
    $after = if (Test-Path $ZipPath) { (Get-FileHash $ZipPath -Algorithm SHA256).Hash } else { '' }
    if ($after -ne $before) { throw "The seal changed the distribution while refusing $Why" }
    return $Why
}

$bytes = [Text.Encoding]::UTF8.GetBytes('not really an executable, but it hashes like one')
$other = [Text.Encoding]::UTF8.GetBytes('a different build entirely')
$zip = New-Distribution 'good' $bytes
$refused = @()

$passing = @{ schema = 1; result = 'passed'; checks = 26; passed = @(1..26 | ForEach-Object { "check $_" })
              failure = ''; executable_sha256 = (Digest $bytes); commit = 'abc123'
              finished_utc = '2026-09-10T00:00:00Z' }

function Variant([hashtable]$Changes) {
    $copy = $passing.Clone()
    foreach ($key in $Changes.Keys) { $copy[$key] = $Changes[$key] }
    return $copy
}

$refused += Should-Refuse 'a failed report' $zip (New-Report 'failed' (Variant @{ result = 'failed'; failure = 'something broke' }))
$refused += Should-Refuse 'a report with no checks' $zip (New-Report 'empty' (Variant @{ checks = 0; passed = @() }))
$refused += Should-Refuse 'a report from another build' $zip (New-Report 'other' (Variant @{ executable_sha256 = (Digest $other) }))
$refused += Should-Refuse 'a report with an unknown format' $zip (New-Report 'schema' (Variant @{ schema = 99 }))
$refused += Should-Refuse 'a report that does not say what it ran against' $zip (New-Report 'nohash' (Variant @{ executable_sha256 = '' }))

$broken = Join-Path $Work 'broken.json'
'{ this is not json' | Set-Content $broken -Encoding utf8
$refused += Should-Refuse 'a report that is not JSON' $zip $broken

$refused += Should-Refuse 'a missing report' $zip (Join-Path $Work 'no-such-report.json')

$good = New-Report 'good' $passing
$refused += Should-Refuse 'a missing extra document' $zip $good @((Join-Path $Work 'no-such-doc.md'))

# And the one that has to work.
$accepted = & $seal -Zip $zip -Report $good -MinimumChecks 20 | ConvertFrom-Json
$sums = Join-Path $Work 'SHA256SUMS.txt'
if (-not (Test-Path $sums)) { throw 'A good report produced no SHA256SUMS.txt' }
if ((Get-Content $sums -Raw) -notmatch $accepted.sha256) { throw 'The published hash is not the sealed one' }
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $accepted.sha256) { throw 'The published hash is not the file' }

$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $names = $archive.Entries | Select-Object -ExpandProperty FullName
} finally { $archive.Dispose() }
if ($names -notcontains 'verification/good.json') { throw 'The report was not added to the distribution' }

Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
[pscustomobject]@{ refused = $refused; accepted = $accepted.checks } | ConvertTo-Json -Compress
