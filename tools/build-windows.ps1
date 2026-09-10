param([int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build-cocompose'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
Invoke-Checked cmake @('-S', "$root/examples/CoCompose", '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-Checked cmake @('--build', $build, '--config', 'Release', '--target', 'CoCompose', '--parallel', "$Jobs")

# What went into this build, written before packaging so it travels inside the ZIP.
# A build from a dirty tree says so rather than claiming the commit it sat on.
$exe = Join-Path $build 'CoCompose_artefacts/Release/CoCompose.exe'
if (-not (Test-Path $exe)) { throw "No executable at $exe" }

$commit = (& git -C $root rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0) { $commit = 'unknown' }
$dirty = (& git -C $root status --porcelain 2>$null)
$tree = 'clean'
if (-not [string]::IsNullOrWhiteSpace($dirty)) { $tree = 'modified' }
$version = (Select-String -Path "$root/examples/CoCompose/CMakeLists.txt" -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value

[pscustomobject]@{
    product        = 'CoCompose'
    version        = $version
    commit         = $commit
    tree           = $tree
    built_utc      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    executable     = 'CoCompose.exe'
    sha256         = (Get-FileHash $exe -Algorithm SHA256).Hash
    size_bytes     = (Get-Item $exe).Length
} | ConvertTo-Json | Set-Content (Join-Path $build 'BUILD-INFO.json') -Encoding utf8

# CPack stages into a fresh directory, so old distribution files cannot leak in.
Invoke-Checked cpack @('--config', "$build/CPackConfig.cmake", '-C', 'Release', '-B', "$build/dist")
Write-Host "Ready: $build/dist/CoCompose-$version-windows-x64.zip"
