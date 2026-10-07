// The device and driver calls install.ps1 and uninstall.ps1 make, through Windows' SetupAPI and
// newdev (as devcon does): the root-enumerated device the driver runs on, its AllowedUser value, the
// driver installed on it, and their removal. C# 5: Windows PowerShell 5.1's Add-Type compiles it.
// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

public static class JauntIddSetup
{
    public const string HardwareId = @"Root\JauntIdd";
    // The Display class (the INF's ClassGUID).
    static readonly Guid DisplayClass = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA
    {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SP_PROPCHANGE_PARAMS
    {
        public int HeaderSize;       // SP_CLASSINSTALL_HEADER.cbSize
        public int InstallFunction;  // SP_CLASSINSTALL_HEADER.InstallFunction
        public int StateChange;
        public int Scope;
        public int HwProfile;
    }

    const int DICD_GENERATE_ID = 0x1;
    const int DIF_REMOVE = 0x5;
    const int DIF_PROPERTYCHANGE = 0x12;
    const int DIF_REGISTERDEVICE = 0x19;
    const int DICS_PROPCHANGE = 0x3;
    const int DICS_FLAG_GLOBAL = 0x1;
    const int DIREG_DEV = 0x1;
    const int SPDRP_HARDWAREID = 0x1;
    const uint INSTALLFLAG_FORCE = 0x1;
    const uint SUOI_FORCEDELETE = 0x1;
    static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid ClassGuid, string Enumerator, IntPtr hwndParent, int Flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr DeviceInfoSet, int MemberIndex, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiOpenDeviceInfoW(IntPtr DeviceInfoSet, string DeviceInstanceId, IntPtr hwndParent, int OpenFlags, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, int CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Property, byte[] PropertyBuffer, int PropertyBufferSize);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Property, out int PropertyRegDataType, byte[] PropertyBuffer, int PropertyBufferSize, out int RequiredSize);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiSetClassInstallParamsW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, ref SP_PROPCHANGE_PARAMS ClassInstallParams, int ClassInstallParamsSize);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceInstanceIdW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, StringBuilder DeviceInstanceId, int DeviceInstanceIdSize, out int RequiredSize);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiCreateDevRegKeyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Scope, int HwProfile, int KeyType, IntPtr InfHandle, string InfSectionName);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupUninstallOEMInfW(string InfFileName, uint Flags, IntPtr Reserved);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool RebootRequired);

    static SP_DEVINFO_DATA NewData()
    {
        SP_DEVINFO_DATA data = new SP_DEVINFO_DATA();
        data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        return data;
    }

    static Win32Exception Failed(string what)
    {
        int error = Marshal.GetLastWin32Error();
        return new Win32Exception(error, what + " failed: " + new Win32Exception(error).Message + " (0x" + error.ToString("X8") + ")");
    }

    static string InstanceId(IntPtr set, ref SP_DEVINFO_DATA data)
    {
        StringBuilder id = new StringBuilder(512);
        int needed;
        if (!SetupDiGetDeviceInstanceIdW(set, ref data, id, id.Capacity, out needed)) throw Failed("SetupDiGetDeviceInstanceId");
        return id.ToString();
    }

    static bool HasHardwareId(IntPtr set, ref SP_DEVINFO_DATA data)
    {
        byte[] buffer = new byte[2048];
        int type, needed;
        if (!SetupDiGetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, out type, buffer, buffer.Length, out needed)) return false;
        foreach (string id in Encoding.Unicode.GetString(buffer, 0, Math.Min(needed, buffer.Length)).Split('\0'))
        {
            if (string.Equals(id, HardwareId, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    // Opens one device by its instance id and runs `action` on it.
    delegate void DeviceAction(IntPtr set, ref SP_DEVINFO_DATA data);

    static void WithDevice(string instanceId, DeviceAction action)
    {
        Guid display = DisplayClass;
        IntPtr set = SetupDiCreateDeviceInfoList(ref display, IntPtr.Zero);
        if (set == InvalidHandle) throw Failed("SetupDiCreateDeviceInfoList");
        try
        {
            SP_DEVINFO_DATA data = NewData();
            if (!SetupDiOpenDeviceInfoW(set, instanceId, IntPtr.Zero, 0, ref data)) throw Failed("SetupDiOpenDeviceInfo " + instanceId);
            action(set, ref data);
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    // The instance ids of the devices with this driver's hardware id, present or not.
    public static string[] FindDevices()
    {
        List<string> found = new List<string>();
        Guid display = DisplayClass;
        IntPtr set = SetupDiGetClassDevsW(ref display, null, IntPtr.Zero, 0);
        if (set == InvalidHandle) throw Failed("SetupDiGetClassDevs");
        try
        {
            SP_DEVINFO_DATA data = NewData();
            for (int i = 0; SetupDiEnumDeviceInfo(set, i, ref data); i++)
            {
                if (HasHardwareId(set, ref data)) found.Add(InstanceId(set, ref data));
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
        return found.ToArray();
    }

    // A new root-enumerated Display device with this driver's hardware id (no driver yet); its
    // instance id (ROOT\DISPLAY\<n>).
    public static string CreateDevice()
    {
        Guid display = DisplayClass;
        IntPtr set = SetupDiCreateDeviceInfoList(ref display, IntPtr.Zero);
        if (set == InvalidHandle) throw Failed("SetupDiCreateDeviceInfoList");
        try
        {
            SP_DEVINFO_DATA data = NewData();
            if (!SetupDiCreateDeviceInfoW(set, "Display", ref display, null, IntPtr.Zero, DICD_GENERATE_ID, ref data)) throw Failed("SetupDiCreateDeviceInfo");
            byte[] ids = Encoding.Unicode.GetBytes(HardwareId + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, ids, ids.Length)) throw Failed("SetupDiSetDeviceRegistryProperty");
            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data)) throw Failed("DIF_REGISTERDEVICE");
            return InstanceId(set, ref data);
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    // The account allowed on the driver's pipe besides SYSTEM (a SID), in the device's hardware key
    // ("Device Parameters"), where the driver reads it when it starts.
    public static void SetAllowedUser(string instanceId, string sid)
    {
        if (!Regex.IsMatch(sid, @"^S-1-[0-9]+(-[0-9]+)+$")) throw new ArgumentException("Not a SID: " + sid);
        WithDevice(instanceId, delegate(IntPtr set, ref SP_DEVINFO_DATA data)
        {
            IntPtr key = SetupDiCreateDevRegKeyW(set, ref data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, IntPtr.Zero, null);
            if (key == InvalidHandle) throw Failed("SetupDiCreateDevRegKey");
            using (RegistryKey parameters = RegistryKey.FromHandle(new SafeRegistryHandle(key, true)))
            {
                parameters.SetValue("AllowedUser", sid, RegistryValueKind.String);
            }
        });
    }

    // Installs the driver from this INF on every device with its hardware id (the driver store
    // takes the package; Windows asks whether to trust a publisher it does not trust yet). Whether
    // Windows needs a restart for it.
    public static bool InstallDriver(string infPath)
    {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, HardwareId, Path.GetFullPath(infPath), INSTALLFLAG_FORCE, out reboot)) throw Failed("UpdateDriverForPlugAndPlayDevices");
        return reboot;
    }

    // Stops and starts the device again (the driver reads AllowedUser when it starts).
    public static void Restart(string instanceId)
    {
        WithDevice(instanceId, delegate(IntPtr set, ref SP_DEVINFO_DATA data)
        {
            SP_PROPCHANGE_PARAMS change = new SP_PROPCHANGE_PARAMS();
            change.HeaderSize = 8;
            change.InstallFunction = DIF_PROPERTYCHANGE;
            change.StateChange = DICS_PROPCHANGE;
            change.Scope = DICS_FLAG_GLOBAL;
            change.HwProfile = 0;
            if (!SetupDiSetClassInstallParamsW(set, ref data, ref change, Marshal.SizeOf(typeof(SP_PROPCHANGE_PARAMS)))) throw Failed("SetupDiSetClassInstallParams");
            if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, ref data)) throw Failed("DIF_PROPERTYCHANGE");
        });
    }

    // Removes the device (its monitors go with it).
    public static void Remove(string instanceId)
    {
        WithDevice(instanceId, delegate(IntPtr set, ref SP_DEVINFO_DATA data)
        {
            if (!SetupDiCallClassInstaller(DIF_REMOVE, set, ref data)) throw Failed("DIF_REMOVE " + instanceId);
        });
    }

    // Removes a driver package from the driver store by its published name (oem<n>.inf).
    public static void RemoveDriverPackage(string publishedName)
    {
        if (!Regex.IsMatch(publishedName, @"^oem[0-9]+\.inf$", RegexOptions.IgnoreCase)) throw new ArgumentException("Not a published driver package name: " + publishedName);
        if (!SetupUninstallOEMInfW(publishedName, SUOI_FORCEDELETE, IntPtr.Zero)) throw Failed("SetupUninstallOEMInf " + publishedName);
    }
}
