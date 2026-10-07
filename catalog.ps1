# Make the driver package's catalog (jaunt-idd.cat) again over the files in a driver folder, as a
# release does once the DLL is signed (the catalog holds the signed DLL's hash).
#   .\catalog.ps1 -Driver <folder with JauntIdd.dll and jaunt-idd.inf> -Platform x64|ARM64
# Inf2Cat from the installed WDK, else from the WDK's NuGet package build.ps1 took. The Windows
# versions are those the INF installs on (10.0...18362 and later). Writes one JSON line.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory = $true)][string]$Driver, [string]$Platform = "x64")
$ErrorActionPreference = "Stop"
$arch = switch ($Platform) { "x64" { "X64" } "ARM64" { "ARM64" } default { throw "Platform is x64 or ARM64, not '$Platform'." } }
$os = (@("10_19H1", "10_VB", "10_CO", "10_NI") | ForEach-Object { "${_}_$arch" }) -join ","
$Driver = (Resolve-Path $Driver).Path
$inf2cat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$PSScriptRoot\packages" -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -like "*\x86\*" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $inf2cat) { throw "Inf2Cat.exe not found (install the WDK, or run build.ps1 first)." }
Remove-Item "$Driver\jaunt-idd.cat" -ErrorAction SilentlyContinue
# Its output in files (Windows PowerShell 5.1 would stop at its first line on stderr), no input, and
# at most ten minutes.
$logs = Join-Path ([IO.Path]::GetTempPath()) ("inf2cat-" + [guid]::NewGuid())
New-Item -ItemType File -Force "$logs.in" | Out-Null  # an empty input
$process = Start-Process -FilePath $inf2cat.FullName -ArgumentList "`"/driver:$Driver`" /os:$os /uselocaltime" -PassThru -NoNewWindow `
    -RedirectStandardOutput "$logs.out" -RedirectStandardError "$logs.err" -RedirectStandardInput "$logs.in"
$null = $process.Handle  # keeps the exit code readable once it ends
if ($process.WaitForExit(600000)) { $code = $process.ExitCode } else { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $code = "stopped after 10 minutes" }
$log = (Get-Content -Raw "$logs.out", "$logs.err" -ErrorAction SilentlyContinue) -join "`n"
Remove-Item "$logs.out", "$logs.err", "$logs.in" -ErrorAction SilentlyContinue
$made = Test-Path "$Driver\jaunt-idd.cat"
[ordered]@{ catalog = $(if ($made) { "made" } else { "not made" }); os = $os; inf2cat = $inf2cat.FullName; exitCode = $code
            log = @($log -split "`r?`n" | Where-Object { $_ -match "error|warning|complete" } | Select-Object -First 8) } | ConvertTo-Json -Compress
if (-not $made -or $code -ne 0) { exit 1 }
