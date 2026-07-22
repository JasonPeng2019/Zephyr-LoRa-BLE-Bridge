param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-final-hil')
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$NcsRoot = if ($env:NCS_ROOT) { $env:NCS_ROOT } else { 'C:\ncs\v3.3.1' }
$ToolchainRoot = if ($env:NCS_TOOLCHAIN_ROOT) { $env:NCS_TOOLCHAIN_ROOT } else { 'C:\ncs\toolchains\936afb6332' }
$West = Join-Path $ToolchainRoot 'opt\bin\Scripts\west.exe'

if (-not (Test-Path $West)) {
    throw "west was not found at $West; set NCS_TOOLCHAIN_ROOT to the installed toolchain root."
}

& (Join-Path $PSScriptRoot 'fetch_lora_basics_modem.ps1')
$env:ZEPHYR_BASE = Join-Path $NcsRoot 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = Join-Path $ToolchainRoot 'opt'
$env:ZEPHYR_EXTRA_MODULES = Join-Path $RepoRoot 'modules\lora-basics-modem'
$env:PATH = (Join-Path $ToolchainRoot 'opt\bin') + ';' +
            (Join-Path $ToolchainRoot 'opt\bin\Scripts') + ';' + $env:PATH

$ResolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
& $West build -b nrf52840dk/nrf52840 -d $ResolvedBuildDir --pristine=always -- `
    '-DCONF_FILE=prj.conf' `
    '-DDTC_OVERLAY_FILE=boards/nrf52840dk_nrf52840.overlay'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Output "build=$ResolvedBuildDir"
Write-Output "module=$env:ZEPHYR_EXTRA_MODULES"
