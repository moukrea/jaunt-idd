// jaunt's indirect display driver (see driver.h). The adapter, monitor and swap chain handling
// follow Microsoft's IddSampleDriver (Windows-driver-samples, video/IndirectDisplay, MIT; NOTICE.md).
// SPDX-License-Identifier: MIT
#include "driver.h"
#include "protocol.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

using namespace Microsoft::WRL;
using namespace jaunt_idd;

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD JauntDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY JauntDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED JauntAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES JauntAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION JauntParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES JauntMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES JauntMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN JauntMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN JauntMonitorUnassignSwapChain;

namespace {

// The account allowed on the control pipe besides SYSTEM: its SID, written by the installer in
// the device's hardware key (`AllowedUser`, REG_SZ). None: SYSTEM only.
std::wstring AllowedSid(WDFDEVICE device) {
    WDFKEY key = nullptr;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return L"";
    }
    WCHAR buffer[200] = {};
    DWORD size = sizeof(buffer) - sizeof(WCHAR);
    LSTATUS read = RegGetValueW(static_cast<HKEY>(WdfRegistryWdmGetHandle(key)), nullptr, L"AllowedUser", RRF_RT_REG_SZ, nullptr, buffer, &size);
    WdfRegistryClose(key);
    if (read != ERROR_SUCCESS) {
        return L"";
    }
    std::wstring sid(buffer);
    // Only a SID's characters: it goes into a security descriptor.
    for (wchar_t c : sid) {
        if (!(c == L'S' || c == L'-' || (c >= L'0' && c <= L'9'))) {
            return L"";
        }
    }
    return sid.rfind(L"S-1-", 0) == 0 ? sid : L"";
}

// A mode's signal. IddCx.h: a monitor mode's vSyncFreqDivider has to be zero, a target mode's cannot
// be (the desktop is updated at vSyncFreq / vSyncFreqDivider); Windows refuses a monitor whose
// modes break this, when it arrives.
DISPLAYCONFIG_VIDEO_SIGNAL_INFO SignalInfo(uint32_t width, uint32_t height, uint32_t refresh, UINT32 divider) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO s = {};
    s.totalSize.cx = s.activeSize.cx = width;
    s.totalSize.cy = s.activeSize.cy = height;
    s.AdditionalSignalInfo.vSyncFreqDivider = divider;
    s.AdditionalSignalInfo.videoStandard = 255;
    s.vSyncFreq.Numerator = refresh;
    s.vSyncFreq.Denominator = 1;
    s.hSyncFreq.Numerator = refresh * height;
    s.hSyncFreq.Denominator = 1;
    s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    s.pixelRate = static_cast<UINT64>(refresh) * width * height;
    return s;
}

IDDCX_MONITOR_MODE MonitorMode(uint32_t width, uint32_t height, uint32_t refresh, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = SignalInfo(width, height, refresh, 0);
    return mode;
}

IDDCX_TARGET_MODE TargetMode(uint32_t width, uint32_t height, uint32_t refresh) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = SignalInfo(width, height, refresh, 1);
    // Zero, as Microsoft's sample leaves it. IddCx.h: the OS never picks modes whose bandwidths add
    // up to more than the adapter's MaxDisplayPipelineRate, which this adapter leaves at zero (no
    // limit, "if the driver does not have these restrictions then it can set all bandwidths to
    // zero"): a bandwidth above zero against a limit of zero is a mode no path could take. (0.2.0
    // stated width * height * refresh here.)
    mode.RequiredBandwidth = static_cast<UINT64>(refresh) * width * height;  // BISECT ONLY: the 0.2.0 value, with the EDID kept
    return mode;
}

// The driver (one device per host process), for the callbacks Windows makes without a monitor or an
// adapter in their arguments (parsing an EDID).
std::atomic<Driver*> g_Driver{nullptr};

// "<high>:<low>", a LUID as the status and the log write it.
#define LUID_FORMAT "%08X:%08X"
#define LUID_ARGS(luid) static_cast<unsigned>((luid).HighPart), static_cast<unsigned>((luid).LowPart)

// An error from a Windows call, with its status for the pipe: "<what> (0x<NTSTATUS>)".
std::wstring WithStatus(const wchar_t* what, NTSTATUS status) {
    wchar_t text[128];
    swprintf_s(text, L"%ls (0x%08X)", what, static_cast<unsigned>(status));
    return text;
}

// The render adapters DXGI lists to the driver, for `status`: "<vendor>:<device>:<flags>:<luid>,...".
// (On GitHub's runner, a virtual machine without a GPU: the software Basic Render Driver, flags 2,
// and the display-only and indirect adapters it renders for, this one included, with its ids.)
void ListRenderAdapters(char* listed, size_t size) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT made = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(made)) {
        snprintf(listed, size, "factory 0x%08X", static_cast<unsigned>(made));
        return;
    }
    listed[0] = '\0';
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            size_t used = strlen(listed);
            snprintf(listed + used, size - used, "%s%04X:%04X:%X:%08X:%08X", used ? "," : "", desc.VendorId, desc.DeviceId,
                     static_cast<unsigned>(desc.Flags), static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
        }
        adapter.Reset();
    }
    if (!listed[0]) {
        snprintf(listed, size, "none");
    }
}

Driver* DriverOf(IDDCX_MONITOR monitor, uint32_t& id) {
    auto* context = WdfObjectGet_JauntMonitorContext(monitor);
    id = context->MonitorId;
    return context->pDriver;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, JauntDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

NTSTATUS JauntDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = JauntDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = JauntAdapterInitFinished;
    config.EvtIddCxParseMonitorDescription = JauntParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = JauntMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes = JauntMonitorQueryModes;
    config.EvtIddCxAdapterCommitModes = JauntAdapterCommitModes;
    config.EvtIddCxMonitorAssignSwapChain = JauntMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = JauntMonitorUnassignSwapChain;
    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JauntDeviceContext);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* context = WdfObjectGet_JauntDeviceContext(object);
        if (context) {
            Driver* gone = context->pDriver;
            Driver* expected = gone;
            g_Driver.compare_exchange_strong(expected, nullptr);
            delete gone;
            context->pDriver = nullptr;
        }
    };
    WDFDEVICE device = nullptr;
    const NTSTATUS created = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(created)) {
        return created;
    }
    const NTSTATUS initialized = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(initialized)) {
        return initialized;
    }
    auto* driver = new Driver(device);
    WdfObjectGet_JauntDeviceContext(device)->pDriver = driver;
    g_Driver = driver;
    driver->trace.events.Add("DeviceAdd: IddCxDeviceInitConfig 0x%08X, WdfDeviceCreate 0x%08X, IddCxDeviceInitialize 0x%08X", static_cast<unsigned>(status),
                             static_cast<unsigned>(created), static_cast<unsigned>(initialized));
    return STATUS_SUCCESS;
}

NTSTATUS JauntDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    Driver* driver = WdfObjectGet_JauntDeviceContext(device)->pDriver;
    driver->trace.events.Add("D0Entry: from power state %d", static_cast<int>(previous));
    driver->InitAdapter();
    return STATUS_SUCCESS;
}

NTSTATUS JauntAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    Driver* driver = WdfObjectGet_JauntDeviceContext(adapter)->pDriver;
    driver->trace.events.Add("AdapterInitFinished: AdapterInitStatus 0x%08X", static_cast<unsigned>(args->AdapterInitStatus));
    if (NT_SUCCESS(args->AdapterInitStatus)) {
        driver->FinishInit();
    }
    return STATUS_SUCCESS;
}

NTSTATUS JauntAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* in) {
    // Nothing to set up for a mode here; counted for `status`, each path written to the log.
    Driver* driver = WdfObjectGet_JauntDeviceContext(adapter)->pDriver;
    if (driver) {
        uint32_t active = 0;
        for (UINT i = 0; i < in->PathCount; ++i) {
            if (in->pPaths[i].Flags & IDDCX_PATH_FLAGS_ACTIVE) {
                ++active;
            }
        }
        driver->trace.Commits++;
        driver->trace.CommitPaths = in->PathCount;
        driver->trace.ActivePaths = active;
        driver->trace.events.Add("CommitModes: %u paths, %u active", static_cast<unsigned>(in->PathCount), static_cast<unsigned>(active));
        for (UINT i = 0; i < in->PathCount && i < MAX_MONITORS; ++i) {
            const IDDCX_PATH& path = in->pPaths[i];
            uint32_t id = 0;
            if (path.MonitorObject != nullptr) {
                id = WdfObjectGet_JauntMonitorContext(path.MonitorObject)->MonitorId;
            }
            const auto& signal = path.TargetVideoSignalInfo;
            driver->trace.events.Add("CommitModes: path %u monitor %u flags 0x%X active %ux%u vsync %u/%u divider %u", static_cast<unsigned>(i),
                                     static_cast<unsigned>(id), static_cast<unsigned>(path.Flags), static_cast<unsigned>(signal.activeSize.cx),
                                     static_cast<unsigned>(signal.activeSize.cy), static_cast<unsigned>(signal.vSyncFreq.Numerator),
                                     static_cast<unsigned>(signal.vSyncFreq.Denominator), static_cast<unsigned>(signal.AdditionalSignalInfo.vSyncFreqDivider));
        }
    }
    return STATUS_SUCCESS;
}

// Windows asks the driver for the modes of an EDID (parsing it is the driver's): ours, made in
// AddMonitor, is known by its serial; the modes are the monitor's own list, the one its target modes
// give too.
NTSTATUS JauntParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in, IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    Driver* driver = g_Driver;
    const auto& description = in->MonitorDescription;
    uint32_t serial = 0;
    Mode described;
    const bool ours = ReadEdid(static_cast<const uint8_t*>(description.pData), description.DataSize, serial, described);
    if (driver) {
        driver->trace.ParsedEdids++;
    }
    std::vector<Mode> modes;
    if (ours) {
        if (driver == nullptr || !driver->ModesOfSerial(serial, modes)) {
            modes.assign(1, described);  // an EDID of ours, of no monitor now: its own timing
        }
    }
    if (driver) {
        driver->trace.events.Add("ParseMonitorDescription: type %d size %u, ours %d serial %08X, %u modes, buffer of %u", static_cast<int>(description.Type),
                                 static_cast<unsigned>(description.DataSize), ours ? 1 : 0, static_cast<unsigned>(serial), static_cast<unsigned>(modes.size()),
                                 static_cast<unsigned>(in->MonitorModeBufferInputCount));
    }
    out->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    out->PreferredMonitorModeIdx = 0;
    if (!ours) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in->MonitorModeBufferInputCount < modes.size()) {
        // No buffer: the caller asks for the count only.
        return in->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    for (size_t i = 0; i < modes.size(); ++i) {
        // The mode the EDID's timing states comes from the description; one it cannot state (a side
        // over 4095) is the driver's.
        in->pMonitorModes[i] = MonitorMode(modes[i].width, modes[i].height, modes[i].refresh,
                                           modes[i].described ? IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR : IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    return STATUS_SUCCESS;
}

// Only for a monitor without an EDID, which this driver never makes: here for completeness, and
// logged in case Windows asks.
NTSTATUS JauntMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    uint32_t id = 0;
    Driver* driver = DriverOf(monitor, id);
    driver->trace.DefaultModes++;
    std::vector<Mode> modes;
    const bool known = driver->ModesOfId(id, modes);
    driver->trace.events.Add("GetDefaultDescriptionModes: monitor %u known %d, %u modes, buffer of %u", static_cast<unsigned>(id), known ? 1 : 0,
                             static_cast<unsigned>(modes.size()), static_cast<unsigned>(in->DefaultMonitorModeBufferInputCount));
    if (!known) {
        return STATUS_INVALID_PARAMETER;
    }
    out->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    out->PreferredMonitorModeIdx = 0;
    if (in->DefaultMonitorModeBufferInputCount < modes.size()) {
        return STATUS_SUCCESS;  // asked for the count only
    }
    for (size_t i = 0; i < modes.size(); ++i) {
        in->pDefaultMonitorModes[i] = MonitorMode(modes[i].width, modes[i].height, modes[i].refresh, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    return STATUS_SUCCESS;
}

NTSTATUS JauntMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in, IDARG_OUT_QUERYTARGETMODES* out) {
    uint32_t id = 0;
    Driver* driver = DriverOf(monitor, id);
    driver->trace.TargetModes++;
    std::vector<Mode> modes;
    const bool known = driver->ModesOfId(id, modes);
    driver->trace.events.Add("QueryTargetModes: monitor %u known %d, %u modes, buffer of %u", static_cast<unsigned>(id), known ? 1 : 0,
                             static_cast<unsigned>(modes.size()), static_cast<unsigned>(in->TargetModeBufferInputCount));
    if (!known) {
        return STATUS_INVALID_PARAMETER;
    }
    out->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (in->TargetModeBufferInputCount >= modes.size()) {
        for (size_t i = 0; i < modes.size(); ++i) {
            in->pTargetModes[i] = TargetMode(modes[i].width, modes[i].height, modes[i].refresh);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS JauntMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in) {
    uint32_t id = 0;
    DriverOf(monitor, id)->AssignSwapChain(id, in->hSwapChain, in->RenderAdapterLuid, in->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

NTSTATUS JauntMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    uint32_t id = 0;
    DriverOf(monitor, id)->UnassignSwapChain(id);
    return STATUS_SUCCESS;
}

// ---- Direct3D and the swap chain (as in Microsoft's sample) ------------------------------------

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) {
        return hr;
    }
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) {
        return hr;
    }
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr,
                             &DeviceContext);
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent, Trace* trace)
    : m_SwapChain(swapChain), m_Device(std::move(device)), m_NewFrameEvent(newFrameEvent), m_Trace(trace) {
    m_TerminateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_Thread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(m_TerminateEvent);
    if (m_Thread) {
        WaitForSingleObject(m_Thread, INFINITE);
        CloseHandle(m_Thread);
    }
    CloseHandle(m_TerminateEvent);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID argument) {
    static_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    // The frames' thread in the multimedia class, as the sample does.
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Distribution", &task);
    RunCore();
    // The swap chain is the system's: deleting it makes the system make a new one if needed.
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(m_SwapChain));
    m_SwapChain = nullptr;
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_Device->Device.As(&dxgiDevice))) {
        return;
    }
    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    HRESULT set = IddCxSwapChainSetDevice(m_SwapChain, &setDevice);
    m_Trace->SetDevice = set;
    m_Trace->events.Add("swap chain: IddCxSwapChainSetDevice 0x%08X", static_cast<unsigned>(set));
    if (FAILED(set)) {
        return;
    }
    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(m_SwapChain, &buffer);
        if (hr == E_PENDING) {
            HANDLE events[] = {m_NewFrameEvent, m_TerminateEvent};
            DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 16);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
                continue;
            }
            m_Trace->events.Add("swap chain: stopped (wait %u)", static_cast<unsigned>(wait));
            break;  // terminating, or the wait failed
        }
        if (FAILED(hr)) {
            m_Trace->events.Add("swap chain: IddCxSwapChainReleaseAndAcquireBuffer 0x%08X, done", static_cast<unsigned>(hr));
            break;  // the swap chain is gone (a new one comes with the next assignment)
        }
        // Nothing to do with the picture here: the desktop is read where it is shown.
        buffer.MetaData.pSurface->Release();
        if (m_Trace->Frames++ == 0) {
            m_Trace->events.Add("swap chain: first frame");
        }
        const HRESULT finished = IddCxSwapChainFinishedProcessingFrame(m_SwapChain);
        if (FAILED(finished)) {
            m_Trace->events.Add("swap chain: IddCxSwapChainFinishedProcessingFrame 0x%08X, done", static_cast<unsigned>(finished));
            break;
        }
    }
}

// ---- the driver ------------------------------------------------------------------------------

Driver::Driver(WDFDEVICE device) : m_WdfDevice(device) {}

Driver::~Driver() {
    m_Pipe.reset();
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Monitors.clear();
}

void Driver::InitAdapter() {
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = MAX_MONITORS;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"jaunt virtual display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"jaunt";
    caps.EndPointDiagnostics.pEndPointModelName = L"jaunt virtual display";
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JauntDeviceContext);
    IDARG_IN_ADAPTER_INIT init = {};
    init.WdfDevice = m_WdfDevice;
    init.pCaps = &caps;
    init.ObjectAttributes = &attributes;
    IDARG_OUT_ADAPTER_INIT out = {};
    m_AdapterStatus = IddCxAdapterInitAsync(&init, &out);
    trace.events.Add("IddCxAdapterInitAsync 0x%08X: up to %u monitors, no pipeline limit (MaxDisplayPipelineRate 0), flags 0x%X",
                     static_cast<unsigned>(m_AdapterStatus), static_cast<unsigned>(caps.MaxMonitorsSupported), static_cast<unsigned>(caps.Flags));
    if (NT_SUCCESS(m_AdapterStatus)) {
        m_Adapter = out.AdapterObject;
        WdfObjectGet_JauntDeviceContext(out.AdapterObject)->pDriver = this;
    }
}

void Driver::FinishInit() {
    ListRenderAdapters(trace.Adapters, sizeof(trace.Adapters));  // for `status`, before the pipe opens
    trace.events.Add("render adapters DXGI lists (vendor:device:flags:luid): %s", trace.Adapters);
    // No monitor until the agent asks for one: the control pipe, open to SYSTEM and the account
    // the installer named.
    m_Pipe = std::make_unique<ControlPipe>(this, AllowedSid(m_WdfDevice));
}

uint32_t Driver::AddMonitor(uint32_t width, uint32_t height, uint32_t refresh, uint32_t connection, std::wstring& error) {
    std::unique_lock<std::mutex> lock(m_Lock);
    if (m_Adapter == nullptr) {
        error = WithStatus(L"the adapter is not ready", m_AdapterStatus);
        return 0;
    }
    // The lowest connector free: a monitor removed (asked, or its connection gone) frees its own.
    uint32_t connector = MAX_MONITORS;
    for (uint32_t c = 0; c < MAX_MONITORS && connector == MAX_MONITORS; ++c) {
        bool used = false;
        for (auto& m : m_Monitors) {
            used = used || m->Connector == c;
        }
        if (!used) {
            connector = c;
        }
    }
    if (connector == MAX_MONITORS) {
        error = L"no free connector";
        return 0;
    }
    auto monitor = std::make_unique<Monitor>();
    monitor->Id = m_NextId++;
    monitor->Connector = connector;
    monitor->Width = width;
    monitor->Height = height;
    monitor->Refresh = refresh;
    monitor->Connection = connection;
    // Its EDID and the modes it gives rise to: one source for the EDID's parse and the target modes.
    monitor->Serial = SerialFor(connector, width, height, refresh);
    monitor->EdidBlock = MakeEdid(width, height, refresh, monitor->Serial);
    monitor->Modes = ModeList(width, height, refresh);
    const uint32_t id = monitor->Id;
    trace.events.Add("add %ux%u@%u for connection %u: monitor %u on connector %u, EDID serial %08X, %u modes (first %ux%u@%u)", static_cast<unsigned>(width),
                     static_cast<unsigned>(height), static_cast<unsigned>(refresh), static_cast<unsigned>(connection), static_cast<unsigned>(id),
                     static_cast<unsigned>(connector), static_cast<unsigned>(monitor->Serial), static_cast<unsigned>(monitor->Modes.size()),
                     static_cast<unsigned>(monitor->Modes[0].width), static_cast<unsigned>(monitor->Modes[0].height),
                     static_cast<unsigned>(monitor->Modes[0].refresh));

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JauntMonitorContext);
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    // The connector type Windows shows: HDMI, as Microsoft's IddSampleDriver and the Virtual Display Driver say.
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = connector;  // 0 to MaxMonitorsSupported - 1 (IddCx.h)
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = static_cast<UINT>(monitor->EdidBlock.size());  // 128 bytes: see edid.h
    info.MonitorDescription.pData = monitor->EdidBlock.data();
    CoCreateGuid(&info.MonitorContainerId);
    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attributes;
    create.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE created = {};
    // Its modes are read while it is created and arrives: it is listed first, and the lock is not
    // held meanwhile (Windows asks for them from another thread, or from this one).
    Monitor* raw = monitor.get();
    m_Monitors.push_back(std::move(monitor));
    lock.unlock();
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &create, &created);
    lock.lock();
    trace.events.Add("monitor %u: IddCxMonitorCreate 0x%08X", static_cast<unsigned>(id), static_cast<unsigned>(status));
    if (!NT_SUCCESS(status)) {
        for (auto it = m_Monitors.begin(); it != m_Monitors.end(); ++it) {
            if ((*it)->Id == id) {
                m_Monitors.erase(it);
                break;
            }
        }
        error = WithStatus(L"IddCxMonitorCreate failed", status);
        return 0;
    }
    raw->Object = created.MonitorObject;
    auto* context = WdfObjectGet_JauntMonitorContext(created.MonitorObject);
    context->pDriver = this;
    context->MonitorId = id;
    IDARG_OUT_MONITORARRIVAL arrival = {};
    lock.unlock();  // Windows asks for its modes from another thread meanwhile
    status = IddCxMonitorArrival(created.MonitorObject, &arrival);
    lock.lock();
    trace.events.Add("monitor %u: IddCxMonitorArrival 0x%08X, OsAdapterLuid " LUID_FORMAT ", OsTargetId %u", static_cast<unsigned>(id),
                     static_cast<unsigned>(status), LUID_ARGS(arrival.OsAdapterLuid), static_cast<unsigned>(arrival.OsTargetId));
    if (!NT_SUCCESS(status)) {
        for (auto it = m_Monitors.begin(); it != m_Monitors.end(); ++it) {
            if ((*it)->Id == id) {
                m_Monitors.erase(it);
                break;
            }
        }
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(created.MonitorObject));
        error = WithStatus(L"IddCxMonitorArrival failed", status);
        return 0;
    }
    raw->OsLuid = arrival.OsAdapterLuid;
    raw->OsTargetId = arrival.OsTargetId;
    trace.Arrivals++;
    trace.OsLow = arrival.OsAdapterLuid.LowPart;
    trace.OsHigh = arrival.OsAdapterLuid.HighPart;
    trace.OsTarget = arrival.OsTargetId;
    return id;
}

bool Driver::RemoveMonitor(uint32_t id, uint32_t connection, std::wstring& error) {
    IDDCX_MONITOR object = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        for (auto it = m_Monitors.begin(); it != m_Monitors.end(); ++it) {
            // Only the connection that made it removes it.
            if ((*it)->Id == id && (*it)->Connection == connection) {
                object = (*it)->Object;
                (*it)->Processor.reset();
                m_Monitors.erase(it);
                break;
            }
        }
    }
    if (object == nullptr) {
        error = L"no such monitor of this connection";
        return false;
    }
    NTSTATUS status = IddCxMonitorDeparture(object);
    trace.events.Add("monitor %u: IddCxMonitorDeparture 0x%08X", static_cast<unsigned>(id), static_cast<unsigned>(status));
    if (!NT_SUCCESS(status)) {
        error = WithStatus(L"IddCxMonitorDeparture failed", status);
        return false;
    }
    return true;
}

void Driver::RemoveConnection(uint32_t connection) {
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        for (auto& m : m_Monitors) {
            if (m->Connection == connection) {
                ids.push_back(m->Id);
            }
        }
    }
    for (uint32_t id : ids) {
        std::wstring ignored;  // the connection is gone: no one to tell
        RemoveMonitor(id, connection, ignored);
    }
}

bool Driver::ModeOf(uint32_t monitorId, uint32_t& width, uint32_t& height, uint32_t& refresh) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id == monitorId) {
            width = m->Width;
            height = m->Height;
            refresh = m->Refresh;
            return true;
        }
    }
    return false;
}

bool Driver::ModesOfId(uint32_t monitorId, std::vector<Mode>& modes) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id == monitorId) {
            modes = m->Modes;
            return true;
        }
    }
    return false;
}

bool Driver::ModesOfSerial(uint32_t serial, std::vector<Mode>& modes) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Serial == serial) {
            modes = m->Modes;
            return true;
        }
    }
    return false;
}

void Driver::AssignSwapChain(uint32_t monitorId, IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id != monitorId) {
            continue;
        }
        m->Processor.reset();
        trace.SwapChains++;
        trace.RenderLow = renderAdapter.LowPart;
        trace.RenderHigh = renderAdapter.HighPart;
        auto device = std::make_shared<Direct3DDevice>(renderAdapter);
        HRESULT made = device->Init();
        trace.Device = made;
        trace.events.Add("AssignSwapChain: monitor %u, render adapter " LUID_FORMAT ", Direct3D device 0x%08X", static_cast<unsigned>(monitorId),
                         LUID_ARGS(renderAdapter), static_cast<unsigned>(made));
        if (FAILED(made)) {
            // The system tries another adapter once this swap chain is gone.
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
            return;
        }
        m->Processor = std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent, &trace);
        return;
    }
    trace.events.Add("AssignSwapChain: monitor %u is not known, swap chain deleted", static_cast<unsigned>(monitorId));
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
}

void Driver::UnassignSwapChain(uint32_t monitorId) {
    trace.Unassigned++;
    trace.events.Add("UnassignSwapChain: monitor %u", static_cast<unsigned>(monitorId));
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id == monitorId) {
            m->Processor.reset();
        }
    }
}

std::string Driver::Status() {
    size_t monitors = 0;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        monitors = m_Monitors.size();
    }
    char text[1300];
    // os=<adapter LUID>/<target id>: where Windows exposed the last monitor that arrived (0 arrivals: zeros).
    snprintf(text, sizeof(text),
             "ok adapter=0x%08X monitors=%u modes=%u targets=%u commits=%u paths=%u active=%u swapchains=%u render=%08X:%08X device=0x%08X "
             "setdevice=0x%08X frames=%u unassigned=%u edids=%u arrivals=%u os=%08X:%08X/%u dxgi=%s\n",
             static_cast<unsigned>(m_AdapterStatus), static_cast<unsigned>(monitors), trace.DefaultModes.load(), trace.TargetModes.load(),
             trace.Commits.load(), trace.CommitPaths.load(), trace.ActivePaths.load(), trace.SwapChains.load(),
             static_cast<unsigned>(trace.RenderHigh.load()), trace.RenderLow.load(), static_cast<unsigned>(trace.Device.load()),
             static_cast<unsigned>(trace.SetDevice.load()), trace.Frames.load(), trace.Unassigned.load(), trace.ParsedEdids.load(), trace.Arrivals.load(),
             static_cast<unsigned>(trace.OsHigh.load()), trace.OsLow.load(), trace.OsTarget.load(), trace.Adapters);
    return text;
}
