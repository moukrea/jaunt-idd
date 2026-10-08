# Load the driver on GitHub's own Windows runner, use it, and remove it. The runner boots in
# test-signing mode: this signs the package with a throwaway test certificate made here, installs it
# with install.ps1 -TestSigning, adds and removes a monitor through the pipe, checks that a silent
# connection and a closed one lose their monitors, then runs uninstall.ps1.
# It runs nowhere else: only where GITHUB_ACTIONS is true and RUNNER_ENVIRONMENT is github-hosted, on
# a computer already in test-signing mode (this never turns that mode on), as an administrator,
# with no jaunt-idd installed. The certificate's private key cannot be exported and never leaves
# this run; the certificate leaves the machine's stores at the end, whatever happened.
#   powershell -NoProfile -NonInteractive -InputFormat None -ExecutionPolicy Bypass -File tests\load_test.ps1 -Package out\x64\package [-Then <script.ps1>]
# -Then: a script run with the driver installed, before it is removed (a program that uses the
# driver checks itself there); its exit code and output are a step.
# Writes one JSON line: {ran: false, why} where it does not run, else each step; exits 1 when a step
# failed (CI reports it).
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
function Connect {
    $pipe = New-Object IO.Pipes.NamedPipeClientStream(".", "jaunt-idd", [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(5000)
    $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    return $pipe
}
# Waits for $aliveTest while the connection stays alive (a ping each turn; the driver's watchdog
# removes the monitors of a connection silent for 5 s).
function WaitAlive($aliveConnection, [scriptblock]$aliveTest, [int]$aliveSeconds) {
    WaitFor { $null = Ask $aliveConnection "ping"; & $aliveTest } $aliveSeconds
}
# A monitor that arrived, listed by Windows: waited for, then once more after extending the desktop
# over every display (what a monitor plugged in gets on a desktop that does it by itself). What
# extending answered, if it was needed.
function Appear($appearConnection, [string]$appearSize) {
    $seen = WaitAlive $appearConnection { Shown $appearSize } 10
    $extend = $null
    if (-not $seen) {
        $extend = [JauntIddDisplays]::Extend()
        $seen = WaitAlive $appearConnection { Shown $appearSize } 10
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
$cert = $null
$pipe = $null
try {
    # ---- the throwaway certificate, trusted on this machine only ----------------------------
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=jaunt-idd CI throwaway test certificate" -CertStoreLocation "Cert:\LocalMachine\My" `
        -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddHours(3)
    $public = New-Object Security.Cryptography.X509Certificates.X509Certificate2(, $cert.RawData)
    foreach ($name in "Root", "TrustedPublisher") {
        $store = New-Object Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
        $store.Open("ReadWrite")
        $store.Add($public)
        $store.Close()
    }
    Step "a throwaway test certificate, trusted on this runner" $true ([ordered]@{ subject = $cert.Subject; notAfter = $cert.NotAfter.ToString("u") })

    # ---- the package signed with it: the DLL, the catalog made again over it, the catalog ------
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item -Recurse $Package (Join-Path $work "package")
    $driver = Join-Path $work "package\driver"
    function Sign([string]$file) {
        $signed = Set-AuthenticodeSignature -FilePath $file -Certificate $cert -HashAlgorithm SHA256
        if ($signed.Status -eq "Valid") { return "Set-AuthenticodeSignature" }
        # Else the SDK's signtool, which the runner has.
        $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $signtool) { throw "$(Split-Path -Leaf $file): $($signed.Status) ($($signed.StatusMessage)), and no signtool" }
        & $signtool.FullName sign /sha1 $cert.Thumbprint /sm /s My /fd sha256 $file | Out-Null
        return "signtool"
    }
    $dllBy = Sign (Join-Path $driver "JauntIdd.dll")
    $catalog = & (Join-Path $root "catalog.ps1") -Driver $driver -Platform x64 | Select-Object -Last 1
    $catBy = Sign (Join-Path $driver "jaunt-idd.cat")
    $signatures = @("jaunt-idd.cat", "JauntIdd.dll") | ForEach-Object { "$_ $((Get-AuthenticodeSignature (Join-Path $driver $_)).Status)" }
    Step "the package signed with it" (@($signatures | Where-Object { $_ -notmatch " Valid$" }).Count -eq 0) ([ordered]@{ signatures = $signatures; dll = $dllBy; catalog = $catBy; catalogMade = $catalog })

    # ---- installed ------------------------------------------------------------------------------
    $displaysBefore = @([JauntIddDisplays]::Attached())
    $code = RunScript (Join-Path $work "package\install.ps1") @("-Yes", "-TestSigning", "-Result", (Join-Path $work "install.json")) 300
    $install = ReadReport (Join-Path $work "install.json")
    Step "install.ps1 -TestSigning" ($code -eq 0 -and $install -and $install.installed) ([ordered]@{ exitCode = $code; report = $install })
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
        # ---- a monitor added, listed by Windows, removed ----------------------------------------
        $pipe = Connect
        $added = Ask $pipe "add 1170 2532 60"
        $listed = WaitAlive $pipe { Shown "1170x2532" } 20
        # Not listed although it arrived: what Windows has, and what extending the desktop does.
        $why = $null
        if ($added -match '^ok \d+$' -and -not $listed) {
            $why = [ordered]@{
                status = Ask $pipe "status"
                devices = @([JauntIddDisplays]::All())
                targets = @([JauntIddDisplays]::Targets())
                displayAdapters = @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object { "$($_.FriendlyName) $($_.Status)" })
                renderDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object FriendlyName -match "Render" | ForEach-Object { "$($_.FriendlyName) $($_.Status)" })
                # Only our display added to the active ones (what jaunt's agent could do), then, if
                # still not listed, the desktop extended over every display.
                attach = [JauntIddDisplays]::AttachIndirect()
            }
            $listed = WaitAlive $pipe { Shown "1170x2532" } 10
            $why.listedAfterAttach = [bool]$listed
            $why.statusAfterAttach = Ask $pipe "status"
            if (-not $listed) {
                $why.extend = [JauntIddDisplays]::Extend()
                $listed = WaitAlive $pipe { Shown "1170x2532" } 10
                $why.listedAfterExtend = [bool]$listed
            }
            $why.targetsAfter = @([JauntIddDisplays]::Targets())
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
        $id = if ($added -match '^ok (\d+)$') { $Matches[1] } else { "0" }
        $removed = Ask $pipe "remove $id"
        $gone = [bool](WaitAlive $pipe { -not (Shown "1170x2532").Count } 20)
        Step "a 1170x2532 monitor: added, listed, removed" ($added -match '^ok \d+$' -and $listed -and $removed -eq "ok" -and $gone) `
            ([ordered]@{ add = $added; listed = @($listed); notListed = $why; remove = $removed; gone = $gone })
        $pong = Ask $pipe "ping"
        $refused = Ask $pipe "add 100 100 60"
        Step "ping answered, a size out of range refused" ($pong -eq "pong" -and $refused -like "error *") ([ordered]@{ ping = $pong; add100 = $refused })

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

    # ---- removed ----------------------------------------------------------------------------------
    $code = RunScript (Join-Path $env:ProgramFiles "jaunt-idd\uninstall.ps1") @("-Yes", "-Result", (Join-Path $work "uninstall.json")) 300
    $uninstall = ReadReport (Join-Path $work "uninstall.json")
    $left = [ordered]@{ devices = @([JauntIddSetup]::FindDevices()).Count; pipe = (PipeThere); folder = (Test-Path (Join-Path $env:ProgramFiles "jaunt-idd"))
                        appsEntry = (Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\jaunt-idd") }
    Step "uninstall.ps1: nothing left" ($code -eq 0 -and $uninstall -and $uninstall.removed -and -not $left.devices -and -not $left.folder -and -not $left.appsEntry) `
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
    if ($cert) {
        foreach ($name in "My", "Root", "TrustedPublisher") {
            $store = New-Object Security.Cryptography.X509Certificates.X509Store($name, "LocalMachine")
            $store.Open("ReadWrite")
            foreach ($found in @($store.Certificates.Find("FindByThumbprint", $cert.Thumbprint, $false))) { $store.Remove($found) }
            $store.Close()
        }
        $left = @(Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher, Cert:\LocalMachine\My | Where-Object Thumbprint -eq $cert.Thumbprint)
        Step "the certificate removed from the machine's stores" (-not $left.Count) $null
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
$steps["passed"] = -not $script:failed
$steps | ConvertTo-Json -Compress -Depth 6
if ($script:failed) { exit 1 }
