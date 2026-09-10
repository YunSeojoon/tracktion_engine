# Folds the check report into the distribution and writes the hash to publish next to
# it. Run only after the checks have passed: what this produces is a ZIP that says,
# from inside, which build it is and what was run against it.
#
# Usage: .\tools\finish_release.ps1 -Zip <path> -Report <test-report.json> [-Extra <file>...]
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Report,
    [string[]]$Extra = @()
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $Zip))    { throw "No distribution at $Zip" }
if (-not (Test-Path $Report)) { throw "No check report at $Report - the checks have to run first" }

$archive = [IO.Compression.ZipFile]::Open((Resolve-Path $Zip), 'Update')
try {
    foreach ($file in @($Report) + $Extra) {
        if (-not (Test-Path $file)) { continue }
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
    zip    = Split-Path $Zip -Leaf
    sha256 = $hash
    sums   = $sums
    added  = (@($Report) + $Extra | Where-Object { Test-Path $_ } | ForEach-Object { Split-Path $_ -Leaf })
} | ConvertTo-Json -Compress
