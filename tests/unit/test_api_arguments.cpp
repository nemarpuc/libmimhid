// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The C ABI's argument checks that need no board: every entry point given a null handle, a null
 * output or an empty discovery returns its documented error (or a harmless value) and says
 * which argument was wrong. The checks with a board are in tests/scripted. */
#include "mimhid.h"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

bool refused(const mimhid_result result, const mimhid_result expected = MIMHID_ERR_PARAM) {
    return result == expected && mimhid_last_error()->code == expected &&
           mimhid_last_error()->field != nullptr;
}

void test_device_arguments() {
    mimhid_device_config config{};
    mimhid_device* device = nullptr;
    MIMHID_CHECK(refused(mimhid_device_open(nullptr, &config, &device)));
    MIMHID_CHECK(refused(mimhid_device_open("/dev/null", nullptr, &device)));
    MIMHID_CHECK(refused(mimhid_device_open("/dev/null", &config, nullptr)));
    MIMHID_CHECK(device == nullptr);
    const mimhid_commit_timeouts timeouts{sizeof(mimhid_commit_timeouts), 0, 100, 1000};
    MIMHID_CHECK(refused(mimhid_device_close(nullptr, 100, 100)));
    MIMHID_CHECK(refused(mimhid_device_set_usb_identity(nullptr, nullptr, nullptr, &timeouts)));
    MIMHID_CHECK(refused(mimhid_device_latched_error(nullptr)) &&
                 std::strcmp(mimhid_last_error()->field, "device") == 0);
    MIMHID_CHECK(mimhid_device_is_ready(nullptr) == 0);
    MIMHID_CHECK(refused(mimhid_device_wait_ready(nullptr, 10)));
    MIMHID_CHECK(refused(mimhid_device_batch_begin(nullptr)));
    MIMHID_CHECK(refused(mimhid_device_batch_end(nullptr, &timeouts)));
    MIMHID_CHECK(refused(mimhid_device_commit(nullptr, 0, &timeouts)));
    MIMHID_CHECK(refused(mimhid_device_connect(nullptr, &timeouts)));
    MIMHID_CHECK(refused(mimhid_device_disconnect(nullptr, 100)));
    MIMHID_CHECK(refused(mimhid_device_remote_wakeup(nullptr, 100)));
    MIMHID_CHECK(refused(mimhid_device_enter_bootloader(nullptr, 100)));
    mimhid_device_info info{};
    mimhid_board_limits limits{};
    mimhid_link_counters counters{};
    MIMHID_CHECK(refused(mimhid_device_board_info(nullptr, &info)));
    MIMHID_CHECK(refused(mimhid_device_limits(nullptr, &limits)));
    MIMHID_CHECK(refused(mimhid_device_link_counters(nullptr, 0, 100, &counters)));
    MIMHID_CHECK(refused(mimhid_device_set_control_routing(nullptr, 0, nullptr, 0, 100)));
    MIMHID_CHECK(refused(
        mimhid_device_control_reply(nullptr, 1, MIMHID_CONTROL_ACTION_LEAVE, nullptr, 0, 10)));
    MIMHID_CHECK(refused(mimhid_device_set_loss_reporting(nullptr, 0, 0, 100)));
}

void test_node_arguments() {
    const mimhid_commit_timeouts timeouts{sizeof(mimhid_commit_timeouts), 0, 100, 1000};
    MIMHID_CHECK(refused(mimhid_node_close(nullptr, &timeouts, 100)));
    MIMHID_CHECK(mimhid_node_id(nullptr) == 0U && mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                 std::strcmp(mimhid_last_error()->field, "node") == 0);
    const std::uint8_t data[1] = {0};
    MIMHID_CHECK(refused(mimhid_node_set_feature_response(nullptr, 0, data, 1, 100)));
    const mimhid_endpoint_policy policy{MIMHID_FULL_DROP_NEWEST, 1};
    MIMHID_CHECK(refused(mimhid_node_endpoint_set_policy(nullptr, 0x81, &policy, 100)));
    MIMHID_CHECK(refused(mimhid_node_endpoint_write(nullptr, 0x81, data, 1, 100)));
    MIMHID_CHECK(refused(mimhid_node_endpoint_stall(nullptr, 0x81, 100)));
    MIMHID_CHECK(refused(mimhid_node_endpoint_clear_stall(nullptr, 0x81, 100)));
    MIMHID_CHECK(refused(mimhid_node_endpoint_flush(nullptr, 0x81, 100)));
    MIMHID_CHECK(refused(mimhid_device_add_string(nullptr, "x", nullptr)));
    MIMHID_CHECK(refused(mimhid_device_remove_string(nullptr, 5)));
    mimhid_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMHID_CHECK(refused(mimhid_node_manifest(nullptr, &manifest)));
    MIMHID_CHECK(refused(mimhid_node_submit(nullptr, 10)));
    MIMHID_CHECK(refused(mimhid_node_submit_blocking(nullptr, 10)));
    MIMHID_CHECK(refused(mimhid_keyboard_key(nullptr, 4, 1)));
    MIMHID_CHECK(refused(mimhid_mouse_move(nullptr, 1, 1)));
    MIMHID_CHECK(refused(mimhid_mouse_scroll(nullptr, 1, 0)));
    MIMHID_CHECK(refused(mimhid_mouse_button(nullptr, 1, 1)));
    MIMHID_CHECK(refused(mimhid_toggle_usage(nullptr, 0xE9, 1)));
    MIMHID_CHECK(refused(mimhid_gamepad_button(nullptr, 1, 1)));
    MIMHID_CHECK(refused(mimhid_gamepad_axis(nullptr, 0, 0)));
    MIMHID_CHECK(refused(mimhid_gamepad_dpad(nullptr, 1, 0, 0, 0)));
    MIMHID_CHECK(refused(mimhid_touchscreen_contact(nullptr, 0, 1, 0, 0, nullptr)));
    MIMHID_CHECK(refused(mimhid_touchpad_contact(nullptr, 0, 1, 0, 0, nullptr)));
    MIMHID_CHECK(refused(mimhid_touchpad_button(nullptr, 1, 1)));
    MIMHID_CHECK(refused(mimhid_pen_update(nullptr, nullptr)));
    MIMHID_CHECK(refused(mimhid_digitizer_update(nullptr, nullptr)));
    MIMHID_CHECK(refused(mimhid_battery_update(nullptr, 50)));
    MIMHID_CHECK(refused(mimhid_raw_submit(nullptr, data, 1, 10)));
    MIMHID_CHECK(refused(mimhid_report_send(nullptr, data, 1, 10)));
}

void test_spec_arguments() {
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(refused(mimhid_spec_create_keyboard(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_mouse(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_toggle(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_gamepad(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_touchscreen(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_touchpad(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_pen(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_digitizer(nullptr, &spec)));
    MIMHID_CHECK(refused(mimhid_spec_create_raw(nullptr, &spec)));
    MIMHID_CHECK(spec == nullptr);
    mimhid_spec_retain(nullptr); // a null Spec is ignored
    mimhid_spec_release(nullptr);
    const std::uint8_t* bytes = nullptr;
    std::size_t length = 0;
    MIMHID_CHECK(refused(mimhid_spec_descriptor(nullptr, &bytes, &length)));
    mimhid_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMHID_CHECK(refused(mimhid_spec_manifest(nullptr, &manifest)));
}

void test_discovery_arguments() {
    MIMHID_CHECK(refused(mimhid_discover(100, nullptr)));
    MIMHID_CHECK(mimhid_discovery_count(nullptr) == 0U &&
                 mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                 std::strcmp(mimhid_last_error()->field, "discovery") == 0);
    MIMHID_CHECK(mimhid_discovery_get(nullptr, 0) == nullptr &&
                 mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                 std::strcmp(mimhid_last_error()->field, "discovery") == 0);
    mimhid_discovery_destroy(nullptr);
    MIMHID_CHECK(mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                 std::strcmp(mimhid_last_error()->field, "discovery") == 0);
}

} // namespace

void test_api_arguments() {
    test_device_arguments();
    test_node_arguments();
    test_spec_arguments();
    test_discovery_arguments();
}
