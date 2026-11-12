// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Checks that the C++ header forwards to the C ABI under namespace mimusb. */
#include "../fill_values.h"
#include "mimusb.hpp"

int main() {
    if (mimusb::version() != mimusb_version() || mimusb::result_name(MIMUSB_OK) == nullptr ||
        mimusb::frame_type_name(0x01) == nullptr ||
        mimusb::keyboard_key(nullptr, 0U, 0U) != MIMUSB_ERR_PARAM ||
        mimusb::mouse_move(nullptr, 0, 0) != MIMUSB_ERR_PARAM ||
        mimusb::node_submit(nullptr, 1U) != MIMUSB_ERR_PARAM ||
        mimusb::discover(1U, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::device_open(nullptr, nullptr, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::device_commit(nullptr, 0U, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::device_connect(nullptr, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::device_disconnect(nullptr, 1U) != MIMUSB_ERR_PARAM ||
        mimusb::device_set_control_rules(nullptr, 0U, nullptr, 0U, 1U) != MIMUSB_ERR_PARAM ||
        mimusb::device_set_rule_data(nullptr, 0U, nullptr, 0U, 1U) != MIMUSB_ERR_PARAM ||
        mimusb::node_set_rule_data(nullptr, 0U, nullptr, 0U, 1U) != MIMUSB_ERR_PARAM ||
        mimusb::hid_descriptor_bytes(0x0111U, 0U, 0U, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::hid_descriptor_rule(0U, 0x22U, nullptr, 0U, nullptr) != MIMUSB_ERR_PARAM ||
        mimusb::last_error() != mimusb_last_error()) {
        return 1;
    }

    const mimusb_device_config config = mimusb::device_config();
    const mimusb_commit_timeouts timeouts = mimusb::commit_timeouts();
    const mimusb_hid_node_config node = mimusb::hid_node_config();
    // The wrappers return what the C *_init functions do: struct_size set, every other byte zero.
    if (config.struct_size != sizeof(config) || config.usb_device.vendor_id != 0 ||
        config.usb_device.product_id != 0 || config.usb_device.bcd_usb != 0 ||
        config.transport.baud_rate != 0 || config.usb_device.manufacturer != nullptr ||
        timeouts.struct_size != sizeof(timeouts) || timeouts.control_timeout_ms != 0 ||
        timeouts.reconnect_timeout_ms != 0 || node.struct_size != sizeof(node) ||
        node.ep_in.address != 0 || node.ep_in.max_packet_size != 0 || node.bcd_hid != 0) {
        return 1;
    }

    mimusb_mouse_options mouse;
    test_fill_mouse(&mouse);
    mimusb_spec* raw_spec = nullptr;
    if (mimusb::spec_create_mouse(&mouse, &raw_spec) != MIMUSB_OK) {
        return 1;
    }
    const mimusb::SpecPtr spec{raw_spec};
    mimusb_capability_manifest manifest = mimusb::capability_manifest();
    if (mimusb::spec_manifest(spec.get(), &manifest) != MIMUSB_OK ||
        manifest.profile_kind != MIMUSB_PROFILE_MOUSE) {
        return 1;
    }
    const mimusb::DiscoveryPtr empty{};
    return 0;
}
