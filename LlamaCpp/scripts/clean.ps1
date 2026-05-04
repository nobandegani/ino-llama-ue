# clean.ps1
#
# Wipes the download cache, build cache, staged headers, and staged
# binaries for the llama.cpp integration. Safe to run anytime — re-run
# setup-llamacpp.ps1 afterwards to restore everything.
#
# Does NOT touch the LlamaCpp/vendor/llama.cpp/ submodule (that's source,
# not generated). Use git submodule commands to manage the submodule.

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$LlamaCppDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir   = (Resolve-Path (Join-Path $LlamaCppDir "..")).Path

$Targets = @(
    (Join-Path $LlamaCppDir ".cache"),
    (Join-Path $PluginDir   "Source\ThirdParty\Public"),
    (Join-Path $PluginDir   "Source\ThirdParty\Win64"),
    (Join-Path $PluginDir   "Source\ThirdParty\Android"),
    (Join-Path $PluginDir   "Source\ThirdParty\.llamacpp_version")
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
Write-Host "Note: vendor/llama.cpp submodule is NOT touched (source, not generated)." -ForegroundColor DarkGray
Write-Host "Clean complete. Run setup-llamacpp.ps1 to restage." -ForegroundColor Green
