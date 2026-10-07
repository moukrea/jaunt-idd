// jaunt's indirect display driver (see driver.h). The adapter, monitor and swap chain handling
// follow Microsoft's IddSampleDriver (Windows-driver-samples, video/IndirectDisplay, MIT; NOTICE.md).
// SPDX-License-Identifier: MIT
#include "driver.h"

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

IDDCX_MONITOR_MODE MonitorMode(uint32_t width, uint32_t height, uint32_t refresh) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO& s = mode.MonitorVideoSignalInfo;
    s.totalSize.cx = s.activeSize.cx = width;
    s.totalSize.cy = s.activeSize.cy = height;
    s.AdditionalSignalInfo.vSyncFreqDivider = 1;
    s.AdditionalSignalInfo.videoStandard = 255;
    s.vSyncFreq.Numerator = refresh;
    s.vSyncFreq.Denominator = 1;
    s.hSyncFreq.Numerator = refresh * height;
    s.hSyncFreq.Denominator = 1;
    s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    s.pixelRate = static_cast<UINT64>(refresh) * width * height;
    return mode;
}

IDDCX_TARGET_MODE TargetMode(uint32_t width, uint32_t height, uint32_t refresh) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = MonitorMode(width, height, refresh).MonitorVideoSignalInfo;
    return mode;
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
            delete context->pDriver;
            context->pDriver = nullptr;
        }
    };
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WdfObjectGet_JauntDeviceContext(device)->pDriver = new Driver(device);
    return STATUS_SUCCESS;
}

NTSTATUS JauntDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_JauntDeviceContext(device)->pDriver->InitAdapter();
    return STATUS_SUCCESS;
}

NTSTATUS JauntAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    if (NT_SUCCESS(args->AdapterInitStatus)) {
        WdfObjectGet_JauntDeviceContext(adapter)->pDriver->FinishInit();
    }
    return STATUS_SUCCESS;
}

NTSTATUS JauntAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

// Monitors are made without an EDID (any mode, up to 8192 a side): Windows asks for their modes
// below instead.
NTSTATUS JauntParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION*, IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    out->MonitorModeBufferOutputCount = 0;
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS JauntMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    uint32_t id = 0, width = 0, height = 0, refresh = 0;
    Driver* driver = DriverOf(monitor, id);
    if (!driver->ModeOf(id, width, height, refresh)) {
        return STATUS_INVALID_PARAMETER;
    }
    out->DefaultMonitorModeBufferOutputCount = 1;
    out->PreferredMonitorModeIdx = 0;
    if (in->DefaultMonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;  // asked for the count only
    }
    in->pDefaultMonitorModes[0] = MonitorMode(width, height, refresh);
    return STATUS_SUCCESS;
}

NTSTATUS JauntMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in, IDARG_OUT_QUERYTARGETMODES* out) {
    uint32_t id = 0, width = 0, height = 0, refresh = 0;
    Driver* driver = DriverOf(monitor, id);
    if (!driver->ModeOf(id, width, height, refresh)) {
        return STATUS_INVALID_PARAMETER;
    }
    out->TargetModeBufferOutputCount = 1;
    if (in->TargetModeBufferInputCount >= 1) {
        in->pTargetModes[0] = TargetMode(width, height, refresh);
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

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent)
    : m_SwapChain(swapChain), m_Device(std::move(device)), m_NewFrameEvent(newFrameEvent) {
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
    if (FAILED(IddCxSwapChainSetDevice(m_SwapChain, &setDevice))) {
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
            break;  // terminating, or the wait failed
        }
        if (FAILED(hr)) {
            break;  // the swap chain is gone (a new one comes with the next assignment)
        }
        // Nothing to do with the picture here: the desktop is read where it is shown.
        buffer.MetaData.pSurface->Release();
        if (FAILED(IddCxSwapChainFinishedProcessingFrame(m_SwapChain))) {
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
    caps.MaxMonitorsSupported = 8;
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
    if (NT_SUCCESS(IddCxAdapterInitAsync(&init, &out))) {
        m_Adapter = out.AdapterObject;
        WdfObjectGet_JauntDeviceContext(out.AdapterObject)->pDriver = this;
    }
}

void Driver::FinishInit() {
    // No monitor until the agent asks for one: the control pipe, open to SYSTEM and the account
    // the installer named.
    m_Pipe = std::make_unique<ControlPipe>(this, AllowedSid(m_WdfDevice));
}

uint32_t Driver::AddMonitor(uint32_t width, uint32_t height, uint32_t refresh, uint32_t connection, std::wstring& error) {
    std::unique_lock<std::mutex> lock(m_Lock);
    if (m_Adapter == nullptr) {
        error = L"the adapter is not ready";
        return 0;
    }
    if (m_Monitors.size() >= 8) {
        error = L"too many monitors";
        return 0;
    }
    auto monitor = std::make_unique<Monitor>();
    monitor->Id = m_NextId++;
    monitor->Width = width;
    monitor->Height = height;
    monitor->Refresh = refresh;
    monitor->Connection = connection;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JauntMonitorContext);
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = monitor->Id;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = 0;  // no EDID: its modes come from the callbacks
    info.MonitorDescription.pData = nullptr;
    CoCreateGuid(&info.MonitorContainerId);
    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attributes;
    create.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE created = {};
    // Its modes are read while it arrives: it is listed first.
    Monitor* raw = monitor.get();
    m_Monitors.push_back(std::move(monitor));
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &create, &created);
    if (!NT_SUCCESS(status)) {
        m_Monitors.pop_back();
        error = L"IddCxMonitorCreate failed";
        return 0;
    }
    raw->Object = created.MonitorObject;
    const uint32_t id = raw->Id;
    auto* context = WdfObjectGet_JauntMonitorContext(created.MonitorObject);
    context->pDriver = this;
    context->MonitorId = id;
    IDARG_OUT_MONITORARRIVAL arrival = {};
    lock.unlock();  // Windows asks for its modes from another thread meanwhile
    status = IddCxMonitorArrival(created.MonitorObject, &arrival);
    lock.lock();
    if (!NT_SUCCESS(status)) {
        for (auto it = m_Monitors.begin(); it != m_Monitors.end(); ++it) {
            if ((*it)->Id == id) {
                m_Monitors.erase(it);
                break;
            }
        }
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(created.MonitorObject));
        error = L"IddCxMonitorArrival failed";
        return 0;
    }
    return id;
}

bool Driver::RemoveMonitor(uint32_t id, uint32_t connection) {
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
        return false;
    }
    IddCxMonitorDeparture(object);
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
        RemoveMonitor(id, connection);
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

void Driver::AssignSwapChain(uint32_t monitorId, IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id != monitorId) {
            continue;
        }
        m->Processor.reset();
        auto device = std::make_shared<Direct3DDevice>(renderAdapter);
        if (FAILED(device->Init())) {
            // The system tries another adapter once this swap chain is gone.
            WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
            return;
        }
        m->Processor = std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
        return;
    }
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
}

void Driver::UnassignSwapChain(uint32_t monitorId) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& m : m_Monitors) {
        if (m->Id == monitorId) {
            m->Processor.reset();
        }
    }
}
