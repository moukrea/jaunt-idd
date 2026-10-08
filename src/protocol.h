// jaunt's indirect display driver: the control pipe's protocol (pure: no Windows API, tested on its
// own by tests/protocol_test.cpp).
//
// One request per message (UTF-8 text, a trailing newline optional), one answer each:
//   add <width> <height> <refresh>   ->  ok <id>        a monitor of that mode (pixels, Hz)
//   remove <id>                      ->  ok             that monitor gone
//   ping                             ->  pong           the watchdog fed
//   status                           ->  ok <name>=<value> ...   what Windows asked of the driver
// else "error <why>". Widths and heights are 320 to 8192, refresh rates 24 to 240. A connection's
// monitors go when it closes or sends nothing for WATCHDOG_MS (its process stopped or hung).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace jaunt_idd {

constexpr uint32_t MIN_SIDE = 320;
constexpr uint32_t MAX_SIDE = 8192;
constexpr uint32_t MIN_REFRESH = 24;
constexpr uint32_t MAX_REFRESH = 240;
constexpr uint32_t WATCHDOG_MS = 5000;
// Monitors at once: the adapter's connectors 0 to MAX_MONITORS - 1, each reused once free.
constexpr uint32_t MAX_MONITORS = 8;
constexpr size_t MAX_MESSAGE = 256;

enum class Kind { Add, Remove, Ping, Status, Invalid };

struct Command {
    Kind kind = Kind::Invalid;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh = 0;
    uint32_t id = 0;
    std::string error;
};

// One message from the pipe; never throws.
Command Parse(const std::string& message);

std::string Ok(uint32_t id);
std::string Ok();
std::string Pong();
std::string Error(const std::string& why);

}  // namespace jaunt_idd
