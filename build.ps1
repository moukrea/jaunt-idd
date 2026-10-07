# Build jaunt's indirect display driver and test its control protocol (Windows, PowerShell).
#   .\build.ps1 [-Platform x64|ARM64] [-Version MAJOR.MINOR.PATCH] [-Out <folder>]
# The version is a release's (from its tag); 0.0.1, the default, is a build that is not a release.
# Needs Visual Studio's C++ tools and the Windows Driver Kit (the WDK's Visual Studio extension, or
# its NuGet packages, which this script takes when the WDK is not installed). Writes one JSON line:
# what was built, the protocol tests' result, the DLL's version information, and what was missing.
# The package (<Out>\<Platform>\package: install.ps1, uninstall.ps1 and driver\ with JauntIdd.dll,
# jaunt-idd.inf and jaunt-idd.cat) is unsigned: a release signs it (release.yml), or a test
# certificate on a machine in test-signing mode.
# SPDX-License-Identifier: MIT
param([string]$Platform = "x64", [string]$Version = "0.0.1", [string]$Out = "$PSScriptRoot\out")
$ErrorActionPreference = "Stop"
if ($Platform -notin @("x64", "ARM64")) { throw "Platform is x64 or ARM64, not '$Platform'." }
# Not 0.0.0: Windows refuses a driver version of 0.0.0.0 (the INF's DriverVer).
if ($Version -notmatch '^[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}$' -or $Version -eq "0.0.0") { throw "Version is MAJOR.MINOR.PATCH other than 0.0.0, not '$Version'." }
$result = [ordered]@{ platform = $Platform; version = $Version; protocolTests = $null; driver = $null; missing = @() }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
# Absolute: MSBuild takes a relative OutDir from the project's folder, not from here.
$Out = (Resolve-Path $Out).Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) { $result.missing += "Visual Studio C++ tools" }

# The protocol on its own: cl from Visual Studio's developer environment.
if ($vs) {
    $devcmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
    $test = Join-Path $Out "protocol_test.exe"
    $cmd = "`"$devcmd`" -arch=x64 -no_logo && cl /nologo /std:c++17 /EHsc /W4 /I`"$PSScriptRoot\src`" `"$PSScriptRoot\tests\protocol_test.cpp`" `"$PSScriptRoot\src\protocol.cpp`" /Fe`"$test`" /Fo`"$Out\\`""
    cmd /c $cmd | Out-File -Encoding utf8 "$Out\protocol-build.log"
    if (Test-Path $test) {
        & $test | Out-File -Encoding utf8 "$Out\protocol-test.log"
        $result.protocolTests = if ($LASTEXITCODE -eq 0) { "passed" } else { "failed" }
    } else { $result.protocolTests = "not built (see protocol-build.log)" }
}

# The driver: MSBuild with the WDK's toolset (WindowsUserModeDriver10.0). Without the WDK installed,
# its NuGet packages (the WDK and the SDK it needs), which jaunt-idd.vcxproj imports from packages\.
$wdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include\*\um\iddcx" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
$result.wdk = if ($wdk) { "installed" } else { $null }
if (-not $wdk) {
    $nuget = (Get-Command nuget -ErrorAction SilentlyContinue).Source
    if (-not $nuget) {
        $nuget = Join-Path $Out "nuget.exe"
        Invoke-WebRequest "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $nuget
    }
    $arch = if ($Platform -eq "ARM64") { "arm64" } else { "x64" }
    foreach ($id in @("Microsoft.Windows.SDK.CPP", "Microsoft.Windows.SDK.CPP.$arch", "Microsoft.Windows.WDK.$arch")) {
        & $nuget install $id -ExcludeVersion -OutputDirectory "$PSScriptRoot\packages" -NonInteractive 2>&1 | Out-File -Append -Encoding utf8 "$Out\nuget.log"
    }
    if (Test-Path "$PSScriptRoot\packages\Microsoft.Windows.WDK.$arch") { $result.wdk = "NuGet" } else { $result.missing += "Windows Driver Kit (installed or NuGet)" }
    $result.imports = @(Get-ChildItem "$PSScriptRoot\packages\*\build\native" -Include *.props, *.targets -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
}
if ($vs) {
    $msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"
    # Unsigned (SignMode=Off): not with the WDK's test certificate of this machine. A release is
    # signed through SignPath; a test machine signs with its own test certificate.
    & $msbuild "$PSScriptRoot\jaunt-idd.vcxproj" /nologo /p:Configuration=Release /p:Platform=$Platform /p:OutDir="$Out\$Platform\\" /p:SignMode=Off /p:JauntIddVersion=$Version /v:minimal 2>&1 |
        Out-File -Encoding utf8 "$Out\driver-build.log"
    # The driver package MSBuild makes: the DLL, the INF stamped with its version, and the catalog.
    $driver = Join-Path "$Out\$Platform" "jaunt-idd"
    $dll = Get-ChildItem $driver -Filter JauntIdd.dll -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dll) {
        $result.driver = "built ($($dll.Length) bytes)"
        $info = $dll.VersionInfo
        $result.versionInfo = [ordered]@{ productName = $info.ProductName; productVersion = $info.ProductVersion; fileVersion = $info.FileVersion;
                                          companyName = $info.CompanyName; fileDescription = $info.FileDescription; originalFilename = $info.OriginalFilename }
        # What a release ships: the install scripts beside the driver package.
        $package = Join-Path "$Out\$Platform" "package"
        if (Test-Path $package) { Remove-Item -Recurse -Force $package }
        New-Item -ItemType Directory -Force "$package\driver", "$package\setup" | Out-Null
        Copy-Item "$driver\JauntIdd.dll", "$driver\jaunt-idd.inf", "$driver\jaunt-idd.cat" "$package\driver\"
        Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\uninstall.ps1", "$PSScriptRoot\README.md", "$PSScriptRoot\LICENSE", "$PSScriptRoot\NOTICE.md" $package
        Copy-Item "$PSScriptRoot\setup\JauntIddSetup.cs" "$package\setup\"
        $result.package = @(Get-ChildItem $package -File -Recurse | ForEach-Object { $_.FullName.Substring($package.Length + 1) })
    } else {
        $result.driver = "not built: " + ((Get-Content "$Out\driver-build.log" | Select-String -Pattern ": (fatal )?error " | Select-Object -First 3) -join " | ")
    }
}
$result | ConvertTo-Json -Compress
