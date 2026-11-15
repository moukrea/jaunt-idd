// jaunt's indirect display driver: each monitor's EDID and its mode list (pure: no Windows API,
// tested on its own by tests/edid_test.cpp).
//
// A monitor is announced with a synthetic EDID 1.4 base block (128 bytes, no extension): the
// manufacturer "JNT", the monitor name "jaunt", a serial that tells the monitors present at once
// apart, the screen's size at 96 pixels an inch, sRGB, and one detailed timing, the preferred one:
// the requested mode, with reduced blanking picked so that its pixel clock gives exactly the
// requested refresh. A detailed timing holds at most 4095 pixels a side and a 655.35 MHz pixel clock;
// a mode beyond that is described by the nearest one it can hold (each side at most 4095, the refresh
// lowered until the clock fits), and the requested mode stays the preferred one of the monitor's mode
// list, before the described one.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jaunt_idd {

constexpr size_t EDID_SIZE = 128;
using Edid = std::array<uint8_t, EDID_SIZE>;

// A detailed timing descriptor's limits.
constexpr uint32_t DTD_MAX_SIDE = 4095;
constexpr uint32_t DTD_MAX_CLOCK = 65535;  // in 10 kHz units: 655.35 MHz

// A mode of the monitor: its active size in pixels and its refresh in Hz.
struct Mode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh = 0;
    bool described = false;  // the mode of the EDID's detailed timing
};

// A detailed timing: the active size and refresh, its blanking and the pixel clock (10 kHz units),
// with (width + hBlank) * (height + vBlank) * refresh == clock * 10000 exactly.
struct Timing {
    uint32_t width = 0, height = 0, refresh = 0;
    uint32_t hBlank = 0, vBlank = 0;
    uint32_t hFront = 48, hSync = 32, vFront = 3, vSync = 6;
    uint32_t clock = 0;
};

// The detailed timing of exactly that mode; false when a descriptor cannot hold it.
bool TimingFor(uint32_t width, uint32_t height, uint32_t refresh, Timing& out);
// The detailed timing that describes that mode: its own, else the nearest one a descriptor holds.
Timing DescribedTiming(uint32_t width, uint32_t height, uint32_t refresh);
// The monitor's EDID for that mode, with that serial.
Edid MakeEdid(uint32_t width, uint32_t height, uint32_t refresh, uint32_t serial);
// The monitor's modes: the requested one first (the preferred one), then the EDID's own when it differs.
std::vector<Mode> ModeList(uint32_t width, uint32_t height, uint32_t refresh);
// The serial of the monitor on that connector (0 to 7) for that mode: never 0, different for two
// connectors, the same for the same mode on the same connector (Windows keeps a display's settings by
// its EDID's identity, so a display of the same size comes back as it was left).
uint32_t SerialFor(uint32_t connector, uint32_t width, uint32_t height, uint32_t refresh);
// An EDID this driver made (its header, checksum and manufacturer): its serial and the mode of its
// detailed timing (refresh rounded to the nearest Hz). False for anything else.
bool ReadEdid(const uint8_t* data, size_t size, uint32_t& serial, Mode& described);

}  // namespace jaunt_idd
