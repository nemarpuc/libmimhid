// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The C ABI's argument checks that need no board: every entry point given a null handle, a null
 * output or an empty discovery returns its documented error (or a harmless value) and says
 * which argument was wrong. The checks with a board are in tests/scripted. */
#include "mimusb.h"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

bool refused(const mimusb_result result, const mimusb_result expected = MIMUSB_ERR_PARAM) {
    return result == expected && mimusb_last_error()->code == expected &&
           mimusb_last_error()->field != nullptr;
}

void test_device_arguments() {
    mimusb_device_config config{};
    mimusb_device* device = nullptr;
    MIMUSB_CHECK(refused(mimusb_device_open(nullptr, &config, &device)));
    MIMUSB_CHECK(refused(mimusb_device_open("/dev/null", nullptr, &device)));
    MIMUSB_CHECK(refused(mimusb_device_open("/dev/null", &config, nullptr)));
    MIMUSB_CHECK(device == nullptr);
    const mimusb_commit_timeouts timeouts{sizeof(mimusb_commit_timeouts), 0, 100, 1000};
    MIMUSB_CHECK(refused(mimusb_device_close(nullptr, 100, 100)));
    MIMUSB_CHECK(refused(mimusb_device_set_usb_identity(nullptr, nullptr, nullptr, &timeouts)));
    MIMUSB_CHECK(refused(mimusb_device_latched_error(nullptr)) &&
                 std::strcmp(mimusb_last_error()->field, "device") == 0);
    MIMUSB_CHECK(mimusb_device_is_ready(nullptr) == 0);
    MIMUSB_CHECK(refused(mimusb_device_wait_ready(nullptr, 10)));
    MIMUSB_CHECK(refused(mimusb_device_batch_begin(nullptr)));
    MIMUSB_CHECK(refused(mimusb_device_batch_end(nullptr, &timeouts)));
    MIMUSB_CHECK(refused(mimusb_device_commit(nullptr, 0, &timeouts)));
    MIMUSB_CHECK(refused(mimusb_device_connect(nullptr, &timeouts)));
    MIMUSB_CHECK(refused(mimusb_device_disconnect(nullptr, 100)));
    MIMUSB_CHECK(refused(mimusb_device_remote_wakeup(nullptr, 100)));
    MIMUSB_CHECK(refused(mimusb_device_enter_bootloader(nullptr, 100)));
    mimusb_device_info info{};
    mimusb_board_limits limits{};
    mimusb_link_counters counters{};
    MIMUSB_CHECK(refused(mimusb_device_board_info(nullptr, &info)));
    MIMUSB_CHECK(refused(mimusb_device_limits(nullptr, &limits)));
    MIMUSB_CHECK(refused(mimusb_device_link_counters(nullptr, 0, 100, &counters)));
    MIMUSB_CHECK(refused(mimusb_device_set_control_rules(nullptr, 0, nullptr, 0, 100)));
    MIMUSB_CHECK(refused(mimusb_device_set_rule_data(nullptr, 0, nullptr, 0, 100)));
    MIMUSB_CHECK(refused(
        mimusb_device_control_reply(nullptr, 1, MIMUSB_CONTROL_ACTION_LEAVE, nullptr, 0, 10)));
    MIMUSB_CHECK(refused(mimusb_device_set_loss_reporting(nullptr, 0, 0, 100)));
}

void test_node_arguments() {
    const mimusb_commit_timeouts timeouts{sizeof(mimusb_commit_timeouts), 0, 100, 1000};
    MIMUSB_CHECK(refused(mimusb_node_close(nullptr, &timeouts, 100)));
    MIMUSB_CHECK(mimusb_node_id(nullptr) == 0U && mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                 std::strcmp(mimusb_last_error()->field, "node") == 0);
    const std::uint8_t data[1] = {0};
    MIMUSB_CHECK(refused(mimusb_node_set_feature_response(nullptr, 0, data, 1, 100)));
    const mimusb_endpoint_policy policy{MIMUSB_FULL_DROP_NEWEST, 1};
    MIMUSB_CHECK(refused(mimusb_node_endpoint_set_policy(nullptr, 0x81, &policy, 100)));
    MIMUSB_CHECK(refused(mimusb_node_endpoint_write(nullptr, 0x81, data, 1, 100)));
    MIMUSB_CHECK(refused(mimusb_node_endpoint_stall(nullptr, 0x81, 100)));
    MIMUSB_CHECK(refused(mimusb_node_endpoint_clear_stall(nullptr, 0x81, 100)));
    MIMUSB_CHECK(refused(mimusb_node_endpoint_flush(nullptr, 0x81, 100)));
    MIMUSB_CHECK(refused(mimusb_device_add_string(nullptr, "x", nullptr)));
    MIMUSB_CHECK(refused(mimusb_device_remove_string(nullptr, 5)));
    mimusb_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMUSB_CHECK(refused(mimusb_node_manifest(nullptr, &manifest)));
    MIMUSB_CHECK(refused(mimusb_node_submit(nullptr, 10)));
    MIMUSB_CHECK(refused(mimusb_node_submit_blocking(nullptr, 10)));
    MIMUSB_CHECK(refused(mimusb_keyboard_key(nullptr, 4, 1)));
    MIMUSB_CHECK(refused(mimusb_mouse_move(nullptr, 1, 1)));
    MIMUSB_CHECK(refused(mimusb_mouse_scroll(nullptr, 1, 0)));
    MIMUSB_CHECK(refused(mimusb_mouse_button(nullptr, 1, 1)));
    MIMUSB_CHECK(refused(mimusb_toggle_usage(nullptr, 0xE9, 1)));
    MIMUSB_CHECK(refused(mimusb_gamepad_button(nullptr, 1, 1)));
    MIMUSB_CHECK(refused(mimusb_gamepad_axis(nullptr, 0, 0)));
    MIMUSB_CHECK(refused(mimusb_gamepad_dpad(nullptr, 1, 0, 0, 0)));
    MIMUSB_CHECK(refused(mimusb_touchscreen_contact(nullptr, 0, 1, 0, 0, nullptr)));
    MIMUSB_CHECK(refused(mimusb_touchpad_contact(nullptr, 0, 1, 0, 0, nullptr)));
    MIMUSB_CHECK(refused(mimusb_touchpad_button(nullptr, 1, 1)));
    MIMUSB_CHECK(refused(mimusb_pen_update(nullptr, nullptr)));
    MIMUSB_CHECK(refused(mimusb_digitizer_update(nullptr, nullptr)));
    MIMUSB_CHECK(refused(mimusb_battery_update(nullptr, 50)));
    MIMUSB_CHECK(refused(mimusb_raw_submit(nullptr, data, 1, 10)));
    MIMUSB_CHECK(refused(mimusb_node_set_rule_data(nullptr, 0, data, 1, 100)));
    mimusb_control_rule rule{};
    MIMUSB_CHECK(refused(mimusb_hid_descriptor_rule(0, 0x22, nullptr, 1, &rule)));
    MIMUSB_CHECK(refused(mimusb_hid_descriptor_bytes(0x0111, 0, 0, nullptr)));
}

void test_spec_arguments() {
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(refused(mimusb_spec_create_keyboard(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_mouse(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_toggle(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_gamepad(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_touchscreen(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_touchpad(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_pen(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_digitizer(nullptr, &spec)));
    MIMUSB_CHECK(refused(mimusb_spec_create_raw(nullptr, &spec)));
    MIMUSB_CHECK(spec == nullptr);
    mimusb_spec_retain(nullptr); // a null Spec is ignored
    mimusb_spec_release(nullptr);
    const std::uint8_t* bytes = nullptr;
    std::size_t length = 0;
    MIMUSB_CHECK(refused(mimusb_spec_descriptor(nullptr, &bytes, &length)));
    mimusb_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMUSB_CHECK(refused(mimusb_spec_manifest(nullptr, &manifest)));
}

void test_discovery_arguments() {
    MIMUSB_CHECK(refused(mimusb_discover(100, nullptr)));
    MIMUSB_CHECK(mimusb_discovery_count(nullptr) == 0U &&
                 mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                 std::strcmp(mimusb_last_error()->field, "discovery") == 0);
    MIMUSB_CHECK(mimusb_discovery_get(nullptr, 0) == nullptr &&
                 mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                 std::strcmp(mimusb_last_error()->field, "discovery") == 0);
    mimusb_discovery_destroy(nullptr);
    MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                 std::strcmp(mimusb_last_error()->field, "discovery") == 0);
}

} // namespace

void test_api_arguments() {
    test_device_arguments();
    test_node_arguments();
    test_spec_arguments();
    test_discovery_arguments();
}
