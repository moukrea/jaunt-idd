# What a CI runner can check of install.ps1 and uninstall.ps1 (it cannot load the driver, which is
# unsigned there): every script parses, the setup helper compiles and lists devices, install.ps1
# refuses the unsigned package and changes nothing, also with -TestSigning (an unsigned package is
# never installed, test-signing mode or not: GitHub's Windows runner boots in that mode), and
# uninstall.ps1 finds nothing to remove (it removes what a check left behind). Run it with Windows PowerShell 5.1
# (powershell.exe), as jaunt and Settings > Apps do, as an administrator (GitHub's runners are):
#   powershell -NoProfile -NonInteractive -InputFormat None -ExecutionPolicy Bypass -File tests\scripts_test.ps1 -Package out\x64\package
# Writes one JSON line; exits 1 when a check fails.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$checks = [ordered]@{ powershell = $PSVersionTable.PSVersion.ToString() }
$script:failed = $false
function Check([string]$name, [bool]$good, $detail) {
    $checks[$name] = [ordered]@{ ok = $good; detail = $detail }
    if (-not $good) { $script:failed = $true }
}
# A script in its own Windows PowerShell, as jaunt and Settings > Apps start it: no input (with stdin
# redirected, powershell -File waits for its end), at most two minutes. Its exit code, or -1 when it
# had to be stopped.
function RunScript([string]$path, [string[]]$arguments) {
    $all = @("-NoProfile", "-NonInteractive", "-InputFormat", "None", "-ExecutionPolicy", "Bypass", "-File", $path) + $arguments
    $quoted = ($all | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " "
    $process = Start-Process -FilePath "powershell.exe" -ArgumentList $quoted -PassThru -WindowStyle Hidden
    $null = $process.Handle  # keeps the exit code readable once it ends
    if (-not $process.WaitForExit(120000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        return -1
    }
    return $process.ExitCode
}
function SystemState {
    [ordered]@{
        devices = @([JauntIddSetup]::FindDevices()).Count
        folder = Test-Path (Join-Path $env:ProgramFiles "jaunt-idd")
        appsEntry = Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\jaunt-idd"
        driverPackages = @(Get-ChildItem (Join-Path $env:SystemRoot "INF\oem*.inf") -ErrorAction SilentlyContinue |
            Where-Object { [string](Get-Content -Raw $_.FullName -ErrorAction SilentlyContinue) -match '(?im)^\s*CatalogFile\s*=\s*jaunt-idd\.cat\s*$' }).Count
        startOptions = [string](Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control").SystemStartOptions
    }
}

foreach ($f in Get-ChildItem $root -Recurse -Filter *.ps1 | Where-Object { $_.FullName -notmatch '\\(out|packages)\\' }) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    Check "parses: $($f.FullName.Substring($root.Length + 1))" ($errors.Count -eq 0) (@($errors | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }) -join "; ")
}

$helper = $false
try {
    Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $root "setup\JauntIddSetup.cs"))
    $before = SystemState
    $checks["testSigningMode"] = [bool]($before.startOptions -match "TESTSIGNING")
    $helper = $true
    Check "setup helper: compiles, lists devices" ($before.devices -eq 0) $before
} catch {
    Check "setup helper: compiles, lists devices" $false $_.Exception.Message
}

if ($helper) {
    $work = Join-Path ([IO.Path]::GetTempPath()) ("jaunt-idd-test-" + [guid]::NewGuid())
    Copy-Item -Recurse $Package $work
    # Hashtables: PowerShell would flatten @(@(), @("-TestSigning")) into one string.
    foreach ($variant in @(@{ extra = @() }, @{ extra = @("-TestSigning") })) {
        $extra = [string[]]$variant.extra
        $name = (@("install.ps1") + $extra + @("refuses the unsigned package, nothing changed")) -join " "
        $out = Join-Path $work "install-result.json"
        Remove-Item $out -ErrorAction SilentlyContinue
        $code = RunScript (Join-Path $work "install.ps1") (@("-Yes") + $extra + @("-Result", $out))
        $report = if (Test-Path $out) { Get-Content -Raw $out | ConvertFrom-Json } else { $null }
        $after = SystemState
        $same = ($after | ConvertTo-Json -Compress) -eq ($before | ConvertTo-Json -Compress)
        Check $name ($code -eq 3 -and $report -and -not $report.installed -and $same) ([ordered]@{ exitCode = $code; error = $(if ($report) { $report.error } else { $null }); after = $after })
    }
    $out = Join-Path $work "uninstall-result.json"
    $code = RunScript (Join-Path $work "uninstall.ps1") @("-Yes", "-Result", $out)
    $report = if (Test-Path $out) { Get-Content -Raw $out | ConvertFrom-Json } else { $null }
    Check "uninstall.ps1 finds nothing to remove" ($code -eq 0 -and $report -and $report.removed -and @($report.devices).Count -eq 0) ([ordered]@{ exitCode = $code; report = $report })
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

$checks["passed"] = -not $script:failed
$checks | ConvertTo-Json -Compress -Depth 5
if ($script:failed) { exit 1 }
