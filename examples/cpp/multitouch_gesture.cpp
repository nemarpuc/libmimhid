// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// The first board found presents a two-finger touchscreen and spreads two fingers apart.
#include <mimhid.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    mimhid_discovery* found_raw = nullptr;
    if (mimhid::discover(50, &found_raw) != MIMHID_OK) {
        std::fprintf(stderr, "discover: %s\n", mimhid::last_error()->reason);
        return 1;
    }
    const mimhid::DiscoveryPtr found{found_raw};
    if (mimhid::discovery_count(found.get()) == 0) {
        std::fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }
    const mimhid_device_info* board = mimhid::discovery_get(found.get(), 0);

    auto config = mimhid::device_config();
    // pid.codes test IDs, for testing in-house only: a device you hand out needs IDs of its own.
    config.usb_device.vendor_id = 0x1209;
    config.usb_device.product_id = 0x0001;
    config.usb_device.product = "Touchscreen";
    config.transport.baud_rate = board->baud_rate;
    mimhid_device* device = nullptr;
    if (const mimhid_result opened = mimhid::device_open(board->port_name, &config, &device);
        opened != MIMHID_OK) {
        std::fprintf(stderr, "device_open: %s\n", mimhid::result_name(opened));
        return 1;
    }

    // The coordinates of a 1080 x 2400 screen, two contacts.
    auto touch = mimhid::touchscreen_options();
    touch.maximum_contacts = 2;
    touch.contacts_per_report = 2;
    touch.x = mimhid_integer_field_make(0, 1080, 16);
    touch.y = mimhid_integer_field_make(0, 2400, 16);
    touch.contact_count = mimhid_integer_field_make(0, 2, 2);
    touch.enable_contact_count_maximum_feature_declaration = 1;
    mimhid_spec* spec_raw = nullptr;
    const mimhid_result created = mimhid::spec_create_touchscreen(&touch, &spec_raw);
    const mimhid::SpecPtr spec{spec_raw};

    const auto timeouts = mimhid::commit_timeouts();
    const auto node_config = mimhid::node_config(0);
    mimhid_node* node = nullptr;
    if (created != MIMHID_OK ||
        mimhid::node_open(device, spec.get(), &node_config, &timeouts, &node) != MIMHID_OK ||
        mimhid::device_wait_ready(device, 5000) != MIMHID_OK) {
        std::fprintf(stderr, "setup failed: %s\n", mimhid::last_error()->reason);
        (void)mimhid::device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
        return 1;
    }

    for (int step = 0; step <= 20; ++step) {
        const std::int32_t spread = step * 15;
        (void)mimhid::touchscreen_contact(node, 0, 1, 540 - spread, 1200, nullptr);
        (void)mimhid::touchscreen_contact(node, 1, 1, 540 + spread, 1200, nullptr);
        (void)mimhid::node_submit_blocking(node, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    (void)mimhid::touchscreen_contact(node, 0, 0, 240, 1200, nullptr);
    (void)mimhid::touchscreen_contact(node, 1, 0, 840, 1200, nullptr);
    (void)mimhid::node_submit_blocking(node, 100);

    // Closing the Device closes the Node too.
    return mimhid::device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS) ==
                   MIMHID_OK
               ? 0
               : 1;
}
