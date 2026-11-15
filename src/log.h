// jaunt's indirect display driver: a bounded log of what Windows asked of the driver and what each
// Windows call answered (pure: no Windows API, tested on its own by tests/edid_test.cpp).
//
// Text only, one line per event: "<sequence> +<milliseconds since the driver started> <text>". The
// newest MAX_LINES lines are kept, each cut at MAX_LINE characters and reduced to printable ASCII.
// Only ids, sizes, statuses and LUIDs go in: no names, no paths, no secrets. The pipe's `log` request
// returns it (protocol.h), the oldest line first.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace jaunt_idd {

constexpr size_t LOG_MAX_LINES = 600;
constexpr size_t LOG_MAX_LINE = 220;

class EventLog {
public:
    EventLog();

    // Appends one line (printf's format; at most LOG_MAX_LINE characters kept).
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void Add(const char* format, ...);

    // The lines from sequence `from` on (those the bound already dropped are skipped), the oldest
    // first, as many as fit in `maxBytes`: "ok next=<sequence of the next line to ask for>
    // first=<sequence of the oldest kept line>\n" then the lines. Asking from `next` again gives the
    // lines added since; nothing new gives the header alone.
    std::string Read(uint32_t from, size_t maxBytes);

    // The sequence the next line will get.
    uint32_t Next();

private:
    std::mutex m_Lock;
    std::deque<std::string> m_Lines;  // m_Lines[i] has sequence m_First + i
    uint32_t m_First = 1;
    std::chrono::steady_clock::time_point m_Start;
};

}  // namespace jaunt_idd
