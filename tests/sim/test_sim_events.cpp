// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Events from the board (mount, suspend, output and feature reports) and raw nodes, and the
 * loss of the serial link. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

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
    mimusb_device_config o = base_options();
    o.usb_config.bm_attributes |= 0x20;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    mimusb_spec* raw = raw_spec_io();
    mimusb_node* node = open_node(device, raw);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));

    // The target writes an Output report and reads a Feature report.
    const std::uint8_t out_data[] = {0x12};
    fake_board_target_set_report(0, 2, 5, out_data, sizeof(out_data));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_OUTPUT_REPORT, &ev));
    MIMUSB_CHECK(ev.node_id == 1 && ev.report_id == 5 && ev.data.size() == 1 && ev.data[0] == 0x12);
    const std::uint8_t feature[] = {0xAB, 0xCD};
    MIMUSB_CHECK(mimusb_node_set_feature_response(node, 5, feature, sizeof(feature), kControlMs) ==
                 MIMUSB_OK);
    std::uint8_t buffer[8] = {};
    MIMUSB_CHECK(fake_board_feature(0, 5, buffer, sizeof(buffer)) == 3 && buffer[0] == 5 &&
                 buffer[1] == 0xAB && buffer[2] == 0xCD); // the Report ID leads (HID 1.11 7.2.1)

    // Frames longer than 127 bytes carry a two-byte length: a Feature answer the target reads
    // whole, and a report the target writes.
    mimusb_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_OK &&
                 limits.max_feature_bytes == 1024 && limits.max_report_bytes == 64 &&
                 limits.max_endpoint_bytes == 64 && limits.max_endpoint_number == SIM_ENDPOINTS);
    limits.reserved = 1;
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_ERR_PARAM);
    limits.reserved = 0;
    std::vector<std::uint8_t> big(644);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    MIMUSB_CHECK(mimusb_node_set_feature_response(node, 5, big.data(), big.size(), kControlMs) ==
                 MIMUSB_OK);
    std::vector<std::uint8_t> got(1024);
    MIMUSB_CHECK(fake_board_feature(0, 5, got.data(), got.size()) == big.size() + 1U &&
                 got[0] == 5 && std::memcmp(got.data() + 1, big.data(), big.size()) == 0);
    MIMUSB_CHECK(mimusb_node_set_feature_response(node, 5, big.data(), 1025, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_set_feature_response(node, 5, feature, sizeof(feature), kControlMs) ==
                 MIMUSB_OK);
    fake_board_target_set_report(0, 2, 5, big.data(), 300);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_OUTPUT_REPORT, &ev));
    MIMUSB_CHECK(ev.report_id == 5 && ev.data.size() == 300 &&
                 std::memcmp(ev.data.data(), big.data(), 300) == 0);

    fake_board_target_set_protocol(0, 0);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_SET_PROTOCOL, &ev) && ev.node_id == 1 && ev.value == 0);
    fake_board_target_set_idle(0, 9);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_SET_IDLE, &ev) && ev.value == 9);

    // Remote wakeup works only while the target has the device suspended.
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, kControlMs) == MIMUSB_ERR_BUSY);
    fake_board_target_suspend(1);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_SUSPEND, &ev) && ev.value == 1);
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, kControlMs) == MIMUSB_OK);
    fake_board_target_resume();
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_RESUME, &ev));

    // Raw reports: exact length and Report ID only.
    fake_board_clear_reports();
    const std::uint8_t good[] = {5, 0x01};
    MIMUSB_CHECK(mimusb_raw_submit(node, good, sizeof(good), kSendMs) == MIMUSB_OK);
    fake_report_t r{};
    MIMUSB_CHECK(next_report(&r) && r.length == 2 && r.data[0] == 5 && r.data[1] == 1);
    const std::uint8_t bad[] = {6, 0x01};
    MIMUSB_CHECK(mimusb_raw_submit(node, bad, sizeof(bad), kSendMs) == MIMUSB_ERR_PARAM);

    // No wait has a default: a zero timeout is refused before anything is sent.
    const std::uint8_t feature_zero[] = {1};
    MIMUSB_CHECK(mimusb_raw_submit(node, good, sizeof(good), 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_set_feature_response(node, 5, feature_zero, sizeof(feature_zero), 0) ==
                 MIMUSB_ERR_UNSET_FIELD);
    const mimusb_commit_timeouts no_control = {sizeof(mimusb_commit_timeouts), 0, 0, kReconnectMs};
    const mimusb_commit_timeouts no_reconnect = {sizeof(mimusb_commit_timeouts), 0, kControlMs, 0};
    const mimusb_commit_timeouts wrong_size = {1, 0, kControlMs, kReconnectMs};
    const mimusb_commit_timeouts reserved_set = {sizeof(mimusb_commit_timeouts), 1, kControlMs,
                                                 kReconnectMs};
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &reserved_set) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_close(node, &no_control, kDrainMs) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_close(node, &no_reconnect, kDrainMs) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_close(node, &wrong_size, kDrainMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_close(node, nullptr, kDrainMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, 0) == MIMUSB_ERR_UNSET_FIELD);
    mimusb_node* refused = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, nullptr, &no_control, &refused) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_open(device, nullptr, &no_reconnect, &refused) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &no_control) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &no_reconnect) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &wrong_size) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_connect(device, &no_control) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &no_reconnect) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, nullptr, nullptr, &no_control) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_disconnect(device, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, nullptr, 0, 0) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_control_reply(device, 1, MIMUSB_CONTROL_ACTION_STALL, nullptr, 0,
                                             0) == MIMUSB_ERR_UNSET_FIELD);
    // A refused close consumes nothing.
    MIMUSB_CHECK(mimusb_device_close(device, 0, kDrainMs) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, 0) == MIMUSB_ERR_UNSET_FIELD);

    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(raw);
}

void test_link_loss() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_node* node = open_node(device, kbd);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    // The board end of the cable disappears.
    close(g_master);
    g_master = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    mimusb_result r = MIMUSB_OK;
    while (std::chrono::steady_clock::now() < deadline) {
        r = mimusb_device_latched_error(device);
        if (r == MIMUSB_ERR_NO_DEVICE)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MIMUSB_CHECK(r == MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_keyboard_key(node, 0x04, 1) == MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_device_is_ready(device) == 0);
    // A close that fails on the way out still takes the Node.
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_ERR_NO_DEVICE);
    mimusb_spec_release(kbd);
}

} // namespace sim
