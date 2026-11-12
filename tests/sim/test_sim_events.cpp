// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Events from the board (mount, suspend, output and feature reports) and raw nodes, and the
 * loss of the serial link. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sim {

void test_events_and_raw() {
    mimhid_device_config o = base_options();
    o.usb_config.bm_attributes |= 0x20;
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    mimhid_spec* raw = raw_spec_io();
    mimhid_node* node = open_node(device, raw);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));

    // The target writes an Output report and reads a Feature report.
    const std::uint8_t out_data[] = {0x12};
    fake_board_target_set_report(0, 2, 5, out_data, sizeof(out_data));
    MIMHID_CHECK(wait_event(MIMHID_EVENT_OUTPUT_REPORT, &ev));
    MIMHID_CHECK(ev.node_id == 1 && ev.report_id == 5 && ev.data.size() == 1 && ev.data[0] == 0x12);
    const std::uint8_t feature[] = {0xAB, 0xCD};
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, feature, sizeof(feature), kControlMs) ==
                 MIMHID_OK);
    std::uint8_t buffer[8] = {};
    MIMHID_CHECK(fake_board_feature(0, 5, buffer, sizeof(buffer)) == 3 && buffer[0] == 5 &&
                 buffer[1] == 0xAB && buffer[2] == 0xCD); // the Report ID leads (HID 1.11 7.2.1)

    // Frames longer than 127 bytes carry a two-byte length: a Feature answer the target reads
    // whole, and a report the target writes.
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_feature_bytes == 1024 && limits.max_report_bytes == 64 &&
                 limits.max_endpoint_bytes == 64 && limits.max_endpoint_number == SIM_ENDPOINTS);
    limits.reserved = 1;
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_ERR_PARAM);
    limits.reserved = 0;
    std::vector<std::uint8_t> big(644);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, big.data(), big.size(), kControlMs) ==
                 MIMHID_OK);
    std::vector<std::uint8_t> got(1024);
    MIMHID_CHECK(fake_board_feature(0, 5, got.data(), got.size()) == big.size() + 1U &&
                 got[0] == 5 && std::memcmp(got.data() + 1, big.data(), big.size()) == 0);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, big.data(), 1025, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, feature, sizeof(feature), kControlMs) ==
                 MIMHID_OK);
    fake_board_target_set_report(0, 2, 5, big.data(), 300);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_OUTPUT_REPORT, &ev));
    MIMHID_CHECK(ev.report_id == 5 && ev.data.size() == 300 &&
                 std::memcmp(ev.data.data(), big.data(), 300) == 0);

    fake_board_target_set_protocol(0, 0);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SET_PROTOCOL, &ev) && ev.node_id == 1 && ev.value == 0);
    fake_board_target_set_idle(0, 9);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SET_IDLE, &ev) && ev.value == 9);

    // Remote wakeup works only while the target has the device suspended.
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, kControlMs) == MIMHID_ERR_BUSY);
    fake_board_target_suspend(1);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SUSPEND, &ev) && ev.value == 1);
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, kControlMs) == MIMHID_OK);
    fake_board_target_resume();
    MIMHID_CHECK(wait_event(MIMHID_EVENT_RESUME, &ev));

    // Raw reports: exact length and Report ID only.
    fake_board_clear_reports();
    const std::uint8_t good[] = {5, 0x01};
    MIMHID_CHECK(mimhid_raw_submit(node, good, sizeof(good), kSendMs) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.length == 2 && r.data[0] == 5 && r.data[1] == 1);
    const std::uint8_t bad[] = {6, 0x01};
    MIMHID_CHECK(mimhid_raw_submit(node, bad, sizeof(bad), kSendMs) == MIMHID_ERR_PARAM);

    // No wait has a default: a zero timeout is refused before anything is sent.
    const std::uint8_t feature_zero[] = {1};
    MIMHID_CHECK(mimhid_raw_submit(node, good, sizeof(good), 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, feature_zero, sizeof(feature_zero), 0) ==
                 MIMHID_ERR_UNSET_FIELD);
    const mimhid_commit_timeouts no_control = {sizeof(mimhid_commit_timeouts), 0, 0, kReconnectMs};
    const mimhid_commit_timeouts no_reconnect = {sizeof(mimhid_commit_timeouts), 0, kControlMs, 0};
    const mimhid_commit_timeouts wrong_size = {1, 0, kControlMs, kReconnectMs};
    const mimhid_commit_timeouts reserved_set = {sizeof(mimhid_commit_timeouts), 1, kControlMs,
                                                 kReconnectMs};
    MIMHID_CHECK(mimhid_device_commit(device, 0, &reserved_set) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_close(node, &no_control, kDrainMs) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_close(node, &no_reconnect, kDrainMs) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_close(node, &wrong_size, kDrainMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_close(node, nullptr, kDrainMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_close(node, &kCommit, 0) == MIMHID_ERR_UNSET_FIELD);
    mimhid_node* refused = nullptr;
    MIMHID_CHECK(mimhid_node_open_profile(device, raw, nullptr, &no_control, &refused) ==
                 MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_open(device, nullptr, &no_reconnect, &refused) ==
                 MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &no_control) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &no_reconnect) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &wrong_size) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_connect(device, &no_control) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_batch_end(device, &no_reconnect) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, nullptr, nullptr, &no_control) ==
                 MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_disconnect(device, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, nullptr, 0, 0) ==
                 MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, MIMHID_CONTROL_ACTION_STALL, nullptr, 0,
                                             0) == MIMHID_ERR_UNSET_FIELD);
    // A refused close consumes nothing.
    MIMHID_CHECK(mimhid_device_close(device, 0, kDrainMs) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, 0) == MIMHID_ERR_UNSET_FIELD);

    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    mimhid_spec_release(raw);
}

void test_link_loss() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_node* node = open_node(device, kbd);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    // The board end of the cable disappears.
    close(g_master);
    g_master = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    mimhid_result r = MIMHID_OK;
    while (std::chrono::steady_clock::now() < deadline) {
        r = mimhid_device_latched_error(device);
        if (r == MIMHID_ERR_NO_DEVICE)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MIMHID_CHECK(r == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_keyboard_key(node, 0x04, 1) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 0);
    // A close that fails on the way out still takes the Node.
    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_ERR_NO_DEVICE);
    mimhid_spec_release(kbd);
}

} // namespace sim
