// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Checks that the C++ header forwards to the C ABI under namespace mimhid. */
#include "../fill_values.h"
#include "mimhid.hpp"

int main() {
    if (mimhid::version() != mimhid_version() || mimhid::result_name(MIMHID_OK) == nullptr ||
        mimhid::frame_type_name(0x01) == nullptr ||
        mimhid::keyboard_key(nullptr, 0U, 0U) != MIMHID_ERR_PARAM ||
        mimhid::mouse_move(nullptr, 0, 0) != MIMHID_ERR_PARAM ||
        mimhid::node_submit(nullptr, 1U) != MIMHID_ERR_PARAM ||
        mimhid::discover(1U, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_open(nullptr, nullptr, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_commit(nullptr, 0U, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_connect(nullptr, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_disconnect(nullptr, 1U) != MIMHID_ERR_PARAM ||
        mimhid::node_open_raw(nullptr, nullptr, 0U, nullptr, nullptr, nullptr) !=
            MIMHID_ERR_PARAM ||
        mimhid::report_send(nullptr, nullptr, 0U, 1U) != MIMHID_ERR_PARAM ||
        mimhid::last_error() != mimhid_last_error()) {
        return 1;
    }

    const mimhid_device_config config = mimhid::device_config();
    const mimhid_commit_timeouts timeouts = mimhid::commit_timeouts();
    const mimhid_node_config node = mimhid::node_config();
    // The wrappers return what the C *_init functions do: struct_size set, every other byte zero.
    if (config.struct_size != sizeof(config) || config.usb_device.vendor_id != 0 ||
        config.usb_device.product_id != 0 || config.usb_device.bcd_usb != 0 ||
        config.transport.baud_rate != 0 || config.usb_device.manufacturer != nullptr ||
        timeouts.struct_size != sizeof(timeouts) || timeouts.control_timeout_ms != 0 ||
        timeouts.reconnect_timeout_ms != 0 || node.struct_size != sizeof(node) ||
        node.ep_in.address != 0 || node.ep_in.max_packet_size != 0 || node.iface.class_code != 0) {
        return 1;
    }

    mimhid_mouse_options mouse;
    test_fill_mouse(&mouse);
    mimhid_spec* raw_spec = nullptr;
    if (mimhid::spec_create_mouse(&mouse, &raw_spec) != MIMHID_OK) {
        return 1;
    }
    const mimhid::SpecPtr spec{raw_spec};
    mimhid_capability_manifest manifest = mimhid::capability_manifest();
    if (mimhid::spec_manifest(spec.get(), &manifest) != MIMHID_OK ||
        manifest.profile_kind != MIMHID_PROFILE_MOUSE) {
        return 1;
    }
    const mimhid::DiscoveryPtr empty{};
    return 0;
}
