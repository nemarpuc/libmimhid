// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// The first board found presents a two-finger touchscreen and spreads two fingers apart.
#include <mimusb.hpp>

#include "../c/example_values.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    mimusb_discovery* found_raw = nullptr;
    if (mimusb::discover(50, &found_raw) != MIMUSB_OK) {
        std::fprintf(stderr, "discover: %s\n", mimusb::last_error()->reason);
        return 1;
    }
    const mimusb::DiscoveryPtr found{found_raw};
    if (mimusb::discovery_count(found.get()) == 0) {
        std::fprintf(stderr, "No mimusb board found.\n");
        return 1;
    }
    const mimusb_device_info* board = mimusb::discovery_get(found.get(), 0);

    // example_values.h: every value of the Device written out, with the pid.codes test IDs
    // (for testing in-house only: a device you hand out needs IDs of its own).
    mimusb_device_config config;
    example_fill_device_config(&config, board->baud_rate, "Touchscreen");
    mimusb_device* device = nullptr;
    if (const mimusb_result opened = mimusb::device_open(board->port_name, &config, &device);
        opened != MIMUSB_OK) {
        std::fprintf(stderr, "device_open: %s\n", mimusb::result_name(opened));
        return 1;
    }

    // The coordinates of a 1080 x 2400 screen, two contacts.
    mimusb_touchscreen_options touch;
    example_fill_touchscreen_options(&touch);
    touch.maximum_contacts = 2;
    touch.contacts_per_report = 2;
    touch.x = mimusb_integer_field_make(0, 1080, 16);
    touch.y = mimusb_integer_field_make(0, 2400, 16);
    touch.contact_count = mimusb_integer_field_make(0, 2, 2);
    touch.enable_contact_count_maximum_feature_declaration = 1;
    mimusb_spec* spec_raw = nullptr;
    const mimusb_result created = mimusb::spec_create_touchscreen(&touch, &spec_raw);
    const mimusb::SpecPtr spec{spec_raw};

    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimusb_hid_node_config node_config;
    example_fill_node_config(&node_config, 0);
    mimusb_node* node = nullptr;
    if (created != MIMUSB_OK ||
        mimusb::node_open_profile(device, spec.get(), &node_config, &timeouts, &node) !=
            MIMUSB_OK ||
        mimusb::device_wait_ready(device, 5000) != MIMUSB_OK) {
        std::fprintf(stderr, "setup failed: %s\n", mimusb::last_error()->reason);
        (void)mimusb::device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
        return 1;
    }

    for (int step = 0; step <= 20; ++step) {
        const std::int32_t spread = step * 15;
        (void)mimusb::touchscreen_contact(node, 0, 1, 540 - spread, 1200, nullptr);
        (void)mimusb::touchscreen_contact(node, 1, 1, 540 + spread, 1200, nullptr);
        (void)mimusb::node_submit_blocking(node, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    (void)mimusb::touchscreen_contact(node, 0, 0, 240, 1200, nullptr);
    (void)mimusb::touchscreen_contact(node, 1, 0, 840, 1200, nullptr);
    (void)mimusb::node_submit_blocking(node, 100);

    // Closing the Device closes the Node too.
    return mimusb::device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS) ==
                   MIMUSB_OK
               ? 0
               : 1;
}
