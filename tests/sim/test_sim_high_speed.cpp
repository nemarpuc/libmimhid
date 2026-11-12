// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The rules only a high-speed board has, against the firmware core built for high speed. */
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

#ifdef SIM_HIGH_SPEED

namespace sim {

// The rules only a high-speed board has: USB 2.0 9.6.1 (the control endpoint is 64 bytes) and
// 9.6.6 (bInterval of an interrupt endpoint is an exponent, 1 to 16). The other tests assume
// full-speed packet sizes, so this build runs these alone.
void test_high_speed_rules() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_device_info board{};
    MIMUSB_CHECK(mimusb_device_board_info(device, &board) == MIMUSB_OK);
    mimusb_board_limits limits;
    mimusb_board_limits_init(&limits);
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_OK &&
                 limits.max_endpoint_bytes == 1024);
    MIMUSB_CHECK(board.usb_high_speed == 1);

    mimusb_device_config o = base_options();
    o.usb_device.ep0_max_packet_size = 8;
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_ERR_PARAM);
    o.usb_device.ep0_max_packet_size = 64;
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_OK);
    mimusb_device_info port{};
    std::strncpy(port.port_name, g_slave.c_str(), sizeof(port.port_name) - 1);

    mimusb_spec* spec = keyboard_spec(1);
    mimusb_hid_node_config nc{};
    mimusb_node* node = nullptr;
    for (const std::uint8_t interval : {0, 17, 255}) {
        test_fill_node_config(&nc, 0);
        nc.ep_in.interval = interval;
        MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &nc, &kCommit, &node) ==
                     (interval == 0 ? MIMUSB_ERR_UNSET_FIELD : MIMUSB_ERR_PARAM));
    }
    test_fill_node_config(&nc, 0);
    nc.ep_in.interval = 1;
    nc.ep_out.address = 0x01;
    nc.ep_out.max_packet_size = 8;
    nc.ep_out.interval = 17;
    nc.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &nc, &kCommit, &node) == MIMUSB_ERR_PARAM);
    test_fill_node_config(&nc, 0);
    nc.ep_in.interval = 16;
    nc.ep_in.max_packet_size = 1024; // the packet of a high-speed interrupt endpoint (5.7.3)
    node = open_node(device, spec, &nc);
    MIMUSB_CHECK(node != nullptr);
    if (node != nullptr)
        MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace sim

#endif
