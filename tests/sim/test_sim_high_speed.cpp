// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The rules only a high-speed board has, against the firmware core built for high speed. */
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

#ifdef SIM_HIGH_SPEED

namespace sim {

// The rules only a high-speed board has: USB 2.0 9.6.1 (the control endpoint is 64 bytes) and
// 9.6.6 (bInterval of an interrupt endpoint is an exponent, 1 to 16). The other tests assume
// full-speed packet sizes, so this build runs these alone.
void test_high_speed_rules() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_device_info board{};
    MIMHID_CHECK(mimhid_device_board_info(device, &board) == MIMHID_OK);
    mimhid_board_limits limits;
    mimhid_board_limits_init(&limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_endpoint_bytes == 1024);
    MIMHID_CHECK(board.usb_high_speed == 1);

    mimhid_device_config o = base_options();
    o.usb_device.ep0_max_packet_size = 8;
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_ERR_PARAM);
    o.usb_device.ep0_max_packet_size = 64;
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_OK);
    mimhid_device_info port{};
    std::strncpy(port.port_name, g_slave.c_str(), sizeof(port.port_name) - 1);

    mimhid_spec* spec = keyboard_spec(1);
    mimhid_node_config nc{};
    mimhid_node* node = nullptr;
    for (const std::uint8_t interval : {0, 17, 255}) {
        test_fill_node_config(&nc, 0);
        nc.ep_in.poll_interval_ms = interval;
        MIMHID_CHECK(mimhid_node_open(device, spec, &nc, &kCommit, &node) ==
                     (interval == 0 ? MIMHID_ERR_UNSET_FIELD : MIMHID_ERR_PARAM));
    }
    test_fill_node_config(&nc, 0);
    nc.ep_in.poll_interval_ms = 1;
    nc.ep_out.address = 0x01;
    nc.ep_out.max_packet_size = 8;
    nc.ep_out.poll_interval_ms = 17;
    MIMHID_CHECK(mimhid_node_open(device, spec, &nc, &kCommit, &node) == MIMHID_ERR_PARAM);
    test_fill_node_config(&nc, 0);
    nc.ep_in.poll_interval_ms = 16;
    nc.ep_in.max_packet_size = 1024; // the packet of a high-speed interrupt endpoint (5.7.3)
    node = open_node(device, spec, &nc);
    MIMHID_CHECK(node != nullptr);
    if (node != nullptr)
        MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace sim

#endif
