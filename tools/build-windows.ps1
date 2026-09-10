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
# CPack stages into a fresh directory, so old distribution files cannot leak in.
Invoke-Checked cpack @('--config', "$build/CPackConfig.cmake", '-C', 'Release', '-B', "$build/dist")
Write-Host "Ready: $build/dist/CoCompose-0.1.0-windows-x64.zip"
