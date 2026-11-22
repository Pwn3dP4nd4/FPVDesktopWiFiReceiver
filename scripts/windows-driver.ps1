# FPVDesktopWiFiReceiver - driver WinUSB para el adaptador RTL8812BU (sustituye a Zadig). Windows 10/11.
#
#   Ejecutar como administrador (PowerShell -> "Ejecutar como administrador"):
#     (o desde la app: Archivo > Instalar driver del adaptador)
#     powershell -ExecutionPolicy Bypass -File scripts\windows-driver.ps1                 instalar
#     powershell -ExecutionPolicy Bypass -File scripts\windows-driver.ps1 -RemoveZadig    instalar y borrar las
#                                                                                          copias que dejo Zadig
#     powershell -ExecutionPolicy Bypass -File scripts\windows-driver.ps1 -Status         ver que driver tiene
#     powershell -ExecutionPolicy Bypass -File scripts\windows-driver.ps1 -Uninstall      quitarlo todo
#
# Que hace al instalar:
#   1. Crea un certificado de firma de codigo local ("FPVDesktopWiFiReceiver local driver signing").
#   2. Firma con el el catalogo del INF (packaging\windows\driver) y BORRA la clave privada: el certificado ya no
#      puede firmar nada mas.
#   3. Anade el certificado publico a "Entidades de certificacion raiz de confianza" y "Editores de confianza"
#      del equipo (Windows solo instala drivers firmados por alguien de confianza). Es lo mismo que hace Zadig.
#   4. Instala el driver con pnputil. Windows lo preferira al driver Realtek en cada enchufado (ver el INF).
# -Uninstall quita el driver y el certificado. El adaptador vuelve a su driver Realtek al reenchufarlo.
#   -InfSha256 <hex>: el SHA-256 que tiene que tener el INF (la app lo pasa: el de su version); si no es ese, no
#                     se instala nada
param([switch]$Uninstall, [switch]$Status, [switch]$RemoveZadig, [string]$InfSha256)
$ErrorActionPreference = 'Stop'
$CertSubject = 'CN=FPVDesktopWiFiReceiver local driver signing'
$InfName = 'fpvdesktopwifireceiver_winusb.inf'
# Junto al script (carpeta driver\ de la version portable) o en el repositorio
$InfSrc = @((Join-Path $PSScriptRoot $InfName), (Join-Path $PSScriptRoot "..\packaging\windows\driver\$InfName")) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
# BrosTrend AC1200 y otros RTL8812BU, TP-Link Archer T4U v3 / T3U / T3U Plus
$DevPattern = 'USB\\VID_0BDA&PID_B812\\|USB\\VID_2357&PID_(0115|012D|0138)\\'

function Show-Adapter {
    $devs = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match $DevPattern }
    if (-not $devs) { Write-Host 'Adaptador: no conectado.'; return }
    foreach ($d in $devs) {
        $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service).Data
        $inf = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
        $ok = if ($svc -eq 'WinUSB') { 'OK (WinUSB)' } else { "driver $svc ($inf): la app no puede usarlo" }
        Write-Host ("Adaptador {0}: {1}" -f $d.FriendlyName, $ok)
    }
}

function Get-OurDrivers { Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like "*\$InfName" } }
function Get-ZadigDrivers {
    Get-WindowsDriver -Online | Where-Object { $_.ProviderName -eq 'libwdi' -and $_.OriginalFileName -like '*\802.11ac_nic.inf' }
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($Status) {
    Show-Adapter
    if ($isAdmin) {
        Write-Host ("Driver FPVDesktopWiFiReceiver instalado: {0}" -f [bool](Get-OurDrivers))
        Write-Host ("Copias de Zadig en el almacen: {0}" -f @(Get-ZadigDrivers).Count)
    }
    exit 0
}
if (-not $isAdmin) {
    Write-Error 'Hace falta ejecutarlo como administrador (PowerShell -> Ejecutar como administrador).'
}

if ($Uninstall) {
    foreach ($d in Get-OurDrivers) {
        Write-Host "Quitando driver $($d.Driver)..."
        pnputil /delete-driver $d.Driver /uninstall /force | Out-Host
    }
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | ForEach-Object {
            Write-Host "Quitando certificado de $store ($($_.Thumbprint))"
            Remove-Item $_.PSPath
        }
    }
    Write-Host 'Hecho. Desenchufa y vuelve a enchufar el adaptador.'
    exit 0
}

# ---- instalar ----
if (-not $InfSrc) { Write-Error "No encuentro $InfName junto al script ni en packaging\windows\driver" }
# Carpeta de trabajo que solo pueden tocar los administradores y SYSTEM: el INF que se firma e instala no se puede
# cambiar entre la comprobacion y la firma (el %TEMP% del usuario, y la carpeta de la app, los puede escribir el)
$work = Join-Path $env:SystemRoot ("Temp\fpvdwr-driver-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
try {
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in 'S-1-5-32-544', 'S-1-5-18') {  # Administradores, SYSTEM
        $id = New-Object System.Security.Principal.SecurityIdentifier $sid
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule $id, 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    }
    Set-Acl -Path $work -AclObject $acl
    Copy-Item $InfSrc $work
    if ($InfSha256) {
        $got = (Get-FileHash -Algorithm SHA256 (Join-Path $work $InfName)).Hash
        if ($got -ne $InfSha256) { Write-Error "El INF no es el de esta version (SHA-256 $got): no se instala." }
    }
    $cat = Join-Path $work 'fpvdesktopwifireceiver_winusb.cat'
    New-FileCatalog -Path $work -CatalogFilePath $cat -CatalogVersion 2.0 | Out-Null

    # Certificado nuevo en cada instalacion; el de una instalacion anterior se sustituye
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $CertSubject } | Remove-Item
    }
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(20)
    try {
        $sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if ($sig.Status -ne 'Valid' -and $sig.Status -ne 'UnknownError') { Write-Error "Firma del catalogo: $($sig.Status) $($sig.StatusMessage)" }
        $cer = Join-Path $work 'cert.cer'
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        foreach ($store in 'Root', 'TrustedPublisher') {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        }
    } finally {
        # La clave privada no se conserva: este certificado no podra firmar nada mas
        Remove-Item $cert.PSPath -DeleteKey -ErrorAction SilentlyContinue
        if (Test-Path $cert.PSPath) { Remove-Item $cert.PSPath }
    }
    $check = Get-AuthenticodeSignature $cat
    Write-Host "Catalogo firmado: $($check.Status)"

    foreach ($d in Get-OurDrivers) { pnputil /delete-driver $d.Driver /force | Out-Null }  # version anterior
    Write-Host 'Instalando el driver...'
    pnputil /add-driver (Join-Path $work $InfName) /install | Out-Host
    # 0 = instalado, 3010 = instalado (pide reiniciar), 259 (ERROR_NO_MORE_ITEMS) = paquete anadido pero ningun
    # dispositivo cambio de driver: o ya lo tenia, o no esta conectado, o otro driver esta mejor clasificado
    # (eso lo detecta la comprobacion de abajo). Cualquier otro codigo es un fallo de verdad.
    if ($LASTEXITCODE -notin 0, 259, 3010) { Write-Error "pnputil fallo (codigo $LASTEXITCODE)" }
    $notWinUsb = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match $DevPattern } | Where-Object {
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB' }
    foreach ($d in $notWinUsb) {
        Write-Host "Otro driver le gana al nuestro en $($d.InstanceId). Candidatos y clasificacion (menor gana):"
        pnputil /enum-devices /instanceid $d.InstanceId /drivers | Out-Host
    }
    if ($notWinUsb) { Write-Error 'El adaptador no quedo en WinUSB (ver la lista de drivers de arriba).' }

    if ($RemoveZadig) {
        foreach ($d in Get-ZadigDrivers) {
            Write-Host "Quitando copia de Zadig $($d.Driver)"
            pnputil /delete-driver $d.Driver /force | Out-Null
        }
    }
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
Show-Adapter
Write-Host 'Si el adaptador no aparece como OK (WinUSB), desenchufalo y vuelve a enchufarlo.'
