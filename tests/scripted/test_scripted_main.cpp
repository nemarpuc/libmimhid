// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Runs the scripted-board suites: the serial port, the link and the handshake below the C ABI,
 * and the C ABI against a board that NAKs, stays silent, stops reading or is unplugged. Each test
 * starts its own board on a new pseudo-terminal. */
#include "scripted_support.hpp"

#include <cstring>

int g_checks = 0;
int g_failures = 0;

namespace {
struct Suite {
    const char* name;
    void (*run)();
};

constexpr Suite kSuites[] = {
    {"serial_port", test_serial_port},   {"link", test_link},
    {"handshaker", test_handshaker},     {"ports_linux", test_ports_linux},
    {"api_open", test_api_open},         {"api_commands", test_api_commands},
    {"api_reports", test_api_reports},   {"api_events", test_api_events},
    {"api_raw_node", test_api_raw_node}, {"api_generic", test_api_generic},
};
} // namespace

// With no arguments every suite runs; otherwise only the named ones. --list prints the names.
int main(const int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Suite& suite : kSuites)
            std::printf("%s\n", suite.name);
        return 0;
    }
    for (int index = 1; index < argc; ++index) {
        bool known = false;
        for (const Suite& suite : kSuites)
            known = known || std::strcmp(argv[index], suite.name) == 0;
        if (!known) {
            std::fprintf(stderr, "unknown suite: %s\n", argv[index]);
            return 2;
        }
    }
    for (const Suite& suite : kSuites) {
        bool selected = argc < 2;
        for (int index = 1; index < argc; ++index)
            selected = selected || std::strcmp(argv[index], suite.name) == 0;
        if (selected)
            suite.run();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
