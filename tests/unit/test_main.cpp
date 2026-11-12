// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Runs the unit suites: the HID layer, the profile state machines and the frame parser.
 * Integration executables own serial port and threading coverage. */
#include "test.hpp"

#include <cstring>

int g_checks = 0;
int g_failures = 0;

namespace {
struct Suite {
    const char* name;
    void (*run)();
};

constexpr Suite kSuites[] = {
    {"item_writer", test_item_writer},
    {"descriptor_builder", test_descriptor_builder},
    {"validator_parser_state", test_validator_parser_state},
    {"validator_arithmetic", test_validator_arithmetic},
    {"validator_encoding", test_validator_encoding},
    {"frame", test_frame},
    {"descriptor_composer", test_descriptor_composer},
    {"descriptor_walker", test_descriptor_walker},
    {"profiles_common", test_profiles_common},
    {"profiles_keyboard", test_profiles_keyboard},
    {"profiles_mouse", test_profiles_mouse},
    {"profiles_usage_controls", test_profiles_usage_controls},
    {"profiles_gamepad", test_profiles_gamepad},
    {"profiles_touch", test_profiles_touch},
    {"profiles_pen", test_profiles_pen},
    {"profiles_raw", test_profiles_raw},
    {"profiles_battery", test_profiles_battery},
    {"error_detail", test_error_detail},
    {"api_arguments", test_api_arguments},
    {"spec_rejections", test_spec_rejections},
    {"input_rejections", test_input_rejections},
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
        for (const Suite& suite : kSuites) {
            known = known || std::strcmp(argv[index], suite.name) == 0;
        }
        if (!known) {
            std::fprintf(stderr, "unknown suite: %s\n", argv[index]);
            return 2;
        }
    }
    for (const Suite& suite : kSuites) {
        bool selected = argc < 2;
        for (int index = 1; index < argc; ++index) {
            selected = selected || std::strcmp(argv[index], suite.name) == 0;
        }
        if (selected) {
            suite.run();
        }
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
