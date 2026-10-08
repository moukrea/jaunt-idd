# Remove jaunt's indirect display driver from this computer, as an administrator: its devices, its
# driver package in Windows' driver store, its files in Program Files, its Settings > Apps entry, and
# the certificate made on this computer to sign it (install.ps1 -SignLocally), if one was.
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-Yes] [-Result <file>]
# It says what it will remove and asks first; -Yes is for a program that has already shown that and
# asked. Settings > Apps > "jaunt indirect display driver" runs this too. -Result: a JSON report.
# Exit codes: 0 removed (or nothing to remove), 1 failed, 2 declined, 4 not an administrator.
# SPDX-License-Identifier: MIT
param([switch]$Yes, [string]$Result = "")
# This PowerShell's own modules first: started from PowerShell 7, Windows PowerShell inherits its
# PSModulePath and finds none of its script-defined commands (Get-FileHash, Expand-Archive).
$env:PSModulePath = "$PSHOME\Modules;$env:PSModulePath"
$ErrorActionPreference = "Stop"
$report = [ordered]@{ action = "uninstall"; removed = $false; devices = @(); driverPackages = @(); certificates = @(); error = $null }

function Finish([int]$code, [string]$message) {
    if ($message) { $report.error = $message; Write-Host $message }
    if ($Result) { $report | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 -Path $Result }
    exit $code
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Finish 4 "Run this as an administrator." }
$target = Join-Path $env:ProgramFiles "jaunt-idd"
$appsKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\jaunt-idd"
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot "setup\JauntIddSetup.cs"))

# What there is: devices with the driver's hardware id, and driver packages that are this driver's
# (published as oem<n>.inf: its catalog's name and its hardware id).
$devices = @([JauntIddSetup]::FindDevices())
$packages = @(Get-ChildItem (Join-Path $env:SystemRoot "INF\oem*.inf") -ErrorAction SilentlyContinue | Where-Object {
    $text = [string](Get-Content -Raw $_.FullName -ErrorAction SilentlyContinue)
    $text -match '(?im)^\s*CatalogFile\s*=\s*jaunt-idd\.cat\s*$' -and $text -match '(?i)Root\\JauntIdd'
} | ForEach-Object { $_.Name })
$folder = Test-Path $target
$entry = Test-Path $appsKey
# The certificate install.ps1 -SignLocally made here: the one it recorded, and any other it made on
# this computer (self-signed, its exact name), in the trusted stores (and its key, if one is left).
$recorded = $null
try { $recorded = (Get-ItemProperty $appsKey -ErrorAction Stop).LocalCertificate } catch { }
$localSubject = "CN=jaunt indirect display driver ($env:COMPUTERNAME)"
$certificates = @(foreach ($name in "Root", "TrustedPublisher", "My") {
    Get-ChildItem "Cert:\LocalMachine\$name" -ErrorAction SilentlyContinue |
        Where-Object { ($recorded -and $_.Thumbprint -eq $recorded) -or ($_.Subject -eq $localSubject -and $_.Issuer -eq $localSubject) } |
        ForEach-Object { [ordered]@{ store = $name; thumbprint = $_.Thumbprint } }
})
if (-not $devices.Count -and -not $packages.Count -and -not $folder -and -not $entry -and -not $certificates.Count) {
    $report.removed = $true
    Write-Host "jaunt's indirect display driver is not installed here."
    Finish 0 ""
}

Write-Host "jaunt's indirect display driver is about to be removed from this computer:"
if ($devices.Count) { Write-Host "  - its device ($($devices -join ', ')); any monitor it shows goes at once;" }
if ($packages.Count) { Write-Host "  - its driver package in Windows' driver store ($($packages -join ', '));" }
if ($folder) { Write-Host "  - its files in $target;" }
if ($entry) { Write-Host "  - its entry in Settings > Apps;" }
if ($certificates.Count) {
    $stores = (@($certificates | ForEach-Object { $_.store }) | Select-Object -Unique) -join ", "
    Write-Host "  - the certificate made on this computer to sign it, from the stores $stores: this computer no longer trusts it."
}
if (-not $Yes) {
    $answer = Read-Host "Remove it? [y/N]"
    if ($answer -notmatch '^\s*(y|yes)\s*$') { Finish 2 "Nothing was removed." }
}

try {
    foreach ($d in $devices) { [JauntIddSetup]::Remove($d); $report.devices += $d }
    foreach ($p in $packages) { [JauntIddSetup]::RemoveDriverPackage($p); $report.driverPackages += $p }
    if ($entry) { Remove-Item -Recurse -Force $appsKey }
    foreach ($c in $certificates) {
        $path = "Cert:\LocalMachine\$($c.store)\$($c.thumbprint)"
        if ($c.store -eq "My") { Remove-Item -Path $path -DeleteKey } else { Remove-Item -Path $path }
        $report.certificates += "$($c.store) $($c.thumbprint)"
    }
    # This script may be the copy in that folder: PowerShell has read it already.
    if ($folder) { Remove-Item -Recurse -Force $target }
} catch {
    Finish 1 "Not fully removed: $($_.Exception.Message). Run this again, or remove ""jaunt virtual display"" in Device Manager."
}
$report.removed = $true
Write-Host "Removed."
Finish 0 ""
