// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* What the scripted-board tests share: a device configuration for the scripted board, an open
 * helper and the suites of this runner. Test-only. */
#pragma once

#include "fill_values.h"
#include "mimhid.h"
#include "scripted_board.hpp"
#include "test.hpp"

#include <cstdint>
#include <cstdio>

namespace scripted {

inline constexpr std::uint32_t kControlMs = 100;
inline constexpr std::uint32_t kSendMs = 100;
inline constexpr std::uint32_t kDrainMs = 200;
inline const mimhid_commit_timeouts kCommit = {sizeof(mimhid_commit_timeouts), 0, kControlMs, 1000};

// The test configuration on the boot rate, so that no rate is tested unless a test asks.
inline mimhid_device_config board_options() {
    mimhid_device_config o{};
    test_fill_device_config(&o);
    o.transport.baud_rate = MIMHID_BOOT_BAUD;
    o.transport.handshake_timeout_ms = 50;
    return o;
}

inline mimhid_device* open_on(Board& board, const mimhid_device_config& options) {
    mimhid_device* device = nullptr;
    const mimhid_result r = mimhid_device_open(board.path(), &options, &device);
    if (r != MIMHID_OK) {
        std::fprintf(stderr, "device_open: %s (%s)\n", mimhid_result_name(r),
                     mimhid_last_error()->reason);
        ++g_failures;
    }
    return device;
}

inline mimhid_spec* keyboard_spec() {
    mimhid_keyboard_options o{};
    test_fill_keyboard(&o);
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&o, &spec) == MIMHID_OK);
    return spec;
}

inline mimhid_hid_node_config node_config(std::uint8_t interface_number = 0) {
    mimhid_hid_node_config c{};
    test_fill_node_config(&c, interface_number);
    return c;
}

// A Node of one vendor interface (class 0xFF) with an interrupt IN endpoint and the rules given,
// committed: the smallest thing that gives the Device a USB configuration to hold rules in.
inline mimhid_node* open_vendor_node(mimhid_device* device, const mimhid_control_rule* rules = nullptr,
                                     std::size_t rule_count = 0) {
    mimhid_usb_endpoint ep{};
    ep.address = 0x81;
    ep.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    ep.max_packet_size = 64;
    ep.interval = 1;
    ep.policy = {MIMHID_FULL_DROP_NEWEST, 4};
    mimhid_usb_interface itf{};
    itf.class_code = 0xFF;
    itf.endpoints = &ep;
    itf.endpoint_count = 1;
    mimhid_node_config config;
    mimhid_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    config.rules = rules;
    config.rule_count = rule_count;
    mimhid_node* node = nullptr;
    const mimhid_result r = mimhid_node_open(device, &config, &kCommit, &node);
    MIMHID_CHECK(r == MIMHID_OK);
    return node;
}

} // namespace scripted

// The suites, one per file of tests/scripted (test_<suite>.cpp); test_scripted_main.cpp lists them.
void test_serial_port();
void test_link();
void test_handshaker();
void test_ports_linux();
void test_api_open();
void test_api_commands();
void test_api_reports();
void test_api_events();
void test_api_raw_node();
void test_api_generic();
