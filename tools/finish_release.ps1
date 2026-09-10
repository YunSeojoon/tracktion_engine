# Folds the check report into the distribution and writes the hash to publish next to
# it. This is the last gate, not a formality: it reads the report, insists it says the
# checks passed, and insists the binary those checks ran against is the binary in this
# ZIP. A report from another build, an older run, or a failed run is refused.
#
# Usage: .\tools\finish_release.ps1 -Zip <path> -Report <test-report.json> [-Extra <file>...]
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Report,
    [string[]]$Extra = @(),
    [int]$MinimumChecks = 20
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $Zip))    { throw "No distribution at $Zip" }
if (-not (Test-Path $Report)) { throw "No check report at $Report - the checks have to run first" }

# Missing extras are a mistake in the caller, not something to pass over quietly.
foreach ($file in $Extra) {
    if (-not (Test-Path $file)) { throw "Cannot add $file to the distribution: it does not exist" }
}

try { $checks = Get-Content $Report -Raw | ConvertFrom-Json }
catch { throw "The check report is not JSON: $Report" }

if ($null -eq $checks.schema -or [int]$checks.schema -ne 1) {
    throw "Unknown check report format (schema '$($checks.schema)') in $Report"
}
if ($checks.result -ne 'passed') {
    throw "The checks did not pass (result '$($checks.result)'): $($checks.failure)"
}
if ([int]$checks.checks -lt $MinimumChecks) {
    throw "Only $($checks.checks) checks are recorded; at least $MinimumChecks were expected"
}
if ([string]::IsNullOrWhiteSpace($checks.executable_sha256)) {
    throw "The check report does not say which binary it ran against"
}

# The binary in the ZIP has to be the one the checks were run against. Without this a
# report from any build at all would seal any distribution at all.
$archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $Zip))
try {
    $entry = $archive.Entries | Where-Object { $_.FullName -eq 'CoCompose.exe' } | Select-Object -First 1
    if (-not $entry) { throw "No CoCompose.exe in $Zip" }

    $stream = $entry.Open()
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        $packaged = ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('X2') }) -join ''
    }
    finally { $stream.Dispose() }
}
finally { $archive.Dispose() }

if ($packaged -ne $checks.executable_sha256.ToUpper()) {
    throw ("The checks ran against a different build. " +
           "Report: $($checks.executable_sha256). In this ZIP: $packaged")
}

$archive = [IO.Compression.ZipFile]::Open((Resolve-Path $Zip), 'Update')
try {
    foreach ($file in @($Report) + $Extra) {
        $name = 'verification/' + (Split-Path $file -Leaf)
        $existing = $archive.GetEntry($name)
        if ($existing) { $existing.Delete() }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Resolve-Path $file), $name)
    }
}
finally { $archive.Dispose() }

# The hash is taken after the report went in, so it is the hash of what people download.
$hash = (Get-FileHash $Zip -Algorithm SHA256).Hash
$sums = Join-Path (Split-Path (Resolve-Path $Zip)) 'SHA256SUMS.txt'
"$hash  $(Split-Path $Zip -Leaf)" | Set-Content $sums -Encoding ascii

[pscustomobject]@{
    zip        = Split-Path $Zip -Leaf
    sha256     = $hash
    sums       = $sums
    checks     = [int]$checks.checks
    commit     = $checks.commit
    executable = $packaged
    added      = (@($Report) + $Extra | ForEach-Object { Split-Path $_ -Leaf })
} | ConvertTo-Json -Compress
