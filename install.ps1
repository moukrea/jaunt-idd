# Install jaunt's indirect display driver on this computer (Windows 10 1903 or later), as an
# administrator, from a release's package folder (this script, setup\, and driver\ with
# JauntIdd.dll, jaunt-idd.inf and jaunt-idd.cat):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-AllowedUser <SID>] [-Yes] [-SignLocally] [-TestSigning] [-Result <file>]
# It says what it will change and asks first; -Yes is for a program that has already shown that
# and asked. It installs only a package whose catalog Windows accepts the signature of, never an
# unsigned one (the DLL, if not signed itself, is checked against the catalog by Windows as it
# installs).
# -SignLocally, for an unsigned release only: a certificate made here, for this computer, signs the
# package's catalog. It can sign code only (not other certificates), its public part is trusted in
# the computer's Trusted Root Certification Authorities and Trusted Publishers, and its private key
# is deleted before the driver is installed: nothing else can ever be signed with it. uninstall.ps1
# removes it. A release already signed (SignPath Foundation) is installed without it.
# -TestSigning also accepts a package signed with a test certificate, on a computer already in
# test-signing mode: this script never turns that mode on. -AllowedUser: the account that may ask
# the driver for monitors besides SYSTEM (default: the account running this). uninstall.ps1 removes
# everything this adds. -Result: a JSON report.
# Exit codes: 0 installed, 1 failed (nothing left half done), 2 declined, 3 not signed (nothing
# changed), 4 not an administrator, 5 not for this computer.
# SPDX-License-Identifier: MIT
param([string]$AllowedUser = "", [switch]$Yes, [switch]$SignLocally, [switch]$TestSigning, [string]$Result = "")
# This PowerShell's own modules first: started from PowerShell 7, Windows PowerShell inherits its
# PSModulePath and finds none of its script-defined commands (Get-FileHash, Expand-Archive).
$env:PSModulePath = "$PSHOME\Modules;$env:PSModulePath"
$ErrorActionPreference = "Stop"
$report = [ordered]@{ action = "install"; installed = $false; version = $null; signer = $null; signedLocally = $false; localCertificate = $null
            allowedUser = $null; devices = @(); rebootRequired = $false; pipe = $false; error = $null }

function Finish([int]$code, [string]$message) {
    if ($message) { $report.error = $message; Write-Host $message }
    if ($Result) { $report | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 -Path $Result }
    exit $code
}

$here = $PSScriptRoot
$target = Join-Path $env:ProgramFiles "jaunt-idd"
$appsKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\jaunt-idd"
$files = @("install.ps1", "uninstall.ps1", "setup\JauntIddSetup.cs", "driver\JauntIdd.dll", "driver\jaunt-idd.inf", "driver\jaunt-idd.cat")

# ---- what is checked before anything changes ---------------------------------------------------
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 18362) { Finish 5 "Windows 10 version 1903 (build 18362) or later is needed; this is build $build." }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Finish 4 "Run this as an administrator." }
foreach ($f in $files) { if (-not (Test-Path (Join-Path $here $f))) { Finish 1 "$f is missing from $here." } }

# The package's architecture (its DLL's) is this computer's.
$os = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment").PROCESSOR_ARCHITECTURE
$bytes = [IO.File]::ReadAllBytes((Join-Path $here "driver\JauntIdd.dll"))
$machine = [BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 0x3c) + 4)
$packageArch = switch ($machine) { 0x8664 { "AMD64" } 0xAA64 { "ARM64" } default { "unknown" } }
if ($packageArch -ne $os) { Finish 5 "This package is for $packageArch and this computer is ${os}; take the release's other package." }

# Signed: Windows accepts the catalog's signature, and the DLL is either signed too (and accepted)
# or not signed itself (Windows checks it against the catalog as it installs).
function Signatures([string]$folder) {
    @("driver\jaunt-idd.cat", "driver\JauntIdd.dll") | ForEach-Object { Get-AuthenticodeSignature (Join-Path $folder $_) }
}
$testMode = [string](Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control").SystemStartOptions -match "TESTSIGNING"
function Refused($signatures) {
    $catalog, $dll = $signatures[0], $signatures[1]
    # A catalog not signed: never. Signed with a test certificate: only with -TestSigning, in
    # test-signing mode. A DLL signed itself: accepted, or a test signature as for the catalog.
    $loose = $TestSigning -and $testMode
    $bad = @()
    if ($catalog.Status -ne "Valid" -and ($catalog.Status -eq "NotSigned" -or -not $loose)) { $bad += $catalog }
    if ($dll.Status -ne "Valid" -and $dll.Status -ne "NotSigned" -and -not $loose) { $bad += $dll }
    if (-not $bad.Count) { return $null }
    $what = ($bad | ForEach-Object { "$(Split-Path -Leaf $_.Path): $($_.Status)" }) -join ", "
    $why = ""
    if ($TestSigning -and $catalog.Status -eq "NotSigned") { $why = " -TestSigning accepts a package signed with a test certificate, not an unsigned one." }
    elseif ($TestSigning) { $why = " This computer is not in test-signing mode, and this script does not turn it on." }
    elseif ($catalog.Status -eq "NotSigned") { $why = " An unsigned release is installed with -SignLocally." }
    return "Not installed: this package's signature is not one Windows accepts ($what).$why Signed releases: https://github.com/moukrea/jaunt-idd/releases"
}
$signatures = Signatures $here
# -SignLocally: only for a catalog not signed at all (an unsigned release); one signed by someone
# Windows does not accept is never signed again here, and one Windows accepts needs nothing.
$signLocal = $false
if ($SignLocally -and $signatures[0].Status -ne "Valid") {
    if ($signatures[0].Status -ne "NotSigned") { Finish 3 "Not installed: jaunt-idd.cat is signed ($($signatures[0].Status)) by $($signatures[0].SignerCertificate.Subject), not by someone Windows accepts; -SignLocally signs an unsigned release only." }
    if ($signatures[1].Status -ne "NotSigned" -and $signatures[1].Status -ne "Valid") { Finish 3 "Not installed: JauntIdd.dll's signature is $($signatures[1].Status)." }
    $signLocal = $true
} else {
    $refusal = Refused $signatures
    if ($refusal) { Finish 3 $refusal }
}
$signer = $signatures[0].SignerCertificate
$report.signer = if ($signer -and -not $signLocal) { $signer.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { $null }
$localSubject = "CN=jaunt indirect display driver ($env:COMPUTERNAME)"

$driverVer = (Select-String -Path (Join-Path $here "driver\jaunt-idd.inf") -Pattern '^\s*DriverVer\s*=\s*(.+)$' | Select-Object -First 1).Matches[0].Groups[1].Value.Trim()
$report.version = ($driverVer -split ",")[-1].Trim()

if (-not $AllowedUser) { $AllowedUser = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
if ($AllowedUser -notmatch '^S-1-[0-9]+(-[0-9]+)+$') { Finish 1 "-AllowedUser is a SID (S-1-5-21-...), not '$AllowedUser'." }
$report.allowedUser = $AllowedUser
$account = $AllowedUser
try { $account = (New-Object Security.Principal.SecurityIdentifier($AllowedUser)).Translate([Security.Principal.NTAccount]).Value + " ($AllowedUser)" } catch { }

# ---- what changes, said before it does ----------------------------------------------------------
$publisher = if ($report.signer) { $report.signer } else { "its publisher" }
Write-Host @"
jaunt's indirect display driver $($report.version) is about to be installed on this computer. This changes the system:
  - its driver package goes into Windows' driver store, and a device "jaunt virtual display" is
    added under Display adapters;
  - only SYSTEM and $account may ask it for virtual monitors;
  - its files are kept in $target, with an entry in Settings > Apps to remove it.
It shows no monitor until that account's program asks for one, and sends nothing over the network.
"@
if ($signLocal) {
    Write-Host @"
This release is not signed. To install it, this computer is made to trust a certificate made here
for it alone:
  - a new certificate, "$localSubject", valid 2 years, that can sign code only (not other
    certificates), signs this package's catalog;
  - it is added to this computer's Trusted Root Certification Authorities and Trusted Publishers,
    so Windows accepts the driver without asking;
  - its private key is deleted right after signing, before the driver is installed: nothing else
    can ever be signed with it.
"@
} else {
    Write-Host "Windows may ask whether to install device software from `"$publisher`": that is this driver."
}
Write-Host "uninstall.ps1 (or Settings > Apps) removes all of it$(if ($signLocal) { ', that certificate included' })."
if (-not $Yes) {
    $answer = Read-Host "Install it? [y/N]"
    if ($answer -notmatch '^\s*(y|yes)\s*$') { Finish 2 "Nothing was installed." }
}

# ---- the changes ---------------------------------------------------------------------------------
$created = @()
$stage = "$target.new"
$trusted = @()      # the stores the local certificate was added to, for a rollback
$made = $null       # the local certificate while its key exists
$madeThumb = $null  # its thumbprint, from the moment it exists
try {
    # A copy only administrators can change (in Program Files), checked again there, which the
    # driver is installed from; it replaces the installed copy, uninstall.ps1 with it, only once the
    # driver is installed.
    $source = $target
    if ($here.TrimEnd("\") -ne $target.TrimEnd("\")) {
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        New-Item -ItemType Directory -Force "$stage\setup", "$stage\driver" | Out-Null
        foreach ($f in $files + @("README.md", "LICENSE", "NOTICE.md")) {
            if (Test-Path (Join-Path $here $f)) { Copy-Item (Join-Path $here $f) (Join-Path $stage $f) }
        }
        if ($signLocal) {
            # The certificate: code signing only, not a CA, its key in the computer's store, not
            # exportable.
            $made = New-SelfSignedCertificate -Type CodeSigningCert -Subject $localSubject -CertStoreLocation "Cert:\LocalMachine\My" `
                -KeyExportPolicy NonExportable -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
                -NotAfter (Get-Date).AddYears(2) -TextExtension @("2.5.29.19={critical}{text}ca=0")
            $madeThumb = $made.Thumbprint
            $usages = @($made.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } |
                ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
            $constraints = @($made.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] })
            if (($usages -join ",") -ne "1.3.6.1.5.5.7.3.3" -or $constraints.Count -ne 1 -or $constraints[0].CertificateAuthority) {
                throw "the certificate made is not code signing only (usages $($usages -join ','), CA $(if ($constraints.Count) { $constraints[0].CertificateAuthority } else { 'unsaid' }))"
            }
            # Trusted: its public part only.
            $public = New-Object Security.Cryptography.X509Certificates.X509Certificate2(, $made.RawData)
            foreach ($name in "Root", "TrustedPublisher") {
                $store = New-Object Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
                $store.Open("ReadWrite")
                $store.Add($public)
                $store.Close()
                $trusted += $name
            }
            # The catalog signed with it (the DLL, not signed itself, is in the catalog).
            $signed = Set-AuthenticodeSignature -FilePath (Join-Path $stage "driver\jaunt-idd.cat") -Certificate $made -HashAlgorithm SHA256
            if ($signed.Status -ne "Valid" -or $signed.SignerCertificate.Thumbprint -ne $madeThumb) { throw "the catalog was not signed ($($signed.Status): $($signed.StatusMessage))" }
            # Its key deleted, before anything is installed: nothing else can be signed with it.
            Remove-Item -Path "Cert:\LocalMachine\My\$madeThumb" -DeleteKey
            $made = $null
            if (Test-Path "Cert:\LocalMachine\My\$madeThumb") { throw "the certificate's private key was not deleted" }
            $report.signedLocally = $true
            $report.localCertificate = $madeThumb
        }
        $refusal = Refused (Signatures $stage)
        if ($refusal) { throw $refusal }
        $source = $stage
    }
    Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $source "setup\JauntIddSetup.cs"))

    $devices = @([JauntIddSetup]::FindDevices())
    if (-not $devices.Count) {
        $devices = @([JauntIddSetup]::CreateDevice())
        $created = $devices
    }
    foreach ($d in $devices) { [JauntIddSetup]::SetAllowedUser($d, $AllowedUser) }
    $report.rebootRequired = [JauntIddSetup]::InstallDriver((Join-Path $source "driver\jaunt-idd.inf"))
    $report.devices = $devices
} catch {
    $message = $_.Exception.Message
    foreach ($d in $created) { try { [JauntIddSetup]::Remove($d) } catch { } }
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
    # The local certificate, its key and its trust gone with it.
    if ($madeThumb) {
        if (Test-Path "Cert:\LocalMachine\My\$madeThumb") { Remove-Item -Path "Cert:\LocalMachine\My\$madeThumb" -DeleteKey -ErrorAction SilentlyContinue }
        foreach ($name in $trusted) {
            $store = New-Object Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
            $store.Open("ReadWrite")
            foreach ($found in @($store.Certificates.Find("FindByThumbprint", $madeThumb, $false))) { $store.Remove($found) }
            $store.Close()
        }
    }
    $report.signedLocally = $false
    $report.localCertificate = $null
    Finish $(if ($message -like "Not installed:*") { 3 } else { 1 }) "$($message.TrimEnd('.')). Nothing was left half installed."
}
if ($source -eq $stage) {
    try {
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        Move-Item $stage $target
    } catch {
        Write-Host "The driver is installed, but its files stayed in $stage ($($_.Exception.Message)): remove it with $stage\uninstall.ps1."
    }
}
# A device that was there reads AllowedUser again when it starts. The driver is installed by now:
# a failure here is said, not undone.
if (-not $created.Count) {
    foreach ($d in $devices) {
        try { [JauntIddSetup]::Restart($d) } catch { $report.rebootRequired = $true; Write-Host "The device did not restart ($($_.Exception.Message)): restart Windows for the account allowed to apply." }
    }
}

# Settings > Apps: the way to remove it.
# The local certificate of an earlier installation, no longer needed once this one is in.
$earlier = $null
try { $earlier = (Get-ItemProperty $appsKey -ErrorAction Stop).LocalCertificate } catch { }
try {
    New-Item -Force $appsKey | Out-Null
    $powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    $values = [ordered]@{ DisplayName = "jaunt indirect display driver"; DisplayVersion = $report.version; Publisher = "jaunt"; InstallLocation = $target
                          URLInfoAbout = "https://github.com/moukrea/jaunt-idd"
                          UninstallString = "`"$powershell`" -NoProfile -ExecutionPolicy Bypass -File `"$target\uninstall.ps1`"" }
    foreach ($name in $values.Keys) { New-ItemProperty -Force -Path $appsKey -Name $name -Value $values[$name] -PropertyType String | Out-Null }
    foreach ($name in "NoModify", "NoRepair") { New-ItemProperty -Force -Path $appsKey -Name $name -Value 1 -PropertyType DWord | Out-Null }
    # The certificate made here for this driver: uninstall.ps1 removes it from the trusted stores.
    if ($report.localCertificate) { New-ItemProperty -Force -Path $appsKey -Name LocalCertificate -Value $report.localCertificate -PropertyType String | Out-Null }
    elseif ($earlier) { Remove-ItemProperty -Path $appsKey -Name LocalCertificate -ErrorAction SilentlyContinue }
} catch {
    Write-Host "The entry in Settings > Apps was not written ($($_.Exception.Message)): remove the driver with $target\uninstall.ps1."
}

if ($earlier -and $earlier -ne $report.localCertificate) {
    foreach ($name in "Root", "TrustedPublisher") {
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
        $store.Open("ReadWrite")
        foreach ($found in @($store.Certificates.Find("FindByThumbprint", $earlier, $false))) { $store.Remove($found) }
        $store.Close()
    }
}

# The driver's pipe, once Windows has started it.
$deadline = (Get-Date).AddSeconds(20)
do {
    try { $report.pipe = [bool](@([IO.Directory]::GetFiles("\\.\pipe\")) -contains "\\.\pipe\jaunt-idd") } catch { }
    if ($report.pipe -or (Get-Date) -gt $deadline) { break }
    Start-Sleep -Milliseconds 500
} while ($true)
$report.installed = $true
if ($report.rebootRequired) { Write-Host "Installed. Windows needs a restart before the driver runs." }
elseif ($report.pipe) { Write-Host "Installed: the driver is running." }
else { Write-Host "Installed, but the driver has not started yet: see Device Manager > Display adapters > jaunt virtual display." }
Finish 0 ""
