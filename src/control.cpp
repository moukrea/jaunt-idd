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
#include <vector>

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

std::string SidText(PSID sid) {
    LPWSTR text = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &text)) {
        return "?";
    }
    std::string out = Narrow(text);
    LocalFree(text);
    return out;
}

// What the driver's host process is (the pipe's creator and owner), for the log: its token's user,
// owner, integrity level, and whether it is restricted (with its restricting SIDs). Well-known
// service SIDs, no person's.
void LogHostToken(Driver* driver) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        driver->trace.events.Add("pipe: the host's token cannot be read (error %lu)", GetLastError());
        return;
    }
    auto query = [&](TOKEN_INFORMATION_CLASS what) {
        std::vector<BYTE> data;
        DWORD size = 0;
        GetTokenInformation(token, what, nullptr, 0, &size);
        if (size == 0 || size > 65536) {
            return data;
        }
        data.resize(size);
        if (!GetTokenInformation(token, what, data.data(), size, &size)) {
            data.clear();
        }
        return data;
    };
    std::vector<BYTE> user = query(TokenUser), owner = query(TokenOwner), label = query(TokenIntegrityLevel), restricting = query(TokenRestrictedSids);
    std::string restrictedSids;
    if (!restricting.empty()) {
        const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(restricting.data());
        for (DWORD i = 0; i < groups->GroupCount && i < 6; ++i) {
            restrictedSids += (i ? "," : "") + SidText(groups->Groups[i].Sid);
        }
    }
    driver->trace.events.Add("pipe: host token user %s owner %s integrity %s restricted %d (%s)",
                             user.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid).c_str(),
                             owner.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_OWNER*>(owner.data())->Owner).c_str(),
                             label.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid).c_str(),
                             IsTokenRestricted(token) ? 1 : 0, restrictedSids.empty() ? "none" : restrictedSids.c_str());
    CloseHandle(token);
}

}  // namespace

ControlPipe::ControlPipe(Driver* driver, const std::wstring& allowedSid) : m_Driver(driver) {
    // Protected (no inherited entries): SYSTEM, and that account when one is named. The pipe's owner
    // (the account the driver's host runs as, LocalService) gets FILE_CREATE_PIPE_INSTANCE (0x4)
    // alone, through OWNER RIGHTS: Windows checks it on every instance after the first, and without
    // it the next instance could not be made while a connection holds one.
    m_Sddl = L"D:P(A;;GA;;;SY)(A;;0x4;;;OW)";
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
        m_Driver->trace.events.Add("pipe: its security descriptor was refused (error %lu): no pipe", GetLastError());
        return;  // no pipe rather than one open to anyone
    }
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    LogHostToken(m_Driver);
    // The first instance is made with FILE_FLAG_FIRST_PIPE_INSTANCE: if another program already made a pipe of this name (to
    // receive the agent's commands, or answer them), creation fails instead of joining its pipe. From then on this listener
    // always holds an instance (the next one is made before the connected one is handed over), so the name never becomes free
    // for anyone else between two connections, and the later instances need not, and cannot, use the flag.
    auto create = [&](bool first) {
        return CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                static_cast<DWORD>(MAX_MESSAGE), static_cast<DWORD>(MAX_MESSAGE), 0, &security);
    };
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool told = false;
    while (!m_Stop && (pipe = create(true)) == INVALID_HANDLE_VALUE) {
        if (!told) {
            told = true;
            m_Driver->trace.events.Add("pipe: the first instance cannot be made (error %lu): the name is taken, or creation failed; trying every second",
                                       GetLastError());
        }
        Sleep(1000);  // the name is taken (or creation failed): never join another program's pipe
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        m_Driver->trace.events.Add("pipe: listening (first instance)");
    }
    while (!m_Stop) {
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
        if (m_Stop) {
            break;
        }
        if (!connected) {
            DisconnectNamedPipe(pipe);  // the same instance waits again: the name stays ours
            continue;
        }
        // The next instance first, while the connected one still holds the name.
        HANDLE next = create(false);
        const DWORD nextError = next == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        // The connection is served whatever happened: a client is never left without an answer.
        uint32_t connection = m_NextConnection++;
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            m_Connections.emplace_back([this, pipe, connection] { Serve(pipe, connection); });
        }
        if (next == INVALID_HANDLE_VALUE) {
            // Not made (it should be: see the security descriptor). Then a first instance again, once
            // the connections served have closed theirs: never another program's pipe.
            m_Driver->trace.events.Add("pipe: the next instance cannot be made (error %lu); a first instance again once the connections close",
                                       nextError);
            while (!m_Stop && (next = create(true)) == INVALID_HANDLE_VALUE) {
                Sleep(250);
            }
        }
        pipe = next;
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
    LocalFree(descriptor);
}

void ControlPipe::Serve(HANDLE pipe, uint32_t connection) {
    m_Driver->trace.events.Add("pipe: connection %u opened", static_cast<unsigned>(connection));
    const char* ended = "closed or unreadable";
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
                ended = "silent for the watchdog's delay";
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
                m_Driver->trace.events.Add("pipe: connection %u add -> %s", static_cast<unsigned>(connection), answer.substr(0, answer.size() - 1).c_str());
                break;
            }
            case Kind::Remove: {
                std::wstring error;
                answer = m_Driver->RemoveMonitor(c.id, connection, error) ? Ok() : Error(Narrow(error));
                m_Driver->trace.events.Add("pipe: connection %u remove %u -> %s", static_cast<unsigned>(connection), static_cast<unsigned>(c.id),
                                           answer.substr(0, answer.size() - 1).c_str());
                break;
            }
            case Kind::Ping:
                answer = Pong();
                break;
            case Kind::Status:
                answer = m_Driver->Status();
                break;
            case Kind::Log:
                answer = m_Driver->trace.events.Read(c.from, MAX_LOG_ANSWER);
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
    m_Driver->trace.events.Add("pipe: connection %u ended (%s)", static_cast<unsigned>(connection), ended);
    // Whatever this connection made goes with it.
    m_Driver->RemoveConnection(connection);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}
