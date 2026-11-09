// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* mimhid_device_open refuses every configuration it cannot present as given. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sim {

void test_bad_arguments() {
    mimhid_device_info info{};
    std::strncpy(info.port_name, g_slave.c_str(), sizeof(info.port_name) - 1);
    mimhid_device* device = nullptr;
    mimhid_device_config o = base_options();
    o.usb_device.bcd_usb = 0; // the descriptor fields that have no zero are required
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD &&
                 device == nullptr);
    o = base_options();
    o.transport.linux_hid.usages = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    // Nothing is chosen for the caller.
    o = base_options();
    o.transport.handshake_timeout_ms = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    // No default for the largest report, the log level or the reserved field.
    o = base_options();
    o.transport.maximum_report_bytes = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.transport.maximum_report_bytes = 65;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_OVERFLOW);
    o = base_options();
    o.transport.log_level = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.transport.log_level = MIMHID_LOG_ERROR; // a level other than DISABLED needs a sink
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.reserved = 1;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_device.language_id = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.bcd_usb = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 48;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_config.configuration_value = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    // USB 2.0 9.6.3: bmAttributes bits 4..0 are reserved. 9.6.1: a subclass needs a class.
    o = base_options();
    o.usb_config.bm_attributes = 0x81;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_device.device_class = 0;
    o.usb_device.device_subclass = 1;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    // The board has one configuration, which SET_CONFIGURATION selects by the value 1.
    o = base_options();
    o.usb_config.configuration_value = 2;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSUPPORTED);
    o.usb_config.configuration_value = 255;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSUPPORTED);
    o = base_options();
    o.usb_config.bm_attributes = 0x00;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_config.bmax_power = 251;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.transport.baud_rate = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    for (const char* bad : {"caf\xE9",                          // Latin-1, not UTF-8
                            "\xC0\x80",                         // overlong NUL
                            "\xED\xA0\x80",                     // a surrogate
                            "\xF4\x90\x80\x80",                 // above U+10FFFF
                            "tab\there", "del\x7F", "\xC2\x85", // control characters
                            "\xE3\x81"}) {                      // cut short
        o = base_options();
        o.usb_device.manufacturer = bad;
        MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    }
    {
        // 127 code units: one more than a string descriptor holds.
        const std::string too_long(127, 'a');
        o = base_options();
        o.usb_device.product = too_long.c_str();
        MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
        // 64 four-byte characters are 128 code units.
        std::string pairs;
        for (int i = 0; i < 64; ++i)
            pairs += "\xF0\x9F\x98\x80";
        o = base_options();
        o.usb_device.product = pairs.c_str();
        MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    }
    o = base_options();
    o.struct_size = 8;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    MIMHID_CHECK(mimhid_device_open("/dev/does-not-exist", &o, &device) == MIMHID_ERR_ACCESS);
    MIMHID_CHECK(mimhid_device_open(nullptr, &o, &device) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_open("", &o, &device) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_close(nullptr, kControlMs, kDrainMs) == MIMHID_ERR_PARAM);
}

} // namespace sim
