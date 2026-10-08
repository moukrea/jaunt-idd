# jaunt indirect display driver

A small Windows indirect display driver (IddCx, UMDF 2) that gives
[jaunt](https://github.com/moukrea/jaunt)'s remote desktop virtual monitors of any size: a phone
held upright or a tablet viewing this computer sees a display of its own size. MIT licensed
(LICENSE). Its sources draw on Microsoft's IddSampleDriver and on the design of the Virtual Display
Driver (NOTICE.md).

## What it does

- Windows 10 version 1903 (build 18362) or later, x64 or ARM64: IddCx 1.4, UMDF 2.25.
- No monitor until a program asks for one. The driver listens on a named pipe,
  `\\.\pipe\jaunt-idd`, and adds a monitor of the mode it is given (320 to 8192 pixels a side, 24
  to 240 Hz), without an EDID: Windows takes the driver's mode list, so any size works. Up to 8
  monitors at once, one per connector of the adapter; a monitor that goes frees its connector.
- **Who may ask:** the pipe's security descriptor lets SYSTEM and one account open it, the one named
  at installation (`AllowedUser`, a SID, in the device's hardware key). No one else: not Everyone,
  not Administrators, not remote clients (`PIPE_REJECT_REMOTE_CLIENTS`). Without that value, SYSTEM
  only.
- **A monitor never outlives the program that asked for it:** each connection's monitors are removed
  when it closes, when its process ends, or when it sends nothing for 5 seconds (the watchdog; jaunt
  pings every second). A connection removes only its own monitors.
- The frames rendered to a monitor are released at once: whoever views that display reads it through
  Windows' own capture (Windows.Graphics.Capture or DXGI Desktop Duplication), as for any monitor.
- It sends nothing over the network: it talks only to the local program that opened its pipe.

## Install

1. From [the releases](https://github.com/moukrea/jaunt-idd/releases), download the zip for this
   computer (`jaunt-idd-x64.zip` or `jaunt-idd-ARM64.zip`) and `SHA256SUMS`, and check it:
   `Get-FileHash jaunt-idd-x64.zip` gives the SHA-256 listed there.
2. Unzip it, and in that folder, as an administrator:

       powershell -ExecutionPolicy Bypass -File install.ps1

   For an unsigned release (all of them until SignPath Foundation signs one), add `-SignLocally`
   (below).

`install.ps1` says what it changes and asks before changing anything:

- the driver package goes into Windows' driver store, and a device "jaunt virtual display" is added
  under Display adapters;
- only SYSTEM and the account running it (or the one given with `-AllowedUser <SID>`; `whoami
  /user` prints yours) may ask the driver for monitors;
- its files are kept in `C:\Program Files\jaunt-idd`, with an entry in Settings > Apps to remove it.

For a signed release, Windows then asks whether to install device software from its publisher
("SignPath Foundation"): that is this driver. `install.ps1` installs only a package whose catalog
Windows accepts the signature of, never an unsigned one (the DLL, if not signed itself, is in the
catalog, and Windows checks it as it installs). It never turns on test-signing mode; on a computer
already in that mode, `-TestSigning` also accepts a build signed with a test certificate (see Test
plan).

### An unsigned release: `-SignLocally`

`install.ps1 -SignLocally` installs an unsigned release by making this computer trust a certificate
made on it, for it alone. It says so and asks first, then:

1. makes a certificate, "CN=jaunt indirect display driver (<computer name>)", valid 2 years, that
   can sign code only: its only usage is code signing, and it is not a certificate authority, so it
   cannot vouch for any other certificate. Its private key is in the computer's store, not
   exportable;
2. adds its public part to the computer's Trusted Root Certification Authorities and Trusted
   Publishers, so Windows accepts what it signed without asking;
3. signs the package's catalog with it (the catalog holds the DLL's and the INF's hashes);
4. **deletes its private key**, before the driver is installed: nothing else can ever be signed with
   it, on this computer or elsewhere;
5. installs the driver as above.

If any step fails, everything it did is undone. Uninstalling removes that certificate from both
stores. A catalog that is already signed is never signed again: a signed release needs no
`-SignLocally`, and a catalog signed by someone Windows does not trust is refused.

This trusts a certificate nobody else has seen, which is what SignPath Foundation's signature avoids:
signed releases stay the preferred way, and `jaunt remote-desktop driver install` uses one as soon as
one exists.

jaunt installs it for you with `jaunt remote-desktop driver install`, which downloads a release whose
SHA-256 jaunt knows, shows the same changes (the certificate made here included, for an unsigned
release) and asks you first, then runs `install.ps1` as an administrator.

## Uninstall

Settings > Apps > "jaunt indirect display driver" > Uninstall, or as an administrator:

    powershell -ExecutionPolicy Bypass -File "C:\Program Files\jaunt-idd\uninstall.ps1"

It lists what it removes and asks first: the device (any monitor it shows goes at once), the driver
package in Windows' driver store, `C:\Program Files\jaunt-idd`, the Settings > Apps entry, and the
certificate `-SignLocally` made, from the trusted stores.
`jaunt remote-desktop driver uninstall` runs it.

## Protocol

One request per pipe message, one answer each (UTF-8 text):

| Request | Answer |
| --- | --- |
| `add <width> <height> <refresh>` | `ok <id>` |
| `remove <id>` | `ok` |
| `ping` | `pong` |
| `status` | `ok adapter=0x… monitors=… modes=… targets=… commits=… paths=… active=… swapchains=… render=… device=0x… setdevice=0x… frames=… unassigned=… dxgi=…` |

Anything else, or a value out of range: `error <why>`. A ninth monitor at once: `error no free
connector`. When Windows refuses a call, the error ends with its NTSTATUS, e.g. `error
IddCxMonitorArrival failed (0xC0000001)`. `status` says what Windows asked of the driver, for diagnosis: how often
it asked for monitor and target modes, committed modes (with how many paths, how many active)
and assigned swap chains, the render adapter of the last one, the results of making its D3D
device and handing it over (`0x8000000A`: not yet), the frames received, and the render adapters
DXGI listed to the driver (`vendor:device:flags:LUID`). `src/protocol.cpp`, tested on its own by
`tests/protocol_test.cpp` (any C++17 compiler).

## Build

On Windows with Visual Studio's C++ tools (the Windows Driver Kit is taken from NuGet when it is
not installed):

    powershell -ExecutionPolicy Bypass -File build.ps1 -Platform x64 [-Version 1.2.3]

It runs the protocol tests and builds the package in `out\<platform>\package`: `install.ps1`,
`uninstall.ps1`, `setup\` and `driver\` (`JauntIdd.dll`, `jaunt-idd.inf` stamped with the version,
`jaunt-idd.cat`), all unsigned. It prints one JSON line: what it built, the DLL's version
information, and what was missing. `tests\scripts_test.ps1` checks the install scripts as far as a
computer that cannot load the driver can (CI runs both, `.github/workflows/ci.yml`).

## Releases

A tag `vMAJOR.MINOR.PATCH` releases the driver (`.github/workflows/release.yml`): built on GitHub's
runners for x64 and ARM64, then signed through SignPath Foundation (below), with `SHA256SUMS`.
Until this repository's signing is set up, a release is published unsigned, as a pre-release titled
"unsigned": `install.ps1 -SignLocally` installs it (above), and plain `install.ps1` refuses it.

## Test plan

CI builds the driver, runs the protocol tests and checks the install scripts. GitHub's Windows
runner boots in test-signing mode, so CI also signs the build with a throwaway test certificate
made in the job, installs it, adds and removes a monitor, checks that a silent or closed connection
loses its monitors, uninstalls it and removes the certificate (`tests/load_test.ps1`, which runs on
GitHub's hosted runners only; reported for now). On a test computer (a virtual machine with
`bcdedit /set testsigning on` set by hand, the build signed with a test certificate it trusts):

1. `install.ps1 -TestSigning`; `Get-PnpDevice -FriendlyName "jaunt virtual display"` shows it
   started, and `\\.\pipe\jaunt-idd` exists.
2. As the account allowed: `add 1170 2532 60` on the pipe answers `ok 1`; Settings > Display lists
   a 1170×2532 display beside the others; `remove 1` takes it away.
3. As another account, and as an administrator who is not SYSTEM: opening the pipe is refused
   (access denied).
4. The watchdog: `add`, then stop sending; the monitor goes within 5 s. `add`, then kill the client
   process; it goes at once.
5. jaunt: a phone views this computer and picks "This device's size"; the view shows a display of
   that size, input lands on it; closing the view removes it.
6. Sizes 320×320 and 8192×4320 work; 100×100 is refused.
7. `uninstall.ps1`: the device, the driver package (`pnputil /enum-drivers`), the folder and the
   Settings > Apps entry are gone.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/).

Only `.github/workflows/release.yml` submits signing requests, on GitHub's runners, for a tag of
this repository: it builds the driver from that tag, has the DLL signed (SignPath checks its product
name and version, `.signpath/artifact-configurations/driver-dll.xml`), makes the catalog over the
signed DLL, has the catalog signed, and publishes the package with its checksums. Each signing
request is approved by hand. Only binaries built from this repository's source are signed.

- Committers and reviewers: [@moukrea](https://github.com/moukrea) (every change by someone else
  is reviewed before it is merged).
- Approvers: [@moukrea](https://github.com/moukrea).

Privacy: this program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it.
