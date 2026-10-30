# Reproduce the Windows client build locally with an actionable compiler log.
# Run in PowerShell 7 on Windows from any working directory:
#   pwsh -NoProfile -File tools/build_client.ps1
param(
    [ValidateSet("release","debug")]
    [string]$Mode = "release",
    [switch]$SkipRepoUpdate
)
$ErrorActionPreference = "Stop"
Set-Location (Resolve-Path (Join-Path $PSScriptRoot ".."))
$logPath = Join-Path (Get-Location) "build-client.log"
if (-not (Get-Command "xmake" -ErrorAction SilentlyContinue)) {
    throw "xmake is required and was not found in PATH."
}
if (-not (Get-Command "clang-cl" -ErrorAction SilentlyContinue)) {
    throw "clang-cl is required and was not found in PATH. Install LLVM 22."
}
if (-not $SkipRepoUpdate) {
    & xmake repo -u
    if ($LASTEXITCODE -ne 0) { throw "xmake repo -u failed ($LASTEXITCODE)" }
}
& xmake f -p windows -a x64 -m $Mode -y -v
if ($LASTEXITCODE -ne 0) { throw "xmake configure failed ($LASTEXITCODE)" }
Write-Host "Compiling; full diagnostic log: $logPath"
& xmake -y -v -D *>&1 | Tee-Object -FilePath $logPath
$buildExit = $LASTEXITCODE
if ($buildExit -ne 0) {
    Write-Error "Native client build failed with exit code $buildExit. See $logPath"
    exit $buildExit
}
& python tools/verify_package.py
if ($LASTEXITCODE -ne 0) { throw "Package validation failed ($LASTEXITCODE)" }
& python tools/verify_model_library.py --self-test
if ($LASTEXITCODE -ne 0) { throw "Model library validation failed ($LASTEXITCODE)" }
Write-Host "Native client build, packaging, and model-library self-test succeeded."
