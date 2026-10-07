jaunt's indirect display driver {version}: a Windows indirect display driver (IddCx, UMDF 2) that
adds a virtual monitor of any size when the one account allowed asks for it, and removes it when
that program closes the connection or stops answering. jaunt's remote desktop uses it to show a
display sized to the device viewing it. Windows 10 version 1903 or later, x64 and ARM64.

{signing}

**Install:** download the zip for this computer (`jaunt-idd-x64.zip` or `jaunt-idd-ARM64.zip`),
check it against `SHA256SUMS`, unzip it, and run `install.ps1` as an administrator. It says what it
changes and asks first. **Uninstall:** `uninstall.ps1`, or Settings > Apps > jaunt indirect display
driver. Details: https://github.com/moukrea/jaunt-idd#readme

**Privacy:** this program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it.

**Code signing policy:** https://github.com/moukrea/jaunt-idd#code-signing-policy
