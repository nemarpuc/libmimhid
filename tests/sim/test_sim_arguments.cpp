// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* mimusb_device_open refuses every configuration it cannot present as given. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sim {

void test_bad_arguments() {
    mimusb_device_info info{};
    std::strncpy(info.port_name, g_slave.c_str(), sizeof(info.port_name) - 1);
    mimusb_device* device = nullptr;
    mimusb_device_config o = base_options();
    o.usb_device.bcd_usb = 0; // the descriptor fields that have no zero are required
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD &&
                 device == nullptr);
    o = base_options();
    o.target.linux_hid.usages = 0;
    // The limits of the target are the HID Spec's: a Device opens without them, and a Node of a
    // HID Spec does not.
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_OK && device != nullptr);
    if (device != nullptr) {
        mimusb_spec* kbd = keyboard_spec(0);
        test_hid_node_config nc{};
        test_fill_node_config(&nc, 0);
        mimusb_node* node = nullptr;
        MIMUSB_CHECK(test_node_open_hid(device, kbd, &nc, &kCommit, &node) ==
                         MIMUSB_ERR_UNSET_FIELD &&
                     node == nullptr);
        mimusb_spec_release(kbd);
        MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
        device = nullptr;
    }
    // Nothing is chosen for the caller.
    o = base_options();
    o.transport.handshake_timeout_ms = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    // No default for the largest report, the log level or the reserved field.
    o = base_options();
    o.transport.maximum_report_bytes = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    o.transport.maximum_report_bytes = 65;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_OVERFLOW);
    o = base_options();
    o.transport.log_level = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    o.transport.log_level = MIMUSB_LOG_ERROR; // a level other than DISABLED needs a sink
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.reserved = 1;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.usb_device.language_id = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.bcd_usb = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 48;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.usb_config.configuration_value = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    // USB 2.0 9.6.3: bmAttributes bits 4..0 are reserved. 9.6.1: a subclass needs a class.
    o = base_options();
    o.usb_config.bm_attributes = 0x81;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.usb_device.device_class = 0;
    o.usb_device.device_subclass = 1;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    // The board has one configuration, which SET_CONFIGURATION selects by the value 1.
    o = base_options();
    o.usb_config.configuration_value = 2;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSUPPORTED);
    o.usb_config.configuration_value = 255;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSUPPORTED);
    o = base_options();
    o.usb_config.bm_attributes = 0x00;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.usb_config.bmax_power = 251;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    o.transport.baud_rate = 0;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_UNSET_FIELD);
    o = base_options();
    for (const char* bad : {"caf\xE9",                          // Latin-1, not UTF-8
                            "\xC0\x80",                         // overlong NUL
                            "\xED\xA0\x80",                     // a surrogate
                            "\xF4\x90\x80\x80",                 // above U+10FFFF
                            "tab\there", "del\x7F", "\xC2\x85", // control characters
                            "\xE3\x81"}) {                      // cut short
        o = base_options();
        o.usb_device.manufacturer = bad;
        MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    }
    {
        // 127 code units: one more than a string descriptor holds.
        const std::string too_long(127, 'a');
        o = base_options();
        o.usb_device.product = too_long.c_str();
        MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
        // 64 four-byte characters are 128 code units.
        std::string pairs;
        for (int i = 0; i < 64; ++i)
            pairs += "\xF0\x9F\x98\x80";
        o = base_options();
        o.usb_device.product = pairs.c_str();
        MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    }
    o = base_options();
    o.struct_size = 8;
    MIMUSB_CHECK(mimusb_device_open(info.port_name, &o, &device) == MIMUSB_ERR_PARAM);
    o = base_options();
    MIMUSB_CHECK(mimusb_device_open("/dev/does-not-exist", &o, &device) == MIMUSB_ERR_ACCESS);
    MIMUSB_CHECK(mimusb_device_open(nullptr, &o, &device) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_open("", &o, &device) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_close(nullptr, kControlMs, kDrainMs) == MIMUSB_ERR_PARAM);
}

} // namespace sim
