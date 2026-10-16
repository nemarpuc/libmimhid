// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <mimhid.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    mimhid_discovery* found = nullptr;
    if (mimhid::discover(500, &found) != MIMHID_OK || mimhid::discovery_count(found) == 0) {
        mimhid::discovery_destroy(found);
        std::fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device_options options{};
    options.struct_size = sizeof(options);
    options.vendor_id = 0x04F3;
    options.product_id = 0x2A15;
    options.manufacturer = "Example";
    options.product = "Touchscreen";
    options.linux_descriptor_policy_bytes = 4096;
    options.linux_hid_fields_per_report_policy = 256;
    options.linux_hid_global_stack_depth_policy = 4;
    options.linux_hid_usages_policy = 1024;
    options.linux_hid_report_data_policy_bits = 8192;
    options.linux_hid_report_size_policy_bits = 32;

    mimhid_device* device = nullptr;
    const mimhid_result opened =
        mimhid::device_open(mimhid::discovery_get(found, 0)->port_name, &options, &device);
    mimhid::discovery_destroy(found);
    if (opened != MIMHID_OK) {
        std::fprintf(stderr, "device_open: %s\n", mimhid::result_name(opened));
        return 1;
    }

    mimhid_touchscreen_options touch{};
    touch.struct_size = sizeof(touch);
    touch.maximum_contacts = 2;
    touch.contacts_per_report = 2;
    touch.contact_id = {0, 15, 4, {}};
    touch.x = {0, 1080, 16, {}};
    touch.y = {0, 2400, 16, {}};
    touch.contact_count = {0, 2, 2, {}};
    touch.enable_contact_count_maximum_feature_declaration = 1;

    mimhid_spec* spec = nullptr;
    mimhid_node_options node_options{};
    node_options.struct_size = sizeof(node_options);
    node_options.interface_number = 0;
    node_options.poll_interval_ms = 1;
    node_options.ep_in_size = 64;
    node_options.bcd_hid = 0x0111;
    mimhid_node* node = nullptr;
    if (mimhid::spec_create_touchscreen(&touch, &spec) != MIMHID_OK ||
        mimhid::node_open(device, spec, &node_options, &node) != MIMHID_OK ||
        mimhid::device_wait_ready(device, 5000) != MIMHID_OK) {
        std::fprintf(stderr, "setup failed: %s\n", mimhid::last_error()->reason);
        return 1;
    }

    // Two fingers move apart (a pinch out).
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

    (void)mimhid::node_close(node);
    (void)mimhid::device_close(device);
    mimhid::spec_release(spec);
    return 0;
}
