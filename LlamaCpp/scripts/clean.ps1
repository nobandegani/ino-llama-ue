# clean.ps1
#
# Wipes the download cache, staged headers, and staged binaries for the
# llama.cpp integration. Safe to run anytime — re-run setup-llamacpp.ps1
# afterwards to restore everything.

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$LlamaCppDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir   = (Resolve-Path (Join-Path $LlamaCppDir "..")).Path

$Targets = @(
    (Join-Path $LlamaCppDir ".cache"),
    (Join-Path $PluginDir   "Source\ThirdParty\InoLlamaCpp\Public"),
    (Join-Path $PluginDir   "Source\ThirdParty\InoLlamaCpp\.llamacpp_version"),
    (Join-Path $PluginDir   "Binaries\ThirdParty\InoLlamaCpp\Win64"),
    (Join-Path $PluginDir   "Binaries\ThirdParty\InoLlamaCpp\Android")
)

Write-Host "=== llama.cpp clean ===" -ForegroundColor Cyan
foreach ($t in $Targets) {
    if (Test-Path $t) {
        Remove-Item -Recurse -Force $t
        Write-Host "  [REMOVED] $t"
    } else {
        Write-Host "  [ABSENT]  $t"
    }
}

Write-Host ""
Write-Host "Clean complete. Run setup-llamacpp.ps1 to restage." -ForegroundColor Green
