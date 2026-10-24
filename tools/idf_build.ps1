# Builds an ESP-IDF project of this repository with the local ESP-IDF 5.5.2.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf_build.ps1 -Project firmware [-Log build.log] [-Target build]
param([string]$Project = 'firmware', [string]$Log = '', [string]$Target = 'build')
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root $Project
if (-not $Log) { $Log = Join-Path $proj 'build.log' }
if (-not $env:IDF_PATH) { $env:IDF_PATH = Join-Path $HOME 'esp\v5.5.2\esp-idf' }
. (Join-Path $env:IDF_PATH 'export.ps1') *> $null
idf.py -C $proj $Target *> $Log
$code = $LASTEXITCODE
"EXIT=$code" | Out-File -Append -FilePath $Log -Encoding utf8
exit $code
