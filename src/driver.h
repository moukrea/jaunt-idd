// jaunt's indirect display driver (IddCx, UMDF 2): monitors of any mode, made and removed through a
// control pipe that only SYSTEM and one account may open (control.cpp), each removed when the
// connection that made it ends or stops answering (the watchdog). The structure follows Microsoft's
// IddSampleDriver (Windows-driver-samples, MIT; see NOTICE.md).
// SPDX-License-Identifier: MIT
#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace jaunt_idd {

class Driver;

// What Windows asked of the driver, for the pipe's `status`: counts, the render adapter of the last
// swap chain, and the last results of making its D3D device and handing it to the swap chain
// (E_PENDING: not yet).
struct Trace {
    std::atomic<uint32_t> DefaultModes{0}, TargetModes{0}, Commits{0}, CommitPaths{0}, ActivePaths{0};
    std::atomic<uint32_t> SwapChains{0}, Unassigned{0}, Frames{0}, RenderLow{0};
    std::atomic<long> RenderHigh{0}, Device{E_PENDING}, SetDevice{E_PENDING};
};

// The D3D device on the adapter that renders a monitor's desktop.
struct Direct3DDevice {
    explicit Direct3DDevice(LUID adapter) : AdapterLuid(adapter) {}
    HRESULT Init();
    LUID AdapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

// Takes each frame of a monitor's swap chain and hands it back: the picture is read by whoever
// captures that display (the agent, through Windows' own capture), not here.
class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent, Trace* trace);
    ~SwapChainProcessor();

private:
    static DWORD CALLBACK RunThread(LPVOID argument);
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN m_SwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_NewFrameEvent;
    HANDLE m_Thread = nullptr;
    HANDLE m_TerminateEvent = nullptr;
    Trace* m_Trace;
};

// One monitor: its mode, and the frames' processor while Windows renders to it.
struct Monitor {
    uint32_t Id = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Refresh = 60;
    uint32_t Connection = 0;
    uint32_t Connector = 0;  // the adapter's connector: 0 to MAX_MONITORS - 1, free again once it goes
    IDDCX_MONITOR Object = nullptr;
    std::unique_ptr<SwapChainProcessor> Processor;
};

class Driver {
public:
    explicit Driver(WDFDEVICE device);
    ~Driver();

    void InitAdapter();
    void FinishInit();

    // Control pipe (control.cpp): a monitor of that mode for connection `connection`; its id, or 0.
    uint32_t AddMonitor(uint32_t width, uint32_t height, uint32_t refresh, uint32_t connection, std::wstring& error);
    // Its error when it is not removed, or when Windows refused its departure (it is gone here).
    bool RemoveMonitor(uint32_t id, uint32_t connection, std::wstring& error);
    void RemoveConnection(uint32_t connection);

    // IddCx callbacks.
    bool ModeOf(uint32_t monitorId, uint32_t& width, uint32_t& height, uint32_t& refresh);
    void AssignSwapChain(uint32_t monitorId, IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain(uint32_t monitorId);

    WDFDEVICE WdfDevice() const { return m_WdfDevice; }

    // The pipe's `status` answer: "ok adapter=0x... monitors=N modes=N ..." (diagnostics).
    std::string Status();
    Trace trace;

private:
    WDFDEVICE m_WdfDevice;
    IDDCX_ADAPTER m_Adapter = nullptr;
    NTSTATUS m_AdapterStatus = STATUS_PENDING;  // what IddCxAdapterInitAsync answered
    std::mutex m_Lock;
    std::vector<std::unique_ptr<Monitor>> m_Monitors;
    uint32_t m_NextId = 1;
    std::unique_ptr<class ControlPipe> m_Pipe;
};

// The control pipe and its watchdog (control.cpp).
class ControlPipe {
public:
    ControlPipe(Driver* driver, const std::wstring& allowedSid);
    ~ControlPipe();

private:
    void Listen();
    void Serve(HANDLE pipe, uint32_t connection);

    Driver* m_Driver;
    std::wstring m_Sddl;
    std::atomic<bool> m_Stop{false};
    std::thread m_Listener;
    std::mutex m_Lock;
    std::vector<std::thread> m_Connections;
    std::atomic<uint32_t> m_NextConnection{1};
};

}  // namespace jaunt_idd

// WDF object contexts (the macro wants plain names): the device's and the adapter's driver, and a
// monitor's id.
struct JauntDeviceContext {
    jaunt_idd::Driver* pDriver;
};
struct JauntMonitorContext {
    jaunt_idd::Driver* pDriver;
    uint32_t MonitorId;
};
WDF_DECLARE_CONTEXT_TYPE(JauntDeviceContext);
WDF_DECLARE_CONTEXT_TYPE(JauntMonitorContext);
