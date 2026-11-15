// jaunt's indirect display driver: the control pipe (\\.\pipe\jaunt-idd) and its watchdog.
//
// Who may open it: SYSTEM and one account (the installer's `AllowedUser`), no one else: no
// Everyone, no Administrators beyond SYSTEM, no other service, no network logon (a remote client is
// refused, PIPE_REJECT_REMOTE_CLIENTS). Each connection's monitors are removed when it closes, or when
// it sends nothing for WATCHDOG_MS (the agent pings every second): a monitor never outlives the
// program that asked for it.
//
// How the name stays the driver's: the first instance is made with FILE_FLAG_FIRST_PIPE_INSTANCE (if
// another program made a pipe of this name first, creation fails, and the driver never joins it), then
// a fixed pool of PIPE_INSTANCES instances is made at once and reused for ever (each disconnected and
// connected again), so the name is never free and no instance is ever made later. Windows checks the
// pipe's DACL each time an instance after the first is made (the open mode's read and write), and the
// driver's host runs as LocalService, which the final DACL does not name: so the pool is made under a
// startup DACL that also grants the host process's own logon SID (its token's TokenLogonSid, which
// no other process has) read, write and WRITE_DAC, then the DACL is replaced by the final one (SYSTEM
// and the allowed account), and one more instance is tried and must be refused (logged). Only if
// Windows refuses the instances under the logon SID is OWNER RIGHTS (LocalService) tried, and the log
// says which one made the pool. No other principal can make an instance, ever.
// SPDX-License-Identifier: MIT
#include "driver.h"
#include "protocol.h"

#include <aclapi.h>
#include <sddl.h>

#include <string>
#include <vector>

using namespace jaunt_idd;

namespace {

constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\jaunt-idd";
// Connections at once: a monitor each (MAX_MONITORS), and room for the agent's own checks and for
// `status` and `log`.
constexpr size_t PIPE_INSTANCES = 2 * MAX_MONITORS;

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
// owner, integrity level, whether it is restricted, and its groups (the principals a narrower DACL
// entry could name). Well-known and service SIDs, no person's.
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
    std::vector<BYTE> user = query(TokenUser), owner = query(TokenOwner), label = query(TokenIntegrityLevel), groups = query(TokenGroups);
    driver->trace.events.Add("pipe: host token user %s owner %s integrity %s restricted %d",
                             user.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid).c_str(),
                             owner.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_OWNER*>(owner.data())->Owner).c_str(),
                             label.empty() ? "?" : SidText(reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid).c_str(),
                             IsTokenRestricted(token) ? 1 : 0);
    if (!groups.empty()) {
        const auto* list = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
        std::string line;
        for (DWORD i = 0; i < list->GroupCount; ++i) {
            char attributes[16];
            snprintf(attributes, sizeof(attributes), ":%lX", static_cast<unsigned long>(list->Groups[i].Attributes));
            std::string item = SidText(list->Groups[i].Sid) + attributes;
            if (line.size() + item.size() + 1 > 150) {
                driver->trace.events.Add("pipe: host token groups %s", line.c_str());
                line.clear();
            }
            line += (line.empty() ? "" : ",") + item;
        }
        if (!line.empty()) {
            driver->trace.events.Add("pipe: host token groups %s", line.c_str());
        }
    }
    CloseHandle(token);
}

// The host process's logon SID (S-1-5-5-x-y, or S-1-5-84-... for a user-mode driver host) as text:
// its token's TokenLogonSid. Empty when it has none or cannot be read.
std::wstring HostLogonSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return L"";
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &size);
    std::wstring out;
    if (size > 0 && size <= 65536) {
        std::vector<BYTE> data(size);
        if (GetTokenInformation(token, TokenLogonSid, data.data(), size, &size)) {
            const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
            LPWSTR text = nullptr;
            if (groups->GroupCount == 1 && ConvertSidToStringSidW(groups->Groups[0].Sid, &text)) {
                out = text;
                LocalFree(text);
            }
        }
    }
    CloseHandle(token);
    return out;
}

// The pipe's DACL as SDDL, the allowed account's SID written <allowed> (the log names no account).
std::string DaclText(HANDLE pipe, const std::wstring& allowedSid) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD got = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (got != ERROR_SUCCESS) {
        return "unreadable (error " + std::to_string(got) + ")";
    }
    LPWSTR text = nullptr;
    std::wstring sddl;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr)) {
        sddl = text;
        LocalFree(text);
    }
    LocalFree(descriptor);
    if (!allowedSid.empty()) {
        for (size_t at = sddl.find(allowedSid); at != std::wstring::npos; at = sddl.find(allowedSid)) {
            sddl.replace(at, allowedSid.size(), L"<allowed>");
        }
    }
    return Narrow(sddl);
}

}  // namespace

ControlPipe::ControlPipe(Driver* driver, const std::wstring& allowedSid) : m_Driver(driver), m_AllowedSid(allowedSid) {
    m_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_Listener = std::thread([this] { Start(); });
}

ControlPipe::~ControlPipe() {
    m_Stop = true;
    SetEvent(m_StopEvent);  // wakes every instance waiting for a client, and every watchdog wait
    if (m_Listener.joinable()) {
        m_Listener.join();
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    for (auto& t : m_Instances) {
        if (t.joinable()) {
            t.join();
        }
    }
    CloseHandle(m_StopEvent);
}

void ControlPipe::Start() {
    // The final DACL, protected (no inherited entries): SYSTEM, and the allowed account when one is
    // named. The startup one adds the pipe's owner (OWNER RIGHTS: the host's account) for as long as
    // the pool is being made.
    std::wstring closedDacl = L"D:P(A;;GA;;;SY)";
    if (!m_AllowedSid.empty()) {
        closedDacl += L"(A;;GRGW;;;" + m_AllowedSid + L")";
    }
    PSECURITY_DESCRIPTOR startupDescriptor = nullptr, finalDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(closedDacl.c_str(), SDDL_REVISION_1, &finalDescriptor, nullptr)) {
        m_Driver->trace.events.Add("pipe: its security descriptor was refused (error %lu): no pipe", GetLastError());
        return;  // no pipe rather than one open to anyone
    }
    LogHostToken(m_Driver);
    // Who may make the pool's instances while it is made, narrowest first: the driver host's own
    // logon session (its token's logon SID, held by this host process alone: the other services
    // running as LocalService have their own), then, only if Windows refuses that, OWNER RIGHTS (the
    // host's account, LocalService). The log says which one made the pool.
    struct Principal {
        const char* name;
        std::wstring sid;
    };
    std::vector<Principal> principals;
    const std::wstring logon = HostLogonSid();
    if (!logon.empty()) {
        principals.push_back({"the host's logon SID", logon});
    }
    principals.push_back({"OWNER RIGHTS (fallback: LocalService)", L"OW"});
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, FALSE};
    const DWORD pipeMode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    std::vector<HANDLE> pool;
    DWORD refused = 0;
    for (size_t p = 0; p < principals.size() && !m_Stop; ++p) {
        LocalFree(startupDescriptor);
        startupDescriptor = nullptr;
        const std::wstring startup = closedDacl + L"(A;;GRGWWD;;;" + principals[p].sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(startup.c_str(), SDDL_REVISION_1, &startupDescriptor, nullptr)) {
            m_Driver->trace.events.Add("pipe: the startup descriptor for %s was refused (error %lu)", principals[p].name, GetLastError());
            continue;
        }
        security.lpSecurityDescriptor = startupDescriptor;
        const bool last = p + 1 == principals.size();
        bool told = false;
        while (!m_Stop) {
            // (No instance limit: the pool is fixed by the driver; a further instance can only be
            // refused by the DACL, which is what the check below shows.)
            HANDLE first = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE | WRITE_DAC, pipeMode,
                                            PIPE_UNLIMITED_INSTANCES, static_cast<DWORD>(MAX_MESSAGE), static_cast<DWORD>(MAX_MESSAGE), 0, &security);
            if (first != INVALID_HANDLE_VALUE) {
                pool.push_back(first);
                break;
            }
            if (!told) {
                told = true;
                m_Driver->trace.events.Add("pipe: the first instance cannot be made (error %lu): the name is taken, or creation failed; trying every second",
                                           GetLastError());
            }
            WaitForSingleObject(m_StopEvent, 1000);  // never joining another program's pipe
        }
        refused = 0;
        while (!m_Stop && !pool.empty() && pool.size() < PIPE_INSTANCES) {
            HANDLE next = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, pipeMode, PIPE_UNLIMITED_INSTANCES,
                                           static_cast<DWORD>(MAX_MESSAGE), static_cast<DWORD>(MAX_MESSAGE), 0, &security);
            if (next == INVALID_HANDLE_VALUE) {
                refused = GetLastError();
                break;
            }
            pool.push_back(next);
        }
        if (pool.size() == PIPE_INSTANCES || last) {
            m_Driver->trace.events.Add("pipe: startup principal %s", principals[p].name);
            break;
        }
        // Windows refused this principal's instances (error `refused`): all closed, the next one tried.
        m_Driver->trace.events.Add("pipe: %u of %u instances made under %s (error %lu): trying the next principal", static_cast<unsigned>(pool.size()),
                                   static_cast<unsigned>(PIPE_INSTANCES), principals[p].name, refused);
        for (HANDLE h : pool) {
            CloseHandle(h);
        }
        pool.clear();
    }
    if (!pool.empty()) {
        m_Driver->trace.events.Add("pipe: %u instances made of %u (error %lu)", static_cast<unsigned>(pool.size()), static_cast<unsigned>(PIPE_INSTANCES), refused);
        // The final DACL, on the pipe its instances share.
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        DWORD set = ERROR_INVALID_SECURITY_DESCR;
        if (GetSecurityDescriptorDacl(finalDescriptor, &present, &dacl, &defaulted) && present) {
            set = SetSecurityInfo(pool[0], SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        }
        m_Driver->trace.events.Add("pipe: final DACL set (error %lu), read back %s", set, DaclText(pool.back(), m_AllowedSid).c_str());
        // One more instance must be refused now: the host's account no longer may make one.
        HANDLE more = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, pipeMode, PIPE_UNLIMITED_INSTANCES,
                                       static_cast<DWORD>(MAX_MESSAGE), static_cast<DWORD>(MAX_MESSAGE), 0, &security);
        const DWORD moreError = more == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (more != INVALID_HANDLE_VALUE) {
            CloseHandle(more);
        }
        m_Driver->trace.events.Add(more == INVALID_HANDLE_VALUE ? "pipe: a further instance is refused now (error %lu)"
                                                                 : "pipe: UNEXPECTED: a further instance was made after the final DACL (%lu)",
                                   moreError);
        if (set != ERROR_SUCCESS) {
            // Fail closed: no pipe rather than one the host's account may still join.
            for (HANDLE h : pool) {
                CloseHandle(h);
            }
            pool.clear();
            m_Driver->trace.events.Add("pipe: closed, its DACL could not be set");
        }
    }
    LocalFree(startupDescriptor);
    LocalFree(finalDescriptor);
    if (!pool.empty()) {
        m_Driver->trace.events.Add("pipe: listening on %u instances", static_cast<unsigned>(pool.size()));
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    for (HANDLE h : pool) {
        m_Instances.emplace_back([this, h] { Instance(h); });
    }
}

void ControlPipe::Instance(HANDLE pipe) {
    while (!m_Stop) {
        OVERLAPPED connect = {};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &connect);
        const DWORD error = GetLastError();
        DWORD ignored = 0;
        if (!connected && error == ERROR_IO_PENDING) {
            HANDLE events[] = {connect.hEvent, m_StopEvent};
            if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                connected = GetOverlappedResult(pipe, &connect, &ignored, FALSE);
            } else {
                CancelIo(pipe);
                GetOverlappedResult(pipe, &connect, &ignored, TRUE);  // the cancelled wait done before its OVERLAPPED goes
                connected = FALSE;
            }
        } else if (!connected && error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(connect.hEvent);
        if (m_Stop) {
            break;
        }
        if (connected) {
            Serve(pipe, m_NextConnection++);
        } else {
            DisconnectNamedPipe(pipe);  // the same instance waits again: the name stays the driver's
        }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
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
            HANDLE events[] = {read.hEvent, m_StopEvent};
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, WATCHDOG_MS);
            if (wait != WAIT_OBJECT_0) {
                CancelIo(pipe);
                GetOverlappedResult(pipe, &read, &got, TRUE);  // the cancelled read done before the instance is reused
                ended = wait == WAIT_TIMEOUT ? "silent for the watchdog's delay" : "the driver stops";
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
    // Whatever this connection made goes with it; the instance waits for the next client.
    m_Driver->RemoveConnection(connection);
    DisconnectNamedPipe(pipe);
}
