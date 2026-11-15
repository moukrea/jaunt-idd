// The monitors' EDID and the bounded log (src/edid.cpp, src/log.cpp), on their own: any compiler,
// no Windows API.
//   cl /std:c++17 /EHsc /I..\src edid_test.cpp ..\src\edid.cpp ..\src\log.cpp   (or g++ -std=c++17)
// SPDX-License-Identifier: MIT
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS  // sscanf, in a test
#endif
#include "edid.h"
#include "log.h"
#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace jaunt_idd;

static int failures = 0;

static void Check(bool good, const char* what) {
    std::printf("%s %s\n", good ? "PASS" : "FAIL", what);
    if (!good) {
        ++failures;
    }
}

// The base block read as a monitor reads it, independently of how it was written: the detailed
// timing's active size, its total size and its pixel clock.
struct Decoded {
    uint32_t width, height, hTotal, vTotal, clockKHz10;
    uint32_t widthMm, heightMm;
    uint32_t hFront, hSync, vFront, vSync;
};
static Decoded Decode(const Edid& e) {
    const uint8_t* d = &e[54];
    Decoded x{};
    x.clockKHz10 = d[0] | (d[1] << 8);
    x.width = d[2] | ((d[4] >> 4) << 8);
    x.hTotal = x.width + (d[3] | ((d[4] & 0xF) << 8));
    x.height = d[5] | ((d[7] >> 4) << 8);
    x.vTotal = x.height + (d[6] | ((d[7] & 0xF) << 8));
    x.hFront = d[8] | (((d[11] >> 6) & 3) << 8);
    x.hSync = d[9] | (((d[11] >> 4) & 3) << 8);
    x.vFront = (d[10] >> 4) | (((d[11] >> 2) & 3) << 4);
    x.vSync = (d[10] & 0xF) | ((d[11] & 3) << 4);
    x.widthMm = d[12] | ((d[14] >> 4) << 8);
    x.heightMm = d[13] | ((d[14] & 0xF) << 8);
    return x;
}

static bool Sums(const Edid& e) {
    unsigned sum = 0;
    for (uint8_t b : e) {
        sum += b;
    }
    return (sum & 0xFF) == 0;
}

static std::string Text(const Edid& e, size_t at) {
    std::string s;
    for (size_t i = 0; i < 13 && e[at + 5 + i] != 0x0A; ++i) {
        s.push_back(static_cast<char>(e[at + 5 + i]));
    }
    return s;
}

int main() {
    // ---- the base block --------------------------------------------------------------------------
    Edid e = MakeEdid(1170, 2532, 60, 0x1234ABCD);
    const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    Check(std::memcmp(e.data(), header, 8) == 0, "the 8-byte EDID header");
    Check(Sums(e), "the 128 bytes add up to 0 modulo 256");
    Check(e[18] == 1 && e[19] == 4 && e[126] == 0, "EDID 1.4, no extension block");
    Check((e[20] & 0x80) != 0, "a digital input");
    Check(e[12] == 0xCD && e[13] == 0xAB && e[14] == 0x34 && e[15] == 0x12, "the serial, little-endian");
    // Manufacturer ID "JNT": J=10, N=14, T=20, five bits each.
    const unsigned id = (e[8] << 8) | e[9];
    Check(((id >> 10) & 31) == 10 && ((id >> 5) & 31) == 14 && (id & 31) == 20 && (e[8] & 0x80) == 0, "the manufacturer JNT");
    Check(Text(e, 72) == "jaunt" && e[75] == 0xFC && e[72] == 0 && e[73] == 0 && e[74] == 0, "the monitor name descriptor, \"jaunt\"");
    Check(e[93] == 0xFF && Text(e, 90) == "1234ABCD", "the serial number descriptor");
    Check(e[111] == 0x10, "the fourth descriptor empty");
    Decoded x = Decode(e);
    Check(x.width == 1170 && x.height == 2532, "the detailed timing's active size is the requested one");
    Check(static_cast<uint64_t>(x.hTotal) * x.vTotal * 60 == static_cast<uint64_t>(x.clockKHz10) * 10000, "its pixel clock gives exactly 60 Hz");
    Check(x.hTotal - x.width >= 160 && x.vTotal - x.height >= 15, "with blanking for its sync and porches");
    Check(x.hFront + x.hSync < x.hTotal - x.width && x.vFront + x.vSync < x.vTotal - x.height, "sync and front porch inside the blanking");
    Check(x.widthMm >= 290 && x.widthMm <= 320 && x.heightMm >= 640 && x.heightMm <= 680, "its size in millimeters at 96 pixels an inch");
    Check(e[21] == 31 && e[22] == 67, "and in centimeters");

    // ---- exact timings over the whole range ------------------------------------------------------
    bool exact = true;
    bool valid = true;
    int checked = 0;
    const uint32_t sides[] = {320, 321, 480, 640, 720, 768, 800, 900, 1024, 1080, 1170, 1280, 1366, 1440, 1600, 1920, 2160, 2532, 2560, 3000, 3840, 4000, 4095};
    const uint32_t rates[] = {24, 25, 30, 48, 50, 59, 60, 72, 75, 90, 100, 120, 144, 165, 240};
    for (uint32_t w : sides) {
        for (uint32_t h : sides) {
            for (uint32_t r : rates) {
                Timing t;
                if (TimingFor(w, h, r, t)) {
                    ++checked;
                    exact = exact && static_cast<uint64_t>(t.width + t.hBlank) * (t.height + t.vBlank) * r == static_cast<uint64_t>(t.clock) * 10000;
                    valid = valid && t.clock <= DTD_MAX_CLOCK && t.hBlank <= 4095 && t.vBlank <= 4095 && t.hBlank >= 160 && t.vBlank >= 15 && t.width == w &&
                            t.height == h;
                    // Reduced blanking: the vertical blanking lasts at least 460 microseconds.
                    valid = valid && static_cast<uint64_t>(t.vBlank) * 1000000ull >= 460ull * r * (t.height + t.vBlank);
                }
            }
        }
    }
    Check(checked > 3000, "most modes up to 4095 a side have a detailed timing of their own");
    Check(exact, "each detailed timing's pixel clock gives exactly its refresh rate");
    Check(valid, "each is within a descriptor's limits, with 460 microseconds of vertical blanking");
    Timing t;
    Check(TimingFor(1920, 1080, 60, t) && t.clock <= 20000, "1920x1080 at 60 Hz fits 200 MHz");
    Check(!TimingFor(4096, 1080, 60, t) && !TimingFor(1080, 4096, 60, t) && !TimingFor(0, 1080, 60, t), "a side over 4095 or zero has none");
    Check(!TimingFor(4095, 4095, 60, t), "4095x4095 at 60 Hz exceeds the pixel clock a descriptor holds");

    // ---- a mode a descriptor cannot hold: the nearest one ----------------------------------------
    Timing nearest = DescribedTiming(8192, 4320, 60);
    Check(nearest.width == 4095 && nearest.height == 4095 && nearest.refresh >= 24 && nearest.refresh < 60,
          "8192x4320 at 60 Hz is described by 4095x4095 at a lower rate");
    Check(static_cast<uint64_t>(nearest.width + nearest.hBlank) * (nearest.height + nearest.vBlank) * nearest.refresh == static_cast<uint64_t>(nearest.clock) * 10000,
          "which is exact too");
    Check(DescribedTiming(1170, 2532, 60).refresh == 60, "a mode that fits is described as it is");
    bool allHold = true;
    for (uint32_t w : {320u, 1170u, 4095u, 4096u, 8192u}) {
        for (uint32_t h : {320u, 2532u, 4095u, 4096u, 8192u}) {
            for (uint32_t r : {24u, 60u, 144u, 240u}) {
                Timing d = DescribedTiming(w, h, r);
                allHold = allHold && d.clock > 0 && d.clock <= DTD_MAX_CLOCK && d.width <= DTD_MAX_SIDE && d.height <= DTD_MAX_SIDE &&
                          static_cast<uint64_t>(d.width + d.hBlank) * (d.height + d.vBlank) * d.refresh == static_cast<uint64_t>(d.clock) * 10000;
                Edid big = MakeEdid(w, h, r, 8);
                allHold = allHold && Sums(big);
            }
        }
    }
    Check(allHold, "every size the pipe accepts (320 to 8192, 24 to 240 Hz) has an EDID whose timing is valid and exact");

    // ---- the mode list --------------------------------------------------------------------------
    std::vector<Mode> modes = ModeList(1170, 2532, 60);
    Check(modes.size() == 1 && modes[0].width == 1170 && modes[0].height == 2532 && modes[0].refresh == 60 && modes[0].described,
          "a mode the EDID states: one mode, the EDID's");
    modes = ModeList(8192, 4320, 60);
    Check(modes.size() == 2 && modes[0].width == 8192 && modes[0].height == 4320 && !modes[0].described && modes[1].described && modes[1].width == 4095,
          "a mode it cannot state: the requested one first, then the EDID's");

    // ---- reading it back (what the parse callback does) ------------------------------------------
    uint32_t serial = 0;
    Mode read;
    Check(ReadEdid(e.data(), e.size(), serial, read) && serial == 0x1234ABCD && read.width == 1170 && read.height == 2532 && read.refresh == 60,
          "an EDID of ours is read back: its serial and its mode");
    for (uint32_t r : {24u, 30u, 59u, 60u, 75u, 120u, 144u, 240u}) {
        Edid f = MakeEdid(1280, 720, r, 16);
        Check(ReadEdid(f.data(), f.size(), serial, read) && read.refresh == r, "the refresh rate comes back exactly");
    }
    Edid bad = e;
    bad[40] ^= 1;
    Check(!ReadEdid(bad.data(), bad.size(), serial, read), "a block whose checksum fails is not ours");
    bad = e;
    bad[8] = 0x10;
    bad[127] = static_cast<uint8_t>(bad[127] + (e[8] - 0x10));
    Check(!ReadEdid(bad.data(), bad.size(), serial, read), "another manufacturer's is not ours");
    Check(!ReadEdid(e.data(), 127, serial, read) && !ReadEdid(nullptr, EDID_SIZE, serial, read), "a wrong size, or none, is not ours");

    // ---- serials --------------------------------------------------------------------------------
    std::set<uint32_t> serials;
    bool nonzero = true;
    for (uint32_t c = 0; c < MAX_MONITORS; ++c) {
        const uint32_t s = SerialFor(c, 640, 480, 60);
        serials.insert(s);
        nonzero = nonzero && s != 0;
    }
    Check(serials.size() == MAX_MONITORS && nonzero, "eight monitors of one mode at once have eight different serials");
    Check(SerialFor(3, 1170, 2532, 60) == SerialFor(3, 1170, 2532, 60), "the same mode on the same connector has the same serial");
    Check(SerialFor(0, 1170, 2532, 60) != SerialFor(0, 1280, 720, 60), "another mode, another serial");
    Check(MakeEdid(640, 480, 60, SerialFor(0, 640, 480, 60)) != MakeEdid(640, 480, 60, SerialFor(1, 640, 480, 60)), "so two EDIDs differ");

    // ---- the log --------------------------------------------------------------------------------
    EventLog log;
    Check(log.Read(0, 3800) == "ok next=1 first=1\n", "an empty log: its header alone");
    log.Add("first %d", 1);
    log.Add("second 0x%08X", 0xC0000001u);
    std::string text = log.Read(0, 3800);
    Check(text.rfind("ok next=3 first=1\n1 +", 0) == 0 && text.find(" first 1\n2 +") != std::string::npos && text.find("second 0xC0000001\n") != std::string::npos,
          "lines are numbered and stamped, the oldest first");
    Check(log.Read(3, 3800) == "ok next=3 first=1\n", "asking from next gives nothing new");
    log.Add("third");
    text = log.Read(3, 3800);
    Check(text.rfind("ok next=4 first=1\n3 +", 0) == 0 && text.find("third\n") != std::string::npos, "and then only what was added");
    log.Add("tab\there \x01 and \xC3\xA9");
    text = log.Read(4, 3800);
    bool printable = true;
    for (size_t i = text.find('\n') + 1; i < text.size(); ++i) {
        printable = printable && (text[i] == '\n' || (static_cast<unsigned char>(text[i]) >= 0x20 && static_cast<unsigned char>(text[i]) <= 0x7e));
    }
    Check(printable && text.find("tab?here ? and ??") != std::string::npos, "text is printable ASCII: anything else becomes ?");
    EventLog many;
    for (size_t i = 0; i < LOG_MAX_LINES + 100; ++i) {
        many.Add("line %zu", i);
    }
    text = many.Read(0, 1000000);
    size_t lines = 0;
    for (char c : text) {
        lines += c == '\n';
    }
    Check(lines == LOG_MAX_LINES + 1 && text.rfind("ok next=701 first=101\n", 0) == 0, "the log keeps the newest 600 lines and says where it starts");
    EventLog longLine;
    longLine.Add("%s", std::string(2000, 'x').c_str());
    text = longLine.Read(0, 3800);
    Check(text.size() < 40 + LOG_MAX_LINE + 40, "a line is cut");
    // Paging: each answer within the bound, and the pages together are every line once.
    std::string pages;
    uint32_t from = 0;
    size_t calls = 0;
    bool within = true;
    for (;;) {
        std::string page = many.Read(from, MAX_LOG_ANSWER);
        within = within && page.size() <= MAX_LOG_ANSWER;
        unsigned next = 0, first = 0;
        std::sscanf(page.c_str(), "ok next=%u first=%u", &next, &first);
        ++calls;
        if (next == from || calls > 1000) {
            break;
        }
        pages += page.substr(page.find('\n') + 1);
        from = next;
    }
    size_t pagedLines = 0;
    for (char c : pages) {
        pagedLines += c == '\n';
    }
    Check(within && pagedLines == LOG_MAX_LINES && calls > 3, "paged by the answer's bound, every kept line comes once");

    // ---- the protocol's log request -------------------------------------------------------------
    Command c = Parse("log");
    Check(c.kind == Kind::Log && c.from == 0, "log");
    c = Parse("log 12345678\n");
    Check(c.kind == Kind::Log && c.from == 12345678, "log from a line");
    Check(Parse("log x").kind == Kind::Invalid && Parse("log 1 2").kind == Kind::Invalid && Parse("log 1234567890").kind == Kind::Invalid &&
              Parse("log -1").kind == Kind::Invalid,
          "log takes one number at most");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
