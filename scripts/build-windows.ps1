# Compila kepyur-desktop en Windows desde PowerShell usando MSYS2 (UCRT64).
#   .\scripts\build-windows.ps1            backend real + carpeta portable dist\kepyur-desktop
#   .\scripts\build-windows.ps1 -Deps      instala antes los paquetes de MSYS2
#   .\scripts\build-windows.ps1 -Demo      solo GUI con backend simulado
param([switch]$Deps, [switch]$Demo, [string]$Msys = "C:\msys64")
$ErrorActionPreference = "Stop"
$bash = Join-Path $Msys "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    Write-Error "No encuentro MSYS2 en $Msys. Instalalo: winget install MSYS2.MSYS2  (o https://www.msys2.org)"
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$unixRoot = (& $bash -lc "cygpath -u '$root'").Trim()
$args = @()
if ($Deps) { $args += "--deps" }
if ($Demo) { $args += "--demo" }
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
& $bash -lc "cd '$unixRoot' && scripts/build.sh $($args -join ' ')"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $Demo) {
    & $bash -lc "cd '$unixRoot' && scripts/deploy-windows.sh"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "Ejecutable: $root\dist\kepyur-desktop\kepyur-desktop.exe"
}
