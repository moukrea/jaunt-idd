# Load the driver on GitHub's own Windows runner, use it, and remove it: installs the unsigned package
# as people install an unsigned release, install.ps1 -SignLocally (a certificate made for this
# computer signs its catalog, is trusted, and loses its private key), checks that certificate, adds
# a monitor through the pipe and checks that it becomes an ACTIVE display of the size asked (an
# active path on the driver's adapter in QueryDisplayConfig, by itself or once the desktop is
# extended to it) and that removing it brings the topology back, adds and removes more monitors,
# checks that a silent connection and a closed one lose theirs, then runs uninstall.ps1, which
# removes the certificate too. Every step blocks: a step that fails fails the job. The driver's own
# log (its `log` request) goes into the first monitor's step and, whole, into out\load-driver.log.
# The runner boots in test-signing mode, so this does not show that the driver loads without it.
# It runs nowhere else: only where GITHUB_ACTIONS is true and RUNNER_ENVIRONMENT is github-hosted, on
# a computer already in test-signing mode (this never turns that mode on), as an administrator,
# with no jaunt-idd installed. The certificate install.ps1 makes loses its private key before the
# driver is installed; it leaves the machine's stores at the end, whatever happened.
#   powershell -NoProfile -NonInteractive -InputFormat None -ExecutionPolicy Bypass -File tests\load_test.ps1 -Package out\x64\package [-Then <script.ps1>]
# -Then: a script run with the driver installed, before it is removed (a program that uses the
# driver checks itself there); its exit code and output are a step.
# Writes one JSON line: {ran: false, why} where it does not run, else each step; exits 1 when a step
# failed (the CI job fails with it).
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory = $true)][string]$Package, [string]$Then = "")
$env:PSModulePath = "$PSHOME\Modules;$env:PSModulePath"
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$steps = [ordered]@{ ran = $true }
$script:failed = $false
function Step([string]$name, [bool]$good, $detail) {
    $steps[$name] = [ordered]@{ ok = $good; detail = $detail }
    if (-not $good) { $script:failed = $true }
}
function NotRun([string]$why) {
    [ordered]@{ ran = $false; why = $why } | ConvertTo-Json -Compress
    exit 0
}

# ---- where it runs ------------------------------------------------------------------------------
if ($env:GITHUB_ACTIONS -ne "true" -or $env:RUNNER_ENVIRONMENT -ne "github-hosted") { NotRun "only on GitHub's hosted runners" }
if ([string](Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control").SystemStartOptions -notmatch "TESTSIGNING") { NotRun "this runner is not in test-signing mode" }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { NotRun "not an administrator" }
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $root "setup\JauntIddSetup.cs"))
if (@([JauntIddSetup]::FindDevices()).Count -or (Test-Path (Join-Path $env:ProgramFiles "jaunt-idd"))) { NotRun "jaunt-idd is installed here" }

# The displays attached to the desktop, in pixels: "<adapter> WxH at X,Y".
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class JauntIddDisplays
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplayDevicesW(string lpDevice, int iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, int dwFlags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string lpszDeviceName, int iModeNum, ref DEVMODE lpDevMode);
    [DllImport("user32.dll")]
    static extern int SetDisplayConfig(uint numPathArrayElements, IntPtr pathArray, uint numModeInfoArrayElements, IntPtr modeInfoArray, uint flags);

    // Every display device, attached or not: "<name> <string> flags 0x<StateFlags>".
    public static string[] All()
    {
        List<string> found = new List<string>();
        DISPLAY_DEVICE device = new DISPLAY_DEVICE();
        device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        for (int i = 0; EnumDisplayDevicesW(null, i, ref device, 0); i++)
        {
            found.Add(device.DeviceName + " " + device.DeviceString + " flags 0x" + device.StateFlags.ToString("X"));
            device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        }
        return found.ToArray();
    }

    // The display configuration (QueryDisplayConfig): paths from a source to a target.
    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    struct PATH_SOURCE { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
    [StructLayout(LayoutKind.Sequential)]
    struct PATH_TARGET
    {
        public LUID adapterId;
        public uint id, modeInfoIdx, outputTechnology, rotation, scaling, refreshNumerator, refreshDenominator, scanLineOrdering;
        public int targetAvailable;
        public uint statusFlags;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PATH { public PATH_SOURCE source; public PATH_TARGET target; public uint flags; }
    [StructLayout(LayoutKind.Sequential)]
    struct MODE { public uint infoType; public uint id; public LUID adapterId; public ulong a, b, c, d, e, f; }
    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPaths, out uint numModes);
    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint numPaths, [Out] PATH[] paths, ref uint numModes, [Out] MODE[] modes, IntPtr topology);
    [DllImport("user32.dll", EntryPoint = "SetDisplayConfig")]
    static extern int SetDisplayConfigSupplied(uint numPaths, [In] PATH[] paths, uint numModes, [In] MODE[] modes, uint flags);
    const uint QDC_ALL_PATHS = 0x1, QDC_ONLY_ACTIVE_PATHS = 0x2, PATH_ACTIVE = 0x1;

    static int Query(uint flags, out PATH[] paths, out MODE[] modes)
    {
        uint numPaths, numModes;
        int result = GetDisplayConfigBufferSizes(flags, out numPaths, out numModes);
        paths = new PATH[numPaths];
        modes = new MODE[numModes];
        if (result != 0) return result;
        result = QueryDisplayConfig(flags, ref numPaths, paths, ref numModes, modes, IntPtr.Zero);
        Array.Resize(ref paths, (int)numPaths);
        Array.Resize(ref modes, (int)numModes);
        return result;
    }

    static string Target(PATH p)
    {
        return p.target.adapterId.HighPart.ToString("X") + ":" + p.target.adapterId.LowPart.ToString("X") + "/" + p.target.id;
    }

    // Each target Windows has a path to: "<adapter>/<target> tech <n> available <0|1> active <0|1>".
    public static string[] Targets()
    {
        PATH[] paths;
        MODE[] modes;
        int result = Query(QDC_ALL_PATHS, out paths, out modes);
        if (result != 0) return new string[] { "QueryDisplayConfig " + result };
        Dictionary<string, string> found = new Dictionary<string, string>();
        foreach (PATH p in paths)
        {
            string key = Target(p);
            bool active = (p.flags & PATH_ACTIVE) != 0;
            if (!found.ContainsKey(key) || active)
            {
                found[key] = key + " tech " + p.target.outputTechnology + " available " + p.target.targetAvailable + " active " + (active ? 1 : 0);
            }
        }
        return new List<string>(found.Values).ToArray();
    }

    // The active paths plus one to the first target available on an adapter no active path uses (the
    // driver's), from a source of that adapter, applied as supplied (Windows picks the modes): what it
    // answers (0: done). Only that display is added; the others stay as they are.
    public static string AttachIndirect()
    {
        PATH[] all, active;
        MODE[] allModes, modes;
        int result = Query(QDC_ALL_PATHS, out all, out allModes);
        if (result != 0) return "QueryDisplayConfig(all) " + result;
        result = Query(QDC_ONLY_ACTIVE_PATHS, out active, out modes);
        if (result != 0) return "QueryDisplayConfig(active) " + result;
        foreach (PATH p in all)
        {
            if (p.target.targetAvailable == 0 || (p.flags & PATH_ACTIVE) != 0) continue;
            bool shown = false;
            foreach (PATH a in active)
            {
                shown = shown || (a.target.adapterId.LowPart == p.target.adapterId.LowPart && a.target.adapterId.HighPart == p.target.adapterId.HighPart);
            }
            if (shown) continue;
            bool used = false;
            foreach (PATH a in active)
            {
                used = used || (a.source.adapterId.LowPart == p.source.adapterId.LowPart && a.source.adapterId.HighPart == p.source.adapterId.HighPart && a.source.id == p.source.id);
            }
            if (used) continue;
            PATH added = p;
            added.flags = PATH_ACTIVE;
            added.source.modeInfoIdx = 0xFFFFFFFF;
            added.target.modeInfoIdx = 0xFFFFFFFF;
            PATH[] wanted = new PATH[active.Length + 1];
            Array.Copy(active, wanted, active.Length);
            wanted[active.Length] = added;
            // SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY | SDC_ALLOW_CHANGES
            int set = SetDisplayConfigSupplied((uint)wanted.Length, wanted, (uint)modes.Length, modes, 0x20 | 0x80 | 0x400);
            return "target " + Target(p) + " from source " + p.source.id + ": SetDisplayConfig " + set;
        }
        return "no target available on another adapter";
    }

    // The size of a path's source, "<width>x<height>", from its mode: QueryDisplayConfig's index into
    // the modes, packed (clone group in the low 16 bits, source mode in the high 16) when the path
    // supports virtual modes. "" when it has none.
    static string SourceSize(PATH p, MODE[] modes)
    {
        uint index = p.source.modeInfoIdx;
        if ((p.flags & 0x8) != 0) index = index >> 16;  // DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE
        if (index >= modes.Length || modes[index].infoType != 1) return "";  // DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE
        return (modes[index].a & 0xFFFFFFFF) + "x" + (modes[index].a >> 32);
    }

    // The size of a path's target signal (its active size), the same way.
    static string TargetSize(PATH p, MODE[] modes)
    {
        uint index = p.target.modeInfoIdx;
        if ((p.flags & 0x8) != 0) index = index & 0xFFFF;
        if (index >= modes.Length || modes[index].infoType != 2) return "";  // DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
        return (modes[index].d & 0xFFFFFFFF) + "x" + (modes[index].d >> 32);
    }

    // Every active path (QueryDisplayConfig, only the active ones), sorted: "<adapter>/<target>
    // source <size> target <size>". The topology, to compare before and after.
    public static string[] Active()
    {
        PATH[] paths;
        MODE[] modes;
        int result = Query(QDC_ONLY_ACTIVE_PATHS, out paths, out modes);
        if (result != 0) return new string[] { "QueryDisplayConfig " + result };
        List<string> found = new List<string>();
        foreach (PATH p in paths) found.Add(Target(p) + " source " + SourceSize(p, modes) + " target " + TargetSize(p, modes));
        found.Sort(StringComparer.Ordinal);
        return found.ToArray();
    }

    // The active paths whose target is on that adapter (LUID high and low), and, when `size` is not
    // empty, whose source is that size: the driver's monitor as an active display.
    public static string[] ActiveOn(uint high, uint low, string size)
    {
        PATH[] paths;
        MODE[] modes;
        if (Query(QDC_ONLY_ACTIVE_PATHS, out paths, out modes) != 0) return new string[0];
        List<string> found = new List<string>();
        foreach (PATH p in paths)
        {
            if ((uint)p.target.adapterId.HighPart != high || p.target.adapterId.LowPart != low) continue;
            string source = SourceSize(p, modes);
            if (size.Length != 0 && source != size) continue;
            found.Add(Target(p) + " source " + source + " target " + TargetSize(p, modes));
        }
        return found.ToArray();
    }

    // The active paths plus one to that target (the driver's monitor, where it arrived), from the
    // first source of its adapter no active path uses, applied as supplied (Windows picks the modes):
    // what it answers (0: done).
    public static string AttachTarget(uint high, uint low, uint targetId)
    {
        PATH[] all, active;
        MODE[] allModes, modes;
        int result = Query(QDC_ALL_PATHS, out all, out allModes);
        if (result != 0) return "QueryDisplayConfig(all) " + result;
        result = Query(QDC_ONLY_ACTIVE_PATHS, out active, out modes);
        if (result != 0) return "QueryDisplayConfig(active) " + result;
        foreach (PATH p in all)
        {
            if ((uint)p.target.adapterId.HighPart != high || p.target.adapterId.LowPart != low || p.target.id != targetId) continue;
            bool used = false;
            foreach (PATH a in active)
            {
                used = used || (a.source.adapterId.LowPart == p.source.adapterId.LowPart && a.source.adapterId.HighPart == p.source.adapterId.HighPart && a.source.id == p.source.id);
            }
            if (used) continue;
            PATH added = p;
            added.flags = PATH_ACTIVE;
            added.source.modeInfoIdx = 0xFFFFFFFF;
            added.target.modeInfoIdx = 0xFFFFFFFF;
            PATH[] wanted = new PATH[active.Length + 1];
            Array.Copy(active, wanted, active.Length);
            wanted[active.Length] = added;
            // SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY | SDC_ALLOW_CHANGES
            int set = SetDisplayConfigSupplied((uint)wanted.Length, wanted, (uint)modes.Length, modes, 0x20 | 0x80 | 0x400);
            return "target " + Target(p) + " from source " + p.source.id + ": SetDisplayConfig " + set;
        }
        return "no free path to target " + targetId;
    }

    // The desktop extended over every display connected (SDC_TOPOLOGY_EXTEND | SDC_APPLY): what
    // Windows answers (0: done).
    public static int Extend()
    {
        return SetDisplayConfig(0, IntPtr.Zero, 0, IntPtr.Zero, 0x4 | 0x80);
    }

    public static string[] Attached()
    {
        List<string> found = new List<string>();
        DISPLAY_DEVICE device = new DISPLAY_DEVICE();
        device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        for (int i = 0; EnumDisplayDevicesW(null, i, ref device, 0); i++)
        {
            if ((device.StateFlags & 0x1) != 0)  // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
            {
                DEVMODE mode = new DEVMODE();
                mode.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
                if (EnumDisplaySettingsW(device.DeviceName, -1, ref mode))  // ENUM_CURRENT_SETTINGS
                {
                    found.Add(device.DeviceString + " " + mode.dmPelsWidth + "x" + mode.dmPelsHeight + " at " + mode.dmPositionX + "," + mode.dmPositionY);
                }
            }
            device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        }
        return found.ToArray();
    }
}
"@

# A script in its own Windows PowerShell, with no input, at most $seconds: its exit code, or -1.
function RunScript([string]$path, [string[]]$arguments, [int]$seconds) {
    $all = @("-NoProfile", "-NonInteractive", "-InputFormat", "None", "-ExecutionPolicy", "Bypass", "-File", $path) + $arguments
    $quoted = ($all | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " "
    $process = Start-Process -FilePath "powershell.exe" -ArgumentList $quoted -PassThru -WindowStyle Hidden
    $null = $process.Handle
    if (-not $process.WaitForExit($seconds * 1000)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; return -1 }
    return $process.ExitCode
}
function ReadReport([string]$path) { if (Test-Path $path) { Get-Content -Raw $path | ConvertFrom-Json } else { $null } }
# (A script block sees its callers' variables: these names are the helpers' own.)
function WaitFor([scriptblock]$waitTest, [int]$waitSeconds) {
    $deadline = (Get-Date).AddSeconds($waitSeconds)
    do {
        $value = & $waitTest
        if ($value) { return $value }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $null
}
function PipeThere { [bool](@([IO.Directory]::GetFiles("\\.\pipe\")) -contains "\\.\pipe\jaunt-idd") }
function Shown([string]$size) { @([JauntIddDisplays]::Attached() | Where-Object { $_ -match " $size at " }) }
# Where Windows exposed the monitor that arrived last, from the driver's `status` (os=<adapter
# LUID>/<target id>), or $null.
function OsOf([string]$statusText) {
    if ($statusText -match ' os=([0-9A-F]{8}):([0-9A-F]{8})/(\d+) ') {
        [pscustomobject]@{ high = [Convert]::ToUInt32($Matches[1], 16); low = [Convert]::ToUInt32($Matches[2], 16); target = [Convert]::ToUInt32($Matches[3]); text = "$($Matches[1]):$($Matches[2])/$($Matches[3])" }
    } else { $null }
}
# The active displays on the driver's adapter (QueryDisplayConfig), of that size when one is given.
function ActiveOn($os, [string]$activeSize) {
    if ($os -and ($os.high -ne 0 -or $os.low -ne 0)) { @([JauntIddDisplays]::ActiveOn($os.high, $os.low, $activeSize)) } else { @() }
}
# The driver's log (its `log` request): every line it kept, oldest first, paged by its answers' bound.
function DriverLog($logPipe) {
    $lines = @()
    $from = [uint32]0
    for ($page = 0; $page -lt 60; $page++) {
        $rows = @((Ask $logPipe "log $from") -split "`r?`n")
        if ($rows[0] -notmatch '^ok next=(\d+) first=(\d+)$') { $lines += "unreadable: $($rows[0])"; break }
        $next = [uint32]$Matches[1]
        $lines += @($rows | Select-Object -Skip 1 | Where-Object { $_ })
        if ($next -le $from) { break }
        $from = $next
    }
    return $lines
}
# Up: the monitor of that size that arrived last, listed by Windows (an attached display) and active
# (QueryDisplayConfig, on the driver's adapter).
function Up($upConnection, [string]$upSize) {
    $upActive = ActiveOn (OsOf (Ask $upConnection "status")) $upSize
    if ($upActive.Count -and (Shown $upSize).Count) { $upActive } else { $null }
}
function Connect([int]$connectMs = 5000) {
    $pipe = New-Object IO.Pipes.NamedPipeClientStream(".", "jaunt-idd", [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect($connectMs)
    $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    return $pipe
}
# Waits for $aliveTest while the connection stays alive (a ping each turn; the driver's watchdog
# removes the monitors of a connection silent for 5 s).
function WaitAlive($aliveConnection, [scriptblock]$aliveTest, [int]$aliveSeconds) {
    WaitFor { $null = Ask $aliveConnection "ping"; & $aliveTest } $aliveSeconds
}
# A monitor that arrived, up (listed and active): waited for, then once more after extending the
# desktop over every display (what a monitor plugged in gets on a desktop that does it by itself).
# What extending answered, if it was needed.
function Appear($appearConnection, [string]$appearSize) {
    $seen = WaitAlive $appearConnection { Up $appearConnection $appearSize } 10
    $extend = $null
    if (-not $seen) {
        $extend = [JauntIddDisplays]::Extend()
        $seen = WaitAlive $appearConnection { Up $appearConnection $appearSize } 10
    }
    [ordered]@{ listed = @($seen); extend = $extend }
}
# One request, its answer (at most 10 s).
function Ask($pipe, [string]$request) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    $buffer = New-Object byte[] 4096  # `status` answers more than a request may be
    $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
    if (-not $read.Wait(10000)) { throw "no answer to '$request' in 10 s" }
    return [Text.Encoding]::UTF8.GetString($buffer, 0, $read.Result).Trim()
}

$started = Get-Date
$work = Join-Path ([IO.Path]::GetTempPath()) ("jaunt-idd-load-" + [guid]::NewGuid())
$thumb = $null
$pipe = $null
try {
    # ---- installed as people install an unsigned release: install.ps1 -SignLocally -------------
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item -Recurse $Package (Join-Path $work "package")
    $before = @("jaunt-idd.cat", "JauntIdd.dll") | ForEach-Object { "$_ $((Get-AuthenticodeSignature (Join-Path $work "package\driver\$_")).Status)" }
    $displaysBefore = @([JauntIddDisplays]::Attached())
    $code = RunScript (Join-Path $work "package\install.ps1") @("-Yes", "-SignLocally", "-Result", (Join-Path $work "install.json")) 300
    $install = ReadReport (Join-Path $work "install.json")
    Step "install.ps1 -SignLocally" ($code -eq 0 -and $install -and $install.installed -and $install.signedLocally) ([ordered]@{ exitCode = $code; before = $before; report = $install })
    # What it left: a certificate trusted, code signing only, not a CA, without a private key anywhere;
    # the installed catalog signed with it, the DLL as it came.
    $thumb = if ($install) { $install.localCertificate } else { $null }
    $localCert = $null
    if ($thumb) {
        $localCert = Get-ChildItem Cert:\LocalMachine\Root | Where-Object Thumbprint -eq $thumb | Select-Object -First 1
        $usages = @($localCert.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } | ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
        $constraints = @($localCert.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] })
        $installed = Join-Path $env:ProgramFiles "jaunt-idd\driver"
        $made = [ordered]@{
            subject = $localCert.Subject
            notAfter = $localCert.NotAfter.ToString("u")
            usages = $usages
            ca = $(if ($constraints.Count) { $constraints[0].CertificateAuthority } else { "unsaid" })
            stores = @("Root", "TrustedPublisher", "My" | Where-Object { Test-Path "Cert:\LocalMachine\$_\$thumb" })
            privateKey = @("Root", "TrustedPublisher", "My" | Where-Object { (Test-Path "Cert:\LocalMachine\$_\$thumb") -and (Get-Item "Cert:\LocalMachine\$_\$thumb").HasPrivateKey })
            catalog = "$((Get-AuthenticodeSignature (Join-Path $installed 'jaunt-idd.cat')).Status) $((Get-AuthenticodeSignature (Join-Path $installed 'jaunt-idd.cat')).SignerCertificate.Thumbprint)"
            dll = "$((Get-AuthenticodeSignature (Join-Path $installed 'JauntIdd.dll')).Status)"
        }
        $good = $localCert -and ($usages -join ",") -eq "1.3.6.1.5.5.7.3.3" -and $made.ca -eq $false -and ($made.stores -join ",") -eq "Root,TrustedPublisher" `
            -and -not $made.privateKey.Count -and $made.catalog -eq "Valid $thumb" -and $localCert.Subject -eq "CN=jaunt indirect display driver ($env:COMPUTERNAME)"
        Step "the certificate made: code signing only, not a CA, trusted, its private key gone; the catalog signed with it" $good $made
    }
    $devices = @([JauntIddSetup]::FindDevices())
    $status = @($devices | ForEach-Object { "$_ $((Get-PnpDevice -InstanceId $_ -ErrorAction SilentlyContinue).Status)" })
    $pipeThere = [bool](WaitFor { PipeThere } 30)
    Step "the device started, its pipe there" ($pipeThere -and @($status | Where-Object { $_ -match " OK$" }).Count -eq 1) ([ordered]@{ devices = $status; pipe = $pipeThere; displays = $displaysBefore })

    # ---- what the caller checks with the driver installed, before any monitor of ours -----------
    if ($Then -and $pipeThere) {
        $thenOut = Join-Path $work "then.out"
        $thenIn = Join-Path $work "then.in"
        New-Item -ItemType File -Force $thenIn | Out-Null
        $quoted = (@("-NoProfile", "-NonInteractive", "-InputFormat", "None", "-ExecutionPolicy", "Bypass", "-File", $Then) |
            ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join " "
        $process = Start-Process -FilePath "powershell.exe" -ArgumentList $quoted -PassThru -NoNewWindow `
            -RedirectStandardOutput $thenOut -RedirectStandardError "$thenOut.err" -RedirectStandardInput $thenIn
        $null = $process.Handle
        $code = if ($process.WaitForExit(600000)) { $process.ExitCode } else { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; -1 }
        $tail = { param($path) $text = [string](Get-Content -Raw $path -ErrorAction SilentlyContinue); if ($text.Length -gt 2000) { $text.Substring($text.Length - 2000) } else { $text } }
        Step "then: $(Split-Path -Leaf $Then)" ($code -eq 0) ([ordered]@{ exitCode = $code; output = (& $tail $thenOut); errors = (& $tail "$thenOut.err") })
    }

    if ($pipeThere) {
        # ---- a monitor added, made an ACTIVE display of its size, removed ------------------------
        # The goal, not a side effect: QueryDisplayConfig lists an active path on the driver's adapter
        # whose source is 1170x2532 (and Windows lists an attached display of that size), by itself or
        # after the desktop is extended to it; removing it brings the topology back as it was.
        $pipe = Connect
        $topologyBefore = @([JauntIddDisplays]::Active())
        $added = Ask $pipe "add 1170 2532 60"
        $os = OsOf (Ask $pipe "status")
        $active = WaitAlive $pipe { Up $pipe "1170x2532" } 20
        $how = if ($active) { "by itself" } else { $null }
        # Not active although it arrived: what Windows has, then our target added to the active paths
        # (what jaunt's agent can do), then the desktop extended over every display.
        $why = $null
        if ($added -match '^ok \d+$' -and -not $active) {
            $why = [ordered]@{
                status = Ask $pipe "status"
                devices = @([JauntIddDisplays]::All())
                targets = @([JauntIddDisplays]::Targets())
                activePaths = @([JauntIddDisplays]::Active())
                displayAdapters = @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object { "$($_.FriendlyName) $($_.Status)" })
                monitors = @(Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue | ForEach-Object { "$($_.FriendlyName) $($_.Status)" })
                renderDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object FriendlyName -match "Render" | ForEach-Object { "$($_.FriendlyName) $($_.Status)" })
                attach = if ($os) { [JauntIddDisplays]::AttachTarget($os.high, $os.low, $os.target) } else { "no arrival in status" }
            }
            $active = WaitAlive $pipe { Up $pipe "1170x2532" } 10
            if ($active) { $how = "after its target was added to the active paths" }
            $why.activeAfterAttach = [bool]$active
            $why.statusAfterAttach = Ask $pipe "status"
            if (-not $active) {
                $why.extend = [JauntIddDisplays]::Extend()
                $active = WaitAlive $pipe { Up $pipe "1170x2532" } 10
                if ($active) { $how = "after the desktop was extended" }
                $why.activeAfterExtend = [bool]$active
            }
            $why.targetsAfter = @([JauntIddDisplays]::Targets())
            $why.activePathsAfter = @([JauntIddDisplays]::Active())
            $why.statusAfter = Ask $pipe "status"
            # What the logs said meanwhile: display, desktop window manager and driver framework.
            $logs = @()
            foreach ($log in "System", "Application") {
                $logs += @(Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $started } -ErrorAction SilentlyContinue |
                    Where-Object { $_.ProviderName -match "WUDF|UMDF|DriverFrameworks|Idd|Display|Dwm|Desktop Window|Kernel-PnP|UserPnp|DeviceSetup" } |
                    Select-Object -First 12 | ForEach-Object {
                        $first = if ($_.Message) { ($_.Message -split "`r?`n")[0] } else { "" }
                        if ($first.Length -gt 200) { $first = $first.Substring(0, 200) }
                        "$log $($_.TimeCreated.ToString('HH:mm:ss')) $($_.ProviderName) $($_.Id) $($_.LevelDisplayName): $first"
                    })
            }
            $why.logs = $logs
        }
        $statusActive = Ask $pipe "status"
        $shown = @(Shown "1170x2532")
        # The topology's size while it is there, then removed.
        $activeList = @([JauntIddDisplays]::Active())
        $id = if ($added -match '^ok (\d+)$') { $Matches[1] } else { "0" }
        $removed = Ask $pipe "remove $id"
        $gone = [bool](WaitAlive $pipe { -not (Shown "1170x2532").Count -and -not (ActiveOn $os "").Count } 20)
        $restored = [bool](WaitAlive $pipe { (@([JauntIddDisplays]::Active()) -join "`n") -eq ($topologyBefore -join "`n") } 20)
        $driverLog = @(DriverLog $pipe)
        Step "a 1170x2532 monitor: added, an active display of its size, removed, the topology back" `
            ($added -match '^ok \d+$' -and $active -and $shown.Count -and $removed -eq "ok" -and $gone -and $restored) `
            ([ordered]@{ add = $added; os = $(if ($os) { $os.text } else { $null }); activeOnDriverAdapter = @($active); becameActive = $how; shown = $shown; statusActive = $statusActive
                         topologyBefore = $topologyBefore; topologyWhileThere = $activeList; notActive = $why
                         remove = $removed; gone = $gone; topologyRestored = $restored; topologyAfter = @([JauntIddDisplays]::Active()); driverLog = $driverLog })
        $pong = Ask $pipe "ping"
        $refused = Ask $pipe "add 100 100 60"
        Step "ping answered, a size out of range refused" ($pong -eq "pong" -and $refused -like "error *") ([ordered]@{ ping = $pong; add100 = $refused })
        # Two connections at once: the driver makes the next instance while one is connected (it
        # always holds one, so the name is never free for another program), and serves both.
        # (The first connection is pinged just before, and the second waited for 2 s at most: the
        # watchdog, 5 s, never closes the first meanwhile.)
        $second = $null
        $both = [ordered]@{}
        try {
            $null = Ask $pipe "ping"
            $second = Connect 2000
            $both.second = Ask $second "ping"
            $both.first = Ask $pipe "ping"
        } catch { $both.error = $_.Exception.Message }
        if ($second) { $second.Dispose() }
        try { $null = Ask $pipe "ping" } catch { $pipe.Dispose(); $pipe = Connect }  # the first connection kept, or a new one
        $both.pipeLog = @(DriverLog $pipe | Where-Object { $_ -match "pipe: " })
        $nextRefused = @($both.pipeLog | Where-Object { $_ -match "next instance cannot be made|first instance cannot be made" })
        Step "two connections at once, both answered; the next instance made each time" ($both.second -eq "pong" -and $both.first -eq "pong" -and -not $nextRefused.Count) `
            ([ordered]@{ answers = $both; driverLogSays = $nextRefused })

        # ---- a silent connection loses its monitor (the watchdog, 5 s) ---------------------------
        $added = Ask $pipe "add 1280 720 60"
        $appeared = Appear $pipe "1280x720"
        $listed = $appeared.listed | Where-Object { $_ }
        $silent = Get-Date  # nothing sent from here
        $gone = [bool](WaitFor { -not (Shown "1280x720").Count } 20)
        Step "a connection silent for 5 s: its monitor removed" ($added -match '^ok \d+$' -and $listed -and $gone) `
            ([ordered]@{ add = $added; appeared = $appeared; goneAfterSeconds = [math]::Round(((Get-Date) - $silent).TotalSeconds, 1) })
        $pipe.Dispose()
        $pipe = $null

        # ---- a closed connection loses its monitor ----------------------------------------------
        $pipe = Connect
        $added = Ask $pipe "add 1366 768 60"
        $appeared = Appear $pipe "1366x768"
        $listed = $appeared.listed | Where-Object { $_ }
        $pipe.Dispose()
        $pipe = $null
        $closed = Get-Date
        $gone = [bool](WaitFor { -not (Shown "1366x768").Count } 10)
        Step "a closed connection: its monitor removed" ($added -match '^ok \d+$' -and $listed -and $gone) `
            ([ordered]@{ add = $added; appeared = $appeared; goneAfterSeconds = [math]::Round(((Get-Date) - $closed).TotalSeconds, 1) })

        # ---- connectors: each freed and taken again, eight at most -------------------------------
        $pipe = Connect
        $cycles = @()
        foreach ($n in 1..9) {
            $added = Ask $pipe "add 640 480 60"
            $removed = if ($added -match '^ok (\d+)$') { Ask $pipe "remove $($Matches[1])" } else { "-" }
            $cycles += "$added / $removed"
        }
        Step "nine monitors made and removed in a row: each accepted" (@($cycles | Where-Object { $_ -notmatch '^ok \d+ / ok$' }).Count -eq 0) ([ordered]@{ answers = $cycles })
        $held = @()
        $answers = @()
        foreach ($n in 1..9) {
            $added = Ask $pipe "add 640 480 60"
            $answers += $added
            if ($added -match '^ok (\d+)$') { $held += $Matches[1] }
        }
        $freed = if ($held.Count) { Ask $pipe "remove $($held[0])" } else { "-" }
        $again = Ask $pipe "add 640 480 60"
        Step "eight at once, the ninth refused, one freed and taken again" `
            ($held.Count -eq 8 -and $answers[8] -eq "error no free connector" -and $freed -eq "ok" -and $again -match '^ok \d+$') `
            ([ordered]@{ answers = $answers; remove = $freed; addAgain = $again })
        $pipe.Dispose()
        $pipe = $null
        $gone = [bool](WaitFor { -not (Shown "640x480").Count } 15)
        Step "that connection closed: its eight monitors removed" $gone ([ordered]@{ displays = @([JauntIddDisplays]::Attached()) })
        # ---- the driver after that: still there and answering, or stopped or restarted ----------
        $still = [bool](WaitFor { PipeThere } 15)
        $answer = $null
        $addAfter = $null
        $removeAfter = $null
        if ($still) {
            try {
                $pipe = Connect
                $answer = Ask $pipe "status"
                # A regression check: a new monitor once that connection is gone.
                $addAfter = Ask $pipe "add 640 480 60"
                if ($addAfter -match '^ok (\d+)$') { $removeAfter = Ask $pipe "remove $($Matches[1])" }
                $pipe.Dispose()
                $pipe = $null
            } catch { $answer = "error: $($_.Exception.Message)" }
        }
        $events = @()
        foreach ($log in "System", "Application") {
            $events += @(Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $started } -ErrorAction SilentlyContinue |
                Where-Object { $_.ProviderName -match "DriverFrameworks|WUDF|UMDF|Application Error|Windows Error Reporting" -or ($_.Message -match "WUDFHost|JauntIdd") } |
                Select-Object -First 12 | ForEach-Object {
                    $first = if ($_.Message) { (($_.Message -split "`r?`n") -join " ") } else { "" }
                    if ($first.Length -gt 300) { $first = $first.Substring(0, 300) }
                    "$log $($_.TimeCreated.ToString('HH:mm:ss')) $($_.ProviderName) $($_.Id): $first"
                })
        }
        $deviceNow = @([JauntIddSetup]::FindDevices() | ForEach-Object { "$_ $((Get-PnpDevice -InstanceId $_ -ErrorAction SilentlyContinue).Status)" })
        Step "after a connection with eight monitors closed: the pipe answers status, a new add works" `
            ($still -and $answer -like "ok *" -and $addAfter -match '^ok \d+$' -and $removeAfter -eq "ok") `
            ([ordered]@{ pipe = $still; status = $answer; add = $addAfter; remove = $removeAfter; devices = $deviceNow; events = $events })
    }

    # ---- the driver's whole log, kept as a file the job uploads (out\load-driver.log) -------------
    if ($pipeThere) {
        try {
            $logPipe = Connect
            $whole = @(DriverLog $logPipe)
            $logPipe.Dispose()
            New-Item -ItemType Directory -Force (Join-Path $root "out") | Out-Null
            Set-Content -Path (Join-Path $root "out\load-driver.log") -Value $whole -Encoding utf8
        } catch { }
    }

    # ---- removed ----------------------------------------------------------------------------------
    $code = RunScript (Join-Path $env:ProgramFiles "jaunt-idd\uninstall.ps1") @("-Yes", "-Result", (Join-Path $work "uninstall.json")) 300
    $uninstall = ReadReport (Join-Path $work "uninstall.json")
    $left = [ordered]@{ devices = @([JauntIddSetup]::FindDevices()).Count; pipe = (PipeThere); folder = (Test-Path (Join-Path $env:ProgramFiles "jaunt-idd"))
                        appsEntry = (Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\jaunt-idd") }
    $left.certificate = if ($thumb) { @("Root", "TrustedPublisher", "My" | Where-Object { Test-Path "Cert:\LocalMachine\$_\$thumb" }) } else { @() }
    Step "uninstall.ps1: nothing left, the certificate made here included" `
        ($code -eq 0 -and $uninstall -and $uninstall.removed -and -not $left.devices -and -not $left.folder -and -not $left.appsEntry -and -not $left.certificate.Count) `
        ([ordered]@{ exitCode = $code; report = $uninstall; left = $left })
} catch {
    Step "stopped" $false $_.Exception.Message
} finally {
    if ($pipe) { $pipe.Dispose() }
    # Whatever happened: no driver, no certificate left.
    if (@([JauntIddSetup]::FindDevices()).Count -or (Test-Path (Join-Path $env:ProgramFiles "jaunt-idd"))) {
        $uninstaller = @((Join-Path $env:ProgramFiles "jaunt-idd\uninstall.ps1"), (Join-Path $work "package\uninstall.ps1")) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($uninstaller) { $null = RunScript $uninstaller @("-Yes") 300 }
    }
    # And any certificate install.ps1 -SignLocally made here (it did not record one if it failed).
    $localSubject = "CN=jaunt indirect display driver ($env:COMPUTERNAME)"
    foreach ($name in "My", "Root", "TrustedPublisher") {
        Get-ChildItem "Cert:\LocalMachine\$name" -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq $localSubject -or ($thumb -and $_.Thumbprint -eq $thumb) } |
            ForEach-Object { if ($name -eq "My") { Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue } else { Remove-Item $_.PSPath -ErrorAction SilentlyContinue } }
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
$steps["passed"] = -not $script:failed
$steps | ConvertTo-Json -Compress -Depth 6
# The same, readable, for the job's artifact (out\load-test.log).
try {
    New-Item -ItemType Directory -Force (Join-Path $root "out") | Out-Null
    Set-Content -Path (Join-Path $root "out\load-test.log") -Value ($steps | ConvertTo-Json -Depth 8) -Encoding utf8
} catch { }
if ($script:failed) { exit 1 }
