$ErrorActionPreference = "Stop"

$env:IDF_PATH = "C:\esp\v6.0\esp-idf"
$preferredTools = "C:\Espressif\tools_idf_v6_0_0"
if (Test-Path $preferredTools) {
    $env:IDF_TOOLS_PATH = $preferredTools
}

. "$env:IDF_PATH\export.ps1"
Set-Location (Join-Path $PSScriptRoot "..")
idf.py set-target esp32c6
idf.py build

