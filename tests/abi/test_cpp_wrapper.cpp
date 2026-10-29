// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Checks that the C++ header forwards to the C ABI under namespace mimhid. */
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
    const mimhid_node_config node = mimhid::node_config(1);
    if (config.struct_size != sizeof(config) || config.usb_device.vendor_id != 0 ||
        timeouts.control_timeout_ms != MIMHID_CONTROL_TIMEOUT_MS || node.ep_in.address != 0x82) {
        return 1;
    }

    const mimhid_mouse_options mouse = mimhid::mouse_options();
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
