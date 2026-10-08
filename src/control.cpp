// jaunt's indirect display driver: the control pipe (\\.\pipe\jaunt-idd) and its watchdog.
//
// Its security descriptor lets SYSTEM and one account (the installer's `AllowedUser`) open it, no
// one else: no Everyone, no Administrators beyond SYSTEM, no network logon (a remote client is
// refused, PIPE_REJECT_REMOTE_CLIENTS). Each connection's monitors are removed when it closes, or
// when it sends nothing for WATCHDOG_MS (the agent pings every second): a monitor never outlives the
// program that asked for it.
// SPDX-License-Identifier: MIT
#include "driver.h"
#include "protocol.h"

#include <sddl.h>

#include <string>

using namespace jaunt_idd;

namespace {

constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\jaunt-idd";

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

}  // namespace

ControlPipe::ControlPipe(Driver* driver, const std::wstring& allowedSid) : m_Driver(driver) {
    // Protected (no inherited entries): SYSTEM, and that account when one is named.
    m_Sddl = L"D:P(A;;GA;;;SY)";
    if (!allowedSid.empty()) {
        m_Sddl += L"(A;;GRGW;;;" + allowedSid + L")";
    }
    m_Listener = std::thread([this] { Listen(); });
}

ControlPipe::~ControlPipe() {
    m_Stop = true;
    // Unblock ConnectNamedPipe with a connection of our own.
    HANDLE wake = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }
    if (m_Listener.joinable()) {
        m_Listener.join();
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& t : m_Connections) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ControlPipe::Listen() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(m_Sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return;  // no pipe rather than one open to anyone
    }
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    while (!m_Stop) {
        HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                       static_cast<DWORD>(MAX_MESSAGE), static_cast<DWORD>(MAX_MESSAGE), 0, &security);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        OVERLAPPED connect = {};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &connect);
        DWORD error = GetLastError();
        if (!connected && error == ERROR_IO_PENDING) {
            DWORD ignored = 0;
            connected = GetOverlappedResult(pipe, &connect, &ignored, TRUE);
        } else if (!connected && error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(connect.hEvent);
        if (!connected || m_Stop) {
            CloseHandle(pipe);
            continue;
        }
        uint32_t connection = m_NextConnection++;
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Connections.emplace_back([this, pipe, connection] { Serve(pipe, connection); });
    }
    LocalFree(descriptor);
}

void ControlPipe::Serve(HANDLE pipe, uint32_t connection) {
    char buffer[MAX_MESSAGE + 1];
    OVERLAPPED read = {};
    read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!m_Stop) {
        ResetEvent(read.hEvent);
        DWORD got = 0;
        BOOL done = ReadFile(pipe, buffer, static_cast<DWORD>(MAX_MESSAGE), &got, &read);
        if (!done && GetLastError() == ERROR_IO_PENDING) {
            // The watchdog: a connection silent for WATCHDOG_MS is treated as gone.
            DWORD wait = WaitForSingleObject(read.hEvent, WATCHDOG_MS);
            if (wait != WAIT_OBJECT_0) {
                CancelIo(pipe);
                break;
            }
            done = GetOverlappedResult(pipe, &read, &got, FALSE);
        }
        if (!done) {
            break;  // closed, or a message longer than MAX_MESSAGE
        }
        buffer[got] = '\0';
        Command c = Parse(std::string(buffer, got));
        std::string answer;
        switch (c.kind) {
            case Kind::Add: {
                std::wstring error;
                uint32_t id = m_Driver->AddMonitor(c.width, c.height, c.refresh, connection, error);
                answer = id ? Ok(id) : Error(Narrow(error));
                break;
            }
            case Kind::Remove: {
                std::wstring error;
                answer = m_Driver->RemoveMonitor(c.id, connection, error) ? Ok() : Error(Narrow(error));
                break;
            }
            case Kind::Ping:
                answer = Pong();
                break;
            case Kind::Status:
                answer = m_Driver->Status();
                break;
            default:
                answer = Error(c.error);
        }
        DWORD written = 0;
        OVERLAPPED write = {};
        write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!WriteFile(pipe, answer.data(), static_cast<DWORD>(answer.size()), &written, &write) && GetLastError() == ERROR_IO_PENDING) {
            GetOverlappedResult(pipe, &write, &written, TRUE);
        }
        CloseHandle(write.hEvent);
    }
    CloseHandle(read.hEvent);
    // Whatever this connection made goes with it.
    m_Driver->RemoveConnection(connection);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}
