// jaunt's indirect display driver: each monitor's EDID and its mode list (see edid.h).
// SPDX-License-Identifier: MIT
#include "edid.h"

#include <algorithm>
#include <initializer_list>

namespace jaunt_idd {

namespace {

// "JNT": three letters of five bits each ('A' is 1), big-endian.
constexpr uint8_t MANUFACTURER_HIGH = 0x29;
constexpr uint8_t MANUFACTURER_LOW = 0xD4;
constexpr uint16_t PRODUCT = 0x0001;
constexpr uint8_t HEADER[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
// sRGB's primaries and white point (red 0.640/0.330, green 0.300/0.600, blue 0.150/0.060, white
// 0.3127/0.3290), in the base block's 10-bit layout.
constexpr uint8_t SRGB[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};

uint8_t Byte(uint32_t value) { return static_cast<uint8_t>(value & 0xFF); }

// The screen's size at 96 pixels an inch: in centimeters for the base block (1 to 255), in
// millimeters for the detailed timing (at most 4095).
uint8_t Centimeters(uint32_t pixels) { return static_cast<uint8_t>(std::min<uint32_t>(std::max<uint32_t>((pixels * 254 + 4800) / 9600, 1), 255)); }
uint32_t Millimeters(uint32_t pixels) { return std::min<uint32_t>((pixels * 254 + 480) / 960, 4095); }

void DetailedTiming(uint8_t* d, const Timing& t, uint32_t widthMm, uint32_t heightMm) {
    d[0] = Byte(t.clock);
    d[1] = Byte(t.clock >> 8);
    d[2] = Byte(t.width);
    d[3] = Byte(t.hBlank);
    d[4] = static_cast<uint8_t>((((t.width >> 8) & 0xF) << 4) | ((t.hBlank >> 8) & 0xF));
    d[5] = Byte(t.height);
    d[6] = Byte(t.vBlank);
    d[7] = static_cast<uint8_t>((((t.height >> 8) & 0xF) << 4) | ((t.vBlank >> 8) & 0xF));
    d[8] = Byte(t.hFront);
    d[9] = Byte(t.hSync);
    d[10] = static_cast<uint8_t>(((t.vFront & 0xF) << 4) | (t.vSync & 0xF));
    d[11] = static_cast<uint8_t>((((t.hFront >> 8) & 3) << 6) | (((t.hSync >> 8) & 3) << 4) | (((t.vFront >> 4) & 3) << 2) | ((t.vSync >> 4) & 3));
    d[12] = Byte(widthMm);
    d[13] = Byte(heightMm);
    d[14] = static_cast<uint8_t>((((widthMm >> 8) & 0xF) << 4) | ((heightMm >> 8) & 0xF));
    d[15] = 0;
    d[16] = 0;
    d[17] = 0x1A;  // digital separate sync, horizontal sync positive, vertical negative (as CVT's reduced blanking)
}

// A display descriptor (not a timing): its tag and 13 bytes of text, ended by a line feed and padded
// with spaces.
void TextDescriptor(uint8_t* d, uint8_t tag, const char* text) {
    d[3] = tag;
    size_t i = 0;
    for (; text[i] && i < 13; ++i) {
        d[5 + i] = static_cast<uint8_t>(text[i]);
    }
    if (i < 13) {
        d[5 + i++] = 0x0A;
    }
    for (; i < 13; ++i) {
        d[5 + i] = 0x20;
    }
}

}  // namespace

bool TimingFor(uint32_t width, uint32_t height, uint32_t refresh, Timing& out) {
    if (width == 0 || height == 0 || refresh == 0 || refresh > 1000 || width > DTD_MAX_SIDE || height > DTD_MAX_SIDE) {
        return false;
    }
    // Vertical blanking of at least 460 us (CVT's reduced blanking) and 15 lines (front porch 3,
    // sync 6, back porch 6 or more); horizontal blanking from 160 pixels (front porch 48, sync 32).
    // Among those, the smallest total for which the pixel clock, in 10 kHz units, gives exactly this
    // refresh: in any 400 widths in a row one is a multiple of 400, and in any 25 heights one is a
    // multiple of 25, so one is always found below the clock's limit if any is.
    const uint64_t num = 460ull * refresh * height;
    const uint64_t den = 1000000ull - 460ull * refresh;
    uint32_t vMin = static_cast<uint32_t>((num + den - 1) / den);
    vMin = std::max<uint32_t>(vMin, 15);
    bool found = false;
    uint64_t best = 0;
    for (uint32_t hBlank = 160; hBlank < 560; ++hBlank) {
        for (uint32_t vBlank = vMin; vBlank < vMin + 25 && vBlank <= DTD_MAX_SIDE; ++vBlank) {
            const uint64_t total = static_cast<uint64_t>(width + hBlank) * (height + vBlank) * refresh;
            if (total % 10000 != 0 || total / 10000 > DTD_MAX_CLOCK || (found && total >= best)) {
                continue;
            }
            found = true;
            best = total;
            out = Timing{};
            out.width = width;
            out.height = height;
            out.refresh = refresh;
            out.hBlank = hBlank;
            out.vBlank = vBlank;
            out.clock = static_cast<uint32_t>(total / 10000);
        }
    }
    return found;
}

Timing DescribedTiming(uint32_t width, uint32_t height, uint32_t refresh) {
    Timing t;
    if (TimingFor(width, height, refresh, t)) {
        return t;
    }
    // The nearest a descriptor holds: each side at most 4095, then the refresh lowered until the
    // clock fits (4095x4095 fits at 24 Hz, the lowest refresh asked).
    const uint32_t w = std::min(width, DTD_MAX_SIDE);
    const uint32_t h = std::min(height, DTD_MAX_SIDE);
    for (uint32_t r = refresh; r >= 1; --r) {
        if (TimingFor(w, h, r, t)) {
            return t;
        }
    }
    TimingFor(640, 480, 60, t);  // a mode out of any range asked for
    return t;
}

Edid MakeEdid(uint32_t width, uint32_t height, uint32_t refresh, uint32_t serial) {
    Edid e{};
    std::copy(HEADER, HEADER + 8, e.begin());
    e[8] = MANUFACTURER_HIGH;
    e[9] = MANUFACTURER_LOW;
    e[10] = Byte(PRODUCT);
    e[11] = Byte(PRODUCT >> 8);
    for (int i = 0; i < 4; ++i) {
        e[12 + i] = Byte(serial >> (8 * i));
    }
    e[16] = 0;   // week of manufacture: not said
    e[17] = 36;  // 2026
    e[18] = 1;   // EDID 1.4
    e[19] = 4;
    e[20] = 0xA0;  // digital, 8 bits a color, interface not said
    e[21] = Centimeters(width);
    e[22] = Centimeters(height);
    e[23] = 120;   // gamma 2.2
    e[24] = 0x06;  // RGB 4:4:4; sRGB the default color space; the preferred timing is native; not continuous
    std::copy(SRGB, SRGB + 10, e.begin() + 25);
    // 35 to 37: no established timing. 38 to 53: no standard timing.
    for (size_t i = 38; i < 54; ++i) {
        e[i] = 0x01;
    }
    DetailedTiming(&e[54], DescribedTiming(width, height, refresh), Millimeters(width), Millimeters(height));
    TextDescriptor(&e[72], 0xFC, "jaunt");  // the monitor's name
    char text[9];
    for (int i = 0; i < 8; ++i) {
        text[i] = "0123456789ABCDEF"[(serial >> (28 - 4 * i)) & 0xF];
    }
    text[8] = '\0';
    TextDescriptor(&e[90], 0xFF, text);  // its serial, as text
    e[108 + 3] = 0x10;                   // a descriptor left empty
    e[126] = 0;                          // no extension block
    uint32_t sum = 0;
    for (size_t i = 0; i < 127; ++i) {
        sum += e[i];
    }
    e[127] = Byte(256 - (sum & 0xFF));
    return e;
}

std::vector<Mode> ModeList(uint32_t width, uint32_t height, uint32_t refresh) {
    const Timing t = DescribedTiming(width, height, refresh);
    const bool same = t.width == width && t.height == height && t.refresh == refresh;
    std::vector<Mode> modes;
    modes.push_back(Mode{width, height, refresh, same});
    if (!same) {
        modes.push_back(Mode{t.width, t.height, t.refresh, true});
    }
    return modes;
}

uint32_t SerialFor(uint32_t connector, uint32_t width, uint32_t height, uint32_t refresh) {
    uint32_t hash = 2166136261u;  // FNV-1a over the mode
    for (uint32_t value : {width, height, refresh}) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (value >> (8 * i)) & 0xFF;
            hash *= 16777619u;
        }
    }
    const uint32_t serial = (hash & ~7u) | (connector & 7u);
    return serial ? serial : 8u;
}

bool ReadEdid(const uint8_t* data, size_t size, uint32_t& serial, Mode& described) {
    if (data == nullptr || size != EDID_SIZE || !std::equal(HEADER, HEADER + 8, data)) {
        return false;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < EDID_SIZE; ++i) {
        sum += data[i];
    }
    if ((sum & 0xFF) != 0 || data[8] != MANUFACTURER_HIGH || data[9] != MANUFACTURER_LOW) {
        return false;
    }
    serial = static_cast<uint32_t>(data[12]) | (static_cast<uint32_t>(data[13]) << 8) | (static_cast<uint32_t>(data[14]) << 16) |
             (static_cast<uint32_t>(data[15]) << 24);
    const uint8_t* d = data + 54;
    const uint32_t clock = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8);
    if (clock == 0) {
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(d[2]) | (static_cast<uint32_t>(d[4] >> 4) << 8);
    const uint32_t hBlank = static_cast<uint32_t>(d[3]) | (static_cast<uint32_t>(d[4] & 0xF) << 8);
    const uint32_t height = static_cast<uint32_t>(d[5]) | (static_cast<uint32_t>(d[7] >> 4) << 8);
    const uint32_t vBlank = static_cast<uint32_t>(d[6]) | (static_cast<uint32_t>(d[7] & 0xF) << 8);
    const uint64_t total = static_cast<uint64_t>(width + hBlank) * (height + vBlank);
    if (width == 0 || height == 0) {
        return false;
    }
    described = Mode{width, height, static_cast<uint32_t>((static_cast<uint64_t>(clock) * 10000 + total / 2) / total), true};
    return true;
}

}  // namespace jaunt_idd
