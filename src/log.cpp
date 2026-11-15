// jaunt's indirect display driver: the bounded log (see log.h).
// SPDX-License-Identifier: MIT
#include "log.h"

#include <cstdio>
#include <cstring>

namespace jaunt_idd {

EventLog::EventLog() : m_Start(std::chrono::steady_clock::now()) {}

void EventLog::Add(const char* format, ...) {
    char text[LOG_MAX_LINE + 1];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n < 0) {
        n = 0;
    }
    size_t length = static_cast<size_t>(n) < LOG_MAX_LINE ? static_cast<size_t>(n) : LOG_MAX_LINE;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7e) {
            text[i] = '?';
        }
    }
    text[length] = '\0';
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Start).count();
    std::lock_guard<std::mutex> lock(m_Lock);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "+%lld ", static_cast<long long>(ms));
    m_Lines.push_back(std::string(stamp) + text);
    if (m_Lines.size() > LOG_MAX_LINES) {
        m_Lines.pop_front();
        ++m_First;
    }
}

uint32_t EventLog::Next() {
    std::lock_guard<std::mutex> lock(m_Lock);
    return m_First + static_cast<uint32_t>(m_Lines.size());
}

std::string EventLog::Read(uint32_t from, size_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_Lock);
    const uint32_t end = m_First + static_cast<uint32_t>(m_Lines.size());
    uint32_t at = from < m_First ? m_First : from;
    std::string body;
    // The header is "ok next=<n> first=<n>\n": at most 40 bytes.
    while (at < end) {
        const std::string& line = m_Lines[at - m_First];
        char number[16];
        snprintf(number, sizeof(number), "%u ", static_cast<unsigned>(at));
        const size_t need = strlen(number) + line.size() + 1;
        if (40 + body.size() + need > maxBytes) {
            break;
        }
        body += number;
        body += line;
        body += '\n';
        ++at;
    }
    char header[64];
    snprintf(header, sizeof(header), "ok next=%u first=%u\n", static_cast<unsigned>(at), static_cast<unsigned>(m_First));
    return std::string(header) + body;
}

}  // namespace jaunt_idd
