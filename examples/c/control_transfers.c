// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <mimhid.h>

#include <inttypes.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

// The waits are this program's choice; the library has no defaults.
#define CONTROL_TIMEOUT_MS 500
#define SEND_TIMEOUT_MS 500
#define RECONNECT_TIMEOUT_MS 3000
#define DRAIN_TIMEOUT_MS 1000

static const mimhid_commit_timeouts commit_timeouts = {sizeof(mimhid_commit_timeouts), 0,
                                                       CONTROL_TIMEOUT_MS, RECONNECT_TIMEOUT_MS};

static const uint8_t vendor_answer[] = {0x01, 0x02, 0x03, 0x04};

static int fail(const char* what, mimhid_result r) {
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), mimhid_last_error()->reason);
    return 1;
}

static void on_event(void* user, const mimhid_event* event) {
    mimhid_device* device = *(mimhid_device**)user;
    if (event->kind != MIMHID_EVENT_CONTROL_SETUP)
        return;
    printf("setup: bmRequestType=0x%02x bRequest=0x%02x token=%" PRIu32 "%s%s\n",
           event->data[0], event->data[1], event->control_token,
           (event->control_flags & MIMHID_CONTROL_FLAG_HELD) ? " held" : "",
           (event->control_flags & MIMHID_CONTROL_FLAG_FIXED) ? " fixed" : "");
    if (event->control_flags & MIMHID_CONTROL_FLAG_HELD)
        mimhid_device_control_reply(device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER,
                                    vendor_answer, sizeof(vendor_answer), SEND_TIMEOUT_MS);
}

int main(void) {
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(50, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        mimhid_discovery_destroy(found);
        fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device* device = NULL;
    mimhid_device_config config;
    mimhid_device_config_init(&config);
    config.usb_device.vendor_id = 0x046D;
    config.usb_device.product_id = 0xC52B;
    config.usb_device.manufacturer = "Example";
    config.usb_device.product = "Control transfers";
    config.usb_device.serial_from_chip_id = 1;
    config.transport.baud_rate = mimhid_discovery_get(found, 0)->baud_rate;
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
    config.transport.event_sink = on_event;
    config.transport.event_user = &device;

    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    mimhid_keyboard_options keyboard_options = {0};
    keyboard_options.struct_size = sizeof(keyboard_options);
    keyboard_options.report_id.enabled = 1;
    keyboard_options.report_id.value = 1;
    keyboard_options.usage_minimum = 0x04;
    keyboard_options.usage_maximum = 0x65;
    mimhid_spec* keyboard_spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMHID_OK)
        return fail("spec_create", r);

    mimhid_node_config node_config;
    mimhid_node_config_init(&node_config);
    node_config.iface.number = 0;
    node_config.iface.class_code = 3;
    node_config.iface.bcd_hid = 0x0111;
    node_config.ep_in.address = 0x81;
    node_config.ep_in.poll_interval_ms = 1;
    node_config.ep_in.max_packet_size = 64;
    mimhid_node* keyboard = NULL;
    if ((r = mimhid_node_open(device, keyboard_spec, &node_config, &commit_timeouts, &keyboard)) != MIMHID_OK ||
        (r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("enumeration", r);

    // Report every request, and hold vendor requests 0x42 that read from the device: the sink
    // answers them. If no answer came within 100 ms the target would see a STALL.
    mimhid_control_rule rule = {0};
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0; // device to host, vendor, device
    rule.request_mask = 0xFF;
    rule.request_value = 0x42;
    rule.timeout_ms = 100;
    rule.when_late = MIMHID_CONTROL_ACTION_STALL;
    if ((r = mimhid_device_set_control_routing(device, 1, &rule, 1, CONTROL_TIMEOUT_MS)) != MIMHID_OK)
        return fail("set_control_routing", r);

    printf("Listening for 10 s; send vendor request 0x42 (read) from the target to get an answer.\n");
    sleep_ms(10000);

    mimhid_node_close(keyboard, &commit_timeouts, DRAIN_TIMEOUT_MS);
    mimhid_device_close(device, CONTROL_TIMEOUT_MS, DRAIN_TIMEOUT_MS); // takes the rules off the board
    mimhid_spec_release(keyboard_spec);
    return 0;
}
