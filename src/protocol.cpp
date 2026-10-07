// jaunt's indirect display driver: the control pipe's protocol (see protocol.h).
// SPDX-License-Identifier: MIT
#include "protocol.h"

#include <sstream>
#include <vector>

namespace jaunt_idd {

namespace {

// A number of 1 to 5 digits, nothing else.
bool Number(const std::string& text, uint32_t& out) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    out = value;
    return true;
}

Command Invalid(const std::string& why) {
    Command c;
    c.kind = Kind::Invalid;
    c.error = why;
    return c;
}

}  // namespace

Command Parse(const std::string& message) {
    if (message.size() > MAX_MESSAGE) {
        return Invalid("message too long");
    }
    std::string text = message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    for (char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            return Invalid("unexpected character");
        }
    }
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string w; in >> w;) {
        words.push_back(w);
    }
    if (words.empty()) {
        return Invalid("empty message");
    }
    Command c;
    if (words[0] == "ping" && words.size() == 1) {
        c.kind = Kind::Ping;
        return c;
    }
    if (words[0] == "remove" && words.size() == 2) {
        if (!Number(words[1], c.id) || c.id == 0) {
            return Invalid("a monitor id");
        }
        c.kind = Kind::Remove;
        return c;
    }
    if (words[0] == "add" && words.size() == 4) {
        if (!Number(words[1], c.width) || !Number(words[2], c.height) || !Number(words[3], c.refresh)) {
            return Invalid("add <width> <height> <refresh>");
        }
        if (c.width < MIN_SIDE || c.width > MAX_SIDE || c.height < MIN_SIDE || c.height > MAX_SIDE) {
            return Invalid("a size of 320 to 8192 pixels a side");
        }
        if (c.refresh < MIN_REFRESH || c.refresh > MAX_REFRESH) {
            return Invalid("a refresh rate of 24 to 240 Hz");
        }
        c.kind = Kind::Add;
        return c;
    }
    return Invalid("unknown request");
}

std::string Ok(uint32_t id) { return "ok " + std::to_string(id) + "\n"; }
std::string Ok() { return "ok\n"; }
std::string Pong() { return "pong\n"; }
std::string Error(const std::string& why) { return "error " + why + "\n"; }

}  // namespace jaunt_idd
