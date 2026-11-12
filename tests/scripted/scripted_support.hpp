// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* What the scripted-board tests share: a device configuration for the scripted board, an open
 * helper and the suites of this runner. Test-only. */
#pragma once

#include "fill_values.h"
#include "mimusb.h"
#include "scripted_board.hpp"
#include "test.hpp"

#include <cstdint>
#include <cstdio>

namespace scripted {

inline constexpr std::uint32_t kControlMs = 100;
inline constexpr std::uint32_t kSendMs = 100;
inline constexpr std::uint32_t kDrainMs = 200;
inline const mimusb_commit_timeouts kCommit = {sizeof(mimusb_commit_timeouts), 0, kControlMs, 1000};

// The test configuration on the boot rate, so that no rate is tested unless a test asks.
inline mimusb_device_config board_options() {
    mimusb_device_config o{};
    test_fill_device_config(&o);
    o.transport.baud_rate = MIMUSB_BOOT_BAUD;
    o.transport.handshake_timeout_ms = 50;
    return o;
}

inline mimusb_device* open_on(Board& board, const mimusb_device_config& options) {
    mimusb_device* device = nullptr;
    const mimusb_result r = mimusb_device_open(board.path(), &options, &device);
    if (r != MIMUSB_OK) {
        std::fprintf(stderr, "device_open: %s (%s)\n", mimusb_result_name(r),
                     mimusb_last_error()->reason);
        ++g_failures;
    }
    return device;
}

inline mimusb_spec* keyboard_spec() {
    mimusb_keyboard_options o{};
    test_fill_keyboard(&o);
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&o, &spec) == MIMUSB_OK);
    return spec;
}

inline mimusb_hid_node_config node_config(std::uint8_t interface_number = 0) {
    mimusb_hid_node_config c{};
    test_fill_node_config(&c, interface_number);
    return c;
}

// A Node of one vendor interface (class 0xFF) with an interrupt IN endpoint and the rules given,
// committed: the smallest thing that gives the Device a USB configuration to hold rules in.
inline mimusb_node* open_vendor_node(mimusb_device* device,
                                     const mimusb_control_rule* rules = nullptr,
                                     std::size_t rule_count = 0) {
    mimusb_usb_endpoint ep{};
    ep.address = 0x81;
    ep.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    ep.max_packet_size = 64;
    ep.interval = 1;
    ep.policy = {MIMUSB_FULL_DROP_NEWEST, 4};
    const mimusb_usb_item item = mimusb_usb_item_make_endpoint(ep);
    const mimusb_usb_setting setting = {&item, 1};
    mimusb_usb_interface itf{};
    itf.class_code = 0xFF;
    itf.settings = &setting;
    itf.setting_count = 1;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    config.rules = rules;
    config.rule_count = rule_count;
    mimusb_node* node = nullptr;
    const mimusb_result r = mimusb_node_open(device, &config, &kCommit, &node);
    MIMUSB_CHECK(r == MIMUSB_OK);
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
