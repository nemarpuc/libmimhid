// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Provides the minimal assertion surface shared by deterministic native tests;
 * it is test-only and never enters the library ABI. */
#pragma once

#include <cstdio>

// Every runner defines both: g_checks counts the checks that ran, g_failures the ones that failed.
extern int g_checks;
extern int g_failures;

#define MIMUSB_CHECK(expression)                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression);    \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

// The unit suites, one per file of tests/unit (test_<suite>.cpp); test_main.cpp lists them.
void test_item_writer();
void test_descriptor_builder();
void test_validator_parser_state();
void test_validator_arithmetic();
void test_validator_encoding();
void test_frame();
void test_descriptor_composer();
void test_descriptor_walker();
void test_profiles_common();
void test_profiles_keyboard();
void test_profiles_mouse();
void test_profiles_usage_controls();
void test_profiles_gamepad();
void test_profiles_touch();
void test_profiles_pen();
void test_profiles_raw();
void test_profiles_battery();
void test_error_detail();
void test_api_arguments();
void test_spec_rejections();
void test_input_rejections();
