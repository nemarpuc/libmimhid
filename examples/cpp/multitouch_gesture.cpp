// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <mimhid.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

// The waits are this program's choice; the library has no defaults.
constexpr std::uint32_t kControlTimeoutMs = 500;
constexpr std::uint32_t kReconnectTimeoutMs = 3000;
constexpr std::uint32_t kDrainTimeoutMs = 1000;

int main() {
    const mimhid_commit_timeouts commit_timeouts = {sizeof(mimhid_commit_timeouts), 0,
                                                    kControlTimeoutMs, kReconnectTimeoutMs};
    mimhid_discovery* found = nullptr;
    if (mimhid::discover(50, &found) != MIMHID_OK || mimhid::discovery_count(found) == 0) {
        mimhid::discovery_destroy(found);
        std::fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device_config config{};
    mimhid_device_config_init(&config);
    config.usb_device.vendor_id = 0x04F3;
    config.usb_device.product_id = 0x2A15;
    config.usb_device.manufacturer = "Example";
    config.usb_device.product = "Touchscreen";
    // The rate the search measured.
    config.transport.baud_rate = mimhid::discovery_get(found, 0)->baud_rate;
    config.transport.handshake_timeout_ms = 50;
    config.transport.maximum_report_bytes = 64;
    config.transport.log_level = MIMHID_LOG_DISABLED;
    config.usb_device.language_id = 0x0409;
    config.usb_device.bcd_usb = 0x0200;
    config.usb_device.ep0_max_packet_size = 64;
    config.usb_device.bcd_device = 0x0100;
    config.usb_config.configuration_value = 1;
    config.usb_config.bm_attributes = 0x80;
    config.usb_config.bmax_power = 50;
    config.transport.linux_descriptor_policy_bytes = 4096;
    config.transport.linux_hid_fields_per_report_policy = 256;
    config.transport.linux_hid_global_stack_depth_policy = 4;
    config.transport.linux_hid_usages_policy = 1024;
    config.transport.linux_hid_report_data_policy_bits = 8192;
    config.transport.linux_hid_report_size_policy_bits = 32;

    mimhid_device* device = nullptr;
    const mimhid_result opened =
        mimhid::device_open(mimhid::discovery_get(found, 0)->port_name, &config, &device);
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
    mimhid_node_config node_config{};
    mimhid_node_config_init(&node_config);
    node_config.iface.number = 0;
    node_config.iface.class_code = 3;
    node_config.iface.bcd_hid = 0x0111;
    node_config.ep_in.address = 0x81;
    node_config.ep_in.poll_interval_ms = 1;
    node_config.ep_in.max_packet_size = 64;
    mimhid_node* node = nullptr;
    if (mimhid::spec_create_touchscreen(&touch, &spec) != MIMHID_OK ||
        mimhid::node_open(device, spec, &node_config, &commit_timeouts, &node) != MIMHID_OK ||
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

    (void)mimhid::node_close(node, &commit_timeouts, kDrainTimeoutMs);
    (void)mimhid::device_close(device, kControlTimeoutMs, kDrainTimeoutMs);
    mimhid::spec_release(spec);
    return 0;
}
