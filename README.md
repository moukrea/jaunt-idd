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
  to 240 Hz), without an EDID: Windows takes the driver's mode list, so any size works.
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

`install.ps1` says what it changes and asks before changing anything:

- the driver package goes into Windows' driver store, and a device "jaunt virtual display" is added
  under Display adapters;
- only SYSTEM and the account running it (or the one given with `-AllowedUser <SID>`; `whoami
  /user` prints yours) may ask the driver for monitors;
- its files are kept in `C:\Program Files\jaunt-idd`, with an entry in Settings > Apps to remove it.

Windows then asks whether to install device software from the package's publisher ("SignPath
Foundation" for a signed release): that is this driver. `install.ps1` installs only a package whose
signature Windows accepts, never an unsigned one. It never turns on test-signing mode; on a
computer already in that mode, `-TestSigning` also accepts a build signed with a test certificate
(see Test plan).

jaunt installs it for you with `jaunt remote-desktop driver install`, which downloads a release whose
SHA-256 jaunt knows, checks its signature, shows the same changes and asks you first.

## Uninstall

Settings > Apps > "jaunt indirect display driver" > Uninstall, or as an administrator:

    powershell -ExecutionPolicy Bypass -File "C:\Program Files\jaunt-idd\uninstall.ps1"

It lists what it removes and asks first: the device (any monitor it shows goes at once), the driver
package in Windows' driver store, `C:\Program Files\jaunt-idd` and the Settings > Apps entry.
`jaunt remote-desktop driver uninstall` runs it.

## Protocol

One request per pipe message, one answer each (UTF-8 text):

| Request | Answer |
| --- | --- |
| `add <width> <height> <refresh>` | `ok <id>` |
| `remove <id>` | `ok` |
| `ping` | `pong` |

Anything else, or a value out of range: `error <why>`. `src/protocol.cpp`, tested on its own by
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
"unsigned, test-signing only": `install.ps1` refuses it as published; to try it, sign it with a
test certificate on a computer in test-signing mode.

## Test plan

GitHub's runners cannot load the driver (it is unsigned there, and test-signing mode needs a
restart), so CI builds it, runs the protocol tests and checks the install scripts. On a test
computer (a virtual machine with `bcdedit /set testsigning on` set by hand, the build signed with a
test certificate it trusts):

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
