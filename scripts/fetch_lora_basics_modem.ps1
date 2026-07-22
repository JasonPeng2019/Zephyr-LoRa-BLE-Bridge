param(
    [string]$ModuleRoot = (Join-Path $PSScriptRoot '..\modules\lora-basics-modem')
)

$ErrorActionPreference = 'Stop'
$Revision = 'a8ddc544043e72807cf7db532478e1dda734ae7c'
$Remote = 'https://github.com/zephyrproject-rtos/lora-basics-modem.git'
$Resolved = [System.IO.Path]::GetFullPath($ModuleRoot)

if (-not (Test-Path (Join-Path $Resolved '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Resolved) | Out-Null
    git clone --no-checkout $Remote $Resolved
}

git -C $Resolved fetch --tags --force origin $Revision
git -C $Resolved checkout --detach $Revision
$Actual = (git -C $Resolved rev-parse HEAD).Trim()
if ($Actual -ne $Revision) {
    throw "Unexpected lora-basics-modem revision: $Actual"
}
Write-Output "lora-basics-modem=$Actual path=$Resolved"
