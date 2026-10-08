// The control pipe's protocol (src/protocol.cpp), on its own: any compiler, no Windows API.
//   cl /std:c++17 /EHsc /I..\src protocol_test.cpp ..\src\protocol.cpp   (or g++ -std=c++17)
// SPDX-License-Identifier: MIT
#include "protocol.h"

#include <cstdio>
#include <cstdlib>

using namespace jaunt_idd;

static int failures = 0;

static void Check(bool good, const char* what) {
    std::printf("%s %s\n", good ? "PASS" : "FAIL", what);
    if (!good) {
        ++failures;
    }
}

int main() {
    Command c = Parse("add 1170 2532 60\n");
    Check(c.kind == Kind::Add && c.width == 1170 && c.height == 2532 && c.refresh == 60, "add with its mode");
    Check(Parse("add 8192 320 240").kind == Kind::Add, "the largest and smallest sides, the fastest rate");
    Check(Parse("add 100 100 60").kind == Kind::Invalid, "a side under 320 refused");
    Check(Parse("add 9000 1080 60").kind == Kind::Invalid, "a side over 8192 refused");
    Check(Parse("add 1920 1080 500").kind == Kind::Invalid, "a rate over 240 refused");
    Check(Parse("add 1920 1080").kind == Kind::Invalid, "a rate missing");
    Check(Parse("add -1920 1080 60").kind == Kind::Invalid, "a sign refused");
    Check(Parse("add 1920 1080 60 extra").kind == Kind::Invalid, "a word too many");
    Check(Parse("add 99999999 1080 60").kind == Kind::Invalid, "a number too long");
    c = Parse("remove 3\r\n");
    Check(c.kind == Kind::Remove && c.id == 3, "remove by id");
    Check(Parse("remove 0").kind == Kind::Invalid, "id 0 refused");
    Check(Parse("ping").kind == Kind::Ping, "ping");
    Check(Parse("status\n").kind == Kind::Status, "status");
    Check(Parse("status now").kind == Kind::Invalid, "status takes nothing");
    Check(Parse("").kind == Kind::Invalid, "empty");
    Check(Parse("add 1920\x01 1080 60").kind == Kind::Invalid, "a control character refused");
    Check(Parse(std::string(300, 'a')).kind == Kind::Invalid, "too long refused");
    Check(Parse("format c:").kind == Kind::Invalid, "anything else refused");
    Check(Ok(7) == "ok 7\n" && Ok() == "ok\n" && Pong() == "pong\n" && Error("x") == "error x\n", "answers");
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
