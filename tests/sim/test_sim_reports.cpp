// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Reports on the wire: neutral reports on close, the board's queue depth and loss reporting,
 * the touch feature report, batched commits and packing, and the board's limits. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sim {

namespace {

// Presses and releases a key in turn, count times, one report each (an unchanged state would
// send nothing).
void queue_key_reports(mimhid_node* node, int count) {
    static bool down = false;
    for (int i = 0; i < count; ++i) {
        down = !down;
        MIMHID_CHECK(mimhid_keyboard_key(node, 0x04, down ? 1 : 0) == MIMHID_OK);
        MIMHID_CHECK(mimhid_node_submit_blocking(node, kSendMs) == MIMHID_OK);
    }
}

std::uint32_t lost_reports(std::uint16_t node_id, int timeout_ms) {
    std::uint32_t lost = 0;
    EventRecord e{};
    while (wait_event(MIMHID_EVENT_TRANSFER_LOST, &e, timeout_ms)) {
        MIMHID_CHECK(e.node_id == node_id);
        lost += e.value;
    }
    return lost;
}

} // namespace

void test_reports_and_neutral() {
    mimhid_device_config o = base_options();
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_spec* mouse = mouse_spec(2);
    mimhid_node* nk = open_node(device, kbd);
    mimhid_node* nm = open_node(device, mouse);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    fake_board_clear_reports();

    mimhid_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMHID_CHECK(mimhid_node_manifest(nk, &manifest) == MIMHID_OK);
    const std::uint32_t kbd_bytes = manifest.reports[0].wire_length;

    // Key down, then up.
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x04, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk, kSendMs) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r));
    MIMHID_CHECK(r.data[0] == 1 && r.length == kbd_bytes && !all_zero_after_id(r, true));
    MIMHID_CHECK(mimhid_node_submit(nk, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_submit_blocking(nk, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x04, 0) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && all_zero_after_id(r, true));

    // Mouse motion lands in the 16-bit X and Y fields.
    MIMHID_CHECK(mimhid_mouse_move(nm, 5, -3) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nm, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && r.data[0] == 2);
    bool seen_x = false, seen_y = false;
    for (std::size_t i = 1; i + 1 < r.length; ++i) {
        seen_x = seen_x || (r.data[i] == 0x05 && r.data[i + 1] == 0x00);
        seen_y = seen_y || (r.data[i] == 0xFD && r.data[i + 1] == 0xFF);
    }
    MIMHID_CHECK(seen_x && seen_y);

    // Closing a node with held input sends the neutral report first.
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x05, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && !all_zero_after_id(r, true));
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_node_close(nk, &kCommit, kDrainMs) == MIMHID_OK);
    // The release was sent before the board re-enumerated without the keyboard.
    MIMHID_CHECK(next_report(&r) && r.data[0] == 1 && all_zero_after_id(r, true));
    MIMHID_CHECK(fake_board_interface_count() == 1);

    // Closing the Device detaches the last node.
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 0);
    mimhid_spec_release(kbd);
    mimhid_spec_release(mouse);
}

void test_queue_depth_and_loss() {
    mimhid_device_config o = base_options();
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_node* nk = open_node(device, kbd);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    const std::uint16_t id = mimhid_node_id(nk);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }

    // Arguments.
    const auto set_depth = [&](mimhid_node* node, std::uint8_t depth, std::uint32_t timeout) {
        const mimhid_endpoint_policy policy{MIMHID_FULL_DROP_NEWEST, depth};
        return mimhid_node_endpoint_set_policy(node, 0x81, &policy, timeout);
    };
    MIMHID_CHECK(set_depth(nullptr, 1, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(set_depth(nk, 0, kControlMs) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(set_depth(nk, 1, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(set_depth(nk, 255, kControlMs) == MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(nullptr, 1, 20, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, 20, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 2, 20, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 0, 20, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, MIMHID_LOSS_MAX_INTERVAL_MS + 1,
                                                  kControlMs) == MIMHID_ERR_PARAM);

    // Off by default: a full queue drops silently, and nothing extra crosses the link.
    MIMHID_CHECK(set_depth(nk, 2, kControlMs) == MIMHID_OK);
    fake_board_clear_reports();
    fake_board_hold_reports(1);
    queue_key_reports(nk, 5);
    MIMHID_CHECK(lost_reports(id, 100) == 0);
    fake_board_hold_reports(0);
    fake_report_t r{};
    // The queue holds 2; a report the endpoint took before the others came is in flight besides,
    // which depends on when the firmware loop ran, so 2 or 3 arrive.
    const auto count_reports = [&] {
        int n = 0;
        while (next_report(&r, n == 0 ? 2000 : 100))
            ++n;
        return n;
    };
    int arrived = count_reports();
    MIMHID_CHECK(arrived == 2 || arrived == 3);

    // Asked for: what did not fit is reported, and what arrived with it is all five.
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, 0, kControlMs) == MIMHID_OK);
    fake_board_clear_reports();
    fake_board_hold_reports(1);
    queue_key_reports(nk, 5);
    const std::uint32_t lost = lost_reports(id, 200);
    fake_board_hold_reports(0);
    arrived = count_reports();
    MIMHID_CHECK((arrived == 2 || arrived == 3) && lost + static_cast<unsigned>(arrived) == 5U);

    // The depth stays with the node when another node's commit rebuilds the interfaces.
    mimhid_spec* mouse = mouse_spec(2);
    mimhid_node* nm = open_node(device, mouse);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    fake_board_clear_reports();
    fake_board_hold_reports(1);
    queue_key_reports(nk, 4);
    const std::uint32_t lost_after = lost_reports(mimhid_node_id(nk), 200);
    fake_board_hold_reports(0);
    arrived = count_reports();
    MIMHID_CHECK((arrived == 2 || arrived == 3) && lost_after + static_cast<unsigned>(arrived) == 4U);

    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 0, 0, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    mimhid_spec_release(kbd);
    mimhid_spec_release(mouse);
    (void)nm;
}

void test_touch_feature() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* touch = touch_spec(3, 1);
    mimhid_node* node = open_node(device, touch);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    std::uint8_t buffer[8] = {};
    MIMHID_CHECK(fake_board_feature(0, 3, buffer, sizeof(buffer)) == 2 && buffer[0] == 3 &&
                 buffer[1] == 3); // the Report ID leads, then the maximum contact count

    // A touch frame: contact down, then lifted.
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 0, 1, 100, 200, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.data[0] == 3);
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 0, 0, 100, 200, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r));

    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    mimhid_spec_release(touch);
}

void test_batch_and_packing() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* specs[4] = {keyboard_spec(1), mouse_spec(2), touch_spec(3, 0), pen_spec(4)};
    mimhid_node* nodes[4] = {};
    const unsigned connects_before = fake_board_connects();
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_ERR_BUSY);
    for (int i = 0; i < 4; ++i) {
        mimhid_hid_node_config opt{};
        test_fill_node_config(&opt, static_cast<uint8_t>(i % SIM_ENDPOINTS));
        nodes[i] = open_node(device, specs[i], &opt);
    }
    // Nothing is committed inside the batch, and staged nodes refuse reports.
    MIMHID_CHECK(fake_board_connects() == connects_before);
    MIMHID_CHECK(mimhid_keyboard_key(nodes[0], 0x04, 1) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_node_submit(nodes[0], kSendMs) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(fake_board_connects() == connects_before + 1);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);

    // Four nodes need four interfaces, or are packed when the board has fewer.
    const unsigned expected = SIM_ENDPOINTS >= 4 ? 4 : SIM_ENDPOINTS;
    MIMHID_CHECK(fake_board_interface_count() == expected);
    std::size_t total = 0;
    for (unsigned i = 0; i < expected; ++i) {
        uint16_t length = 0;
        fake_board_report_descriptor(static_cast<std::uint8_t>(i), &length);
        total += length;
    }
    std::size_t expected_total = 0;
    for (auto* spec : specs) {
        const std::uint8_t* bytes = nullptr;
        size_t length = 0;
        MIMHID_CHECK(mimhid_spec_descriptor(spec, &bytes, &length) == MIMHID_OK);
        expected_total += length;
    }
    MIMHID_CHECK(total == expected_total);

    // Reports of packed nodes all arrive, each with its own Report ID.
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_keyboard_key(nodes[0], 0x04, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nodes[0], kSendMs) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.data[0] == 1);
    MIMHID_CHECK(mimhid_mouse_move(nodes[1], 1, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nodes[1], kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && r.data[0] == 2);

    // Closing inside a batch only stages the change.
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    const unsigned connects = fake_board_connects();
    for (auto* node : nodes)
        MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_connects() == connects);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 0);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    for (auto* spec : specs)
        mimhid_spec_release(spec);
}

void test_limits() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    // Nodes without Report IDs cannot share an interface, so the board's interface count is a hard
    // limit.
    std::vector<mimhid_node*> nodes;
    std::vector<mimhid_spec*> specs;
    mimhid_result last = MIMHID_OK;
    const unsigned limit = SIM_ENDPOINTS;
    for (unsigned i = 0; i < limit + 1; ++i) {
        specs.push_back(raw_spec_no_id());
        mimhid_hid_node_config no{};
        test_fill_node_config(&no, static_cast<uint8_t>(i));
        no.ep_in.policy.depth = 1; // the queues of all the IN endpoints share the board's pool
        mimhid_node* node = nullptr;
        last = mimhid_node_open_profile(device, specs.back(), &no, &kCommit, &node);
        if (last != MIMHID_OK)
            break;
        nodes.push_back(node);
    }
    MIMHID_CHECK(nodes.size() == limit && last == MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(fake_board_interface_count() == limit);
    // The failed open left the others working.
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    // Interface numbers are real: the highest goes first, or the others would have a gap.
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        MIMHID_CHECK(mimhid_node_close(*it, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    for (auto* spec : specs)
        mimhid_spec_release(spec);
}

// Interface numbers are real: a number that leaves a gap is refused by the library before the
// board sees it, and the board, given such a configuration anyway, refuses the commit
// (firmware/tests covers that). Here the whole path: 0, then a gap, then the gap filled.
void test_interface_numbers() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    if (SIM_ENDPOINTS < 3) {
        MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
        return;
    }
    std::vector<mimhid_spec*> specs;
    mimhid_node* nodes[3] = {};
    for (unsigned i : {0U, 2U, 1U, 2U}) {
        specs.push_back(raw_spec_no_id());
        mimhid_hid_node_config c{};
        test_fill_node_config(&c, static_cast<uint8_t>(i));
        mimhid_node* node = nullptr;
        const mimhid_result r = mimhid_node_open_profile(device, specs.back(), &c, &kCommit, &node);
        if (i == 2U && nodes[1] == nullptr) {
            MIMHID_CHECK(r == MIMHID_ERR_PARAM && node == nullptr &&
                         mimhid_last_error()->offset == 1U);
        } else {
            MIMHID_CHECK(r == MIMHID_OK);
            nodes[i] = node;
        }
    }
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 3U);
    // The target sees interface numbers 0, 1, 2 in its configuration descriptor.
    uint16_t length = 0;
    const uint8_t* c = fake_board_config_descriptor(&length);
    unsigned seen = 0;
    for (uint16_t k = 0; k + 1U < length; k = static_cast<uint16_t>(k + c[k])) {
        if (c[k + 1] == 0x04) {
            MIMHID_CHECK(c[k + 2] == seen);
            ++seen;
        }
    }
    MIMHID_CHECK(seen == 3U);
    for (int i = 2; i >= 0; --i)
        MIMHID_CHECK(mimhid_node_close(nodes[i], &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    for (auto* spec : specs)
        mimhid_spec_release(spec);
}

} // namespace sim
