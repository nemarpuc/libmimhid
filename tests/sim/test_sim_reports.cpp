// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Reports on the wire: neutral reports on close, the board's queue depth and loss reporting,
 * the touch feature report, batched commits and packing, and the board's limits. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

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
void queue_key_reports(mimusb_node* node, int count) {
    static bool down = false;
    for (int i = 0; i < count; ++i) {
        down = !down;
        MIMUSB_CHECK(mimusb_keyboard_key(node, 0x04, down ? 1 : 0) == MIMUSB_OK);
        MIMUSB_CHECK(mimusb_node_submit_blocking(node, kSendMs) == MIMUSB_OK);
    }
}

// Holds the target (it polls nothing), sends the first report and waits until the board has given
// it to the controller, where it stays in flight, then sends the rest. The first report is then
// always the one in flight and the others meet the queue: with a depth of d, 1 + d reports arrive
// once the target reads again, whatever the order the firmware loop and the host ran in.
void send_held(mimusb_node* node, int count) {
    fake_board_hold_reports(1);
    queue_key_reports(node, 1);
    for (int i = 0; i < 2000 && fake_board_endpoint_busy(0x81) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MIMUSB_CHECK(fake_board_endpoint_busy(0x81) == 1);
    queue_key_reports(node, count - 1);
}

std::uint32_t lost_reports(std::uint16_t node_id, int timeout_ms) {
    std::uint32_t lost = 0;
    EventRecord e{};
    while (wait_event(MIMUSB_EVENT_TRANSFER_LOST, &e, timeout_ms)) {
        MIMUSB_CHECK(e.node_id == node_id);
        lost += e.value;
    }
    return lost;
}

} // namespace

void test_reports_and_neutral() {
    mimusb_device_config o = base_options();
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_spec* mouse = mouse_spec(2);
    mimusb_node* nk = open_node(device, kbd);
    mimusb_node* nm = open_node(device, mouse);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    fake_board_clear_reports();

    mimusb_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMUSB_CHECK(mimusb_node_manifest(nk, &manifest) == MIMUSB_OK);
    const std::uint32_t kbd_bytes = manifest.reports[0].wire_length;

    // Key down, then up.
    MIMUSB_CHECK(mimusb_keyboard_key(nk, 0x04, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nk, kSendMs) == MIMUSB_OK);
    fake_report_t r{};
    MIMUSB_CHECK(next_report(&r));
    MIMUSB_CHECK(r.data[0] == 1 && r.length == kbd_bytes && !all_zero_after_id(r, true));
    MIMUSB_CHECK(mimusb_node_submit(nk, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_submit_blocking(nk, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_keyboard_key(nk, 0x04, 0) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nk, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(next_report(&r) && all_zero_after_id(r, true));

    // Mouse motion lands in the 16-bit X and Y fields.
    MIMUSB_CHECK(mimusb_mouse_move(nm, 5, -3) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nm, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 2);
    bool seen_x = false, seen_y = false;
    for (std::size_t i = 1; i + 1 < r.length; ++i) {
        seen_x = seen_x || (r.data[i] == 0x05 && r.data[i + 1] == 0x00);
        seen_y = seen_y || (r.data[i] == 0xFD && r.data[i + 1] == 0xFF);
    }
    MIMUSB_CHECK(seen_x && seen_y);

    // Closing a node with held input sends the neutral report first.
    MIMUSB_CHECK(mimusb_keyboard_key(nk, 0x05, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nk, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(next_report(&r) && !all_zero_after_id(r, true));
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_node_close(nk, &kCommit, kDrainMs) == MIMUSB_OK);
    // The release was sent before the board re-enumerated without the keyboard.
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 1 && all_zero_after_id(r, true));
    MIMUSB_CHECK(fake_board_interface_count() == 1);

    // Closing the Device detaches the last node.
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 0);
    mimusb_spec_release(kbd);
    mimusb_spec_release(mouse);
}

void test_queue_depth_and_loss() {
    mimusb_device_config o = base_options();
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_node* nk = open_node(device, kbd);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    const std::uint16_t id = mimusb_node_id(nk);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }

    // Arguments.
    const auto set_depth = [&](mimusb_node* node, std::uint8_t depth, std::uint32_t timeout) {
        const mimusb_endpoint_policy policy{MIMUSB_FULL_DROP_NEWEST, depth};
        return mimusb_node_endpoint_set_policy(node, 0x81, &policy, timeout);
    };
    MIMUSB_CHECK(set_depth(nullptr, 1, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(set_depth(nk, 0, kControlMs) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(set_depth(nk, 1, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(set_depth(nk, 255, kControlMs) == MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(nullptr, 1, 20, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, 20, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 2, 20, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 0, 20, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, MIMUSB_LOSS_MAX_INTERVAL_MS + 1,
                                                  kControlMs) == MIMUSB_ERR_PARAM);

    // Off by default: a full queue drops silently, and nothing extra crosses the link.
    MIMUSB_CHECK(set_depth(nk, 2, kControlMs) == MIMUSB_OK);
    fake_board_clear_reports();
    send_held(nk, 5);
    MIMUSB_CHECK(lost_reports(id, 100) == 0);
    fake_board_hold_reports(0);
    fake_report_t r{};
    // The first report is in flight and the queue holds 2: three arrive, the others were dropped.
    const auto count_reports = [&] {
        int n = 0;
        while (next_report(&r, n == 0 ? 2000 : 100))
            ++n;
        return n;
    };
    int arrived = count_reports();
    MIMUSB_CHECK(arrived == 3);

    // Asked for: what did not fit is reported, and what arrived with it is all five.
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, 0, kControlMs) == MIMUSB_OK);
    fake_board_clear_reports();
    send_held(nk, 5);
    const std::uint32_t lost = lost_reports(id, 200);
    fake_board_hold_reports(0);
    arrived = count_reports();
    MIMUSB_CHECK(arrived == 3 && lost == 2U);

    // The depth stays with the node when another node's commit rebuilds the interfaces.
    mimusb_spec* mouse = mouse_spec(2);
    mimusb_node* nm = open_node(device, mouse);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    fake_board_clear_reports();
    send_held(nk, 4);
    const std::uint32_t lost_after = lost_reports(mimusb_node_id(nk), 200);
    fake_board_hold_reports(0);
    arrived = count_reports();
    MIMUSB_CHECK(arrived == 3 && lost_after == 1U);

    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 0, 0, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
    mimusb_spec_release(mouse);
    (void)nm;
}

void test_touch_feature() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* touch = touch_spec(3, 1);
    mimusb_node* node = open_node(device, touch);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    std::uint8_t buffer[8] = {};
    MIMUSB_CHECK(fake_board_feature(0, 3, buffer, sizeof(buffer)) == 2 && buffer[0] == 3 &&
                 buffer[1] == 3); // the Report ID leads, then the maximum contact count

    // A touch frame: contact down, then lifted.
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_touchscreen_contact(node, 0, 1, 100, 200, nullptr) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit_blocking(node, 1000) == MIMUSB_OK);
    fake_report_t r{};
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 3);
    MIMUSB_CHECK(mimusb_touchscreen_contact(node, 0, 0, 100, 200, nullptr) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit_blocking(node, 1000) == MIMUSB_OK);
    MIMUSB_CHECK(next_report(&r));

    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(touch);
}

void test_batch_and_packing() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* specs[4] = {keyboard_spec(1), mouse_spec(2), touch_spec(3, 0), pen_spec(4)};
    mimusb_node* nodes[4] = {};
    const unsigned connects_before = fake_board_connects();
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_ERR_BUSY);
    for (int i = 0; i < 4; ++i) {
        test_hid_node_config opt{};
        test_fill_node_config(&opt, static_cast<uint8_t>(i % SIM_ENDPOINTS));
        nodes[i] = open_node(device, specs[i], &opt);
    }
    // Nothing is committed inside the batch, and staged nodes refuse reports.
    MIMUSB_CHECK(fake_board_connects() == connects_before);
    MIMUSB_CHECK(mimusb_keyboard_key(nodes[0], 0x04, 1) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_node_submit(nodes[0], kSendMs) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(fake_board_connects() == connects_before + 1);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);

    // Four nodes need four interfaces, or are packed when the board has fewer.
    const unsigned expected = SIM_ENDPOINTS >= 4 ? 4 : SIM_ENDPOINTS;
    MIMUSB_CHECK(fake_board_interface_count() == expected);
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
        MIMUSB_CHECK(mimusb_spec_descriptor(spec, &bytes, &length) == MIMUSB_OK);
        expected_total += length;
    }
    MIMUSB_CHECK(total == expected_total);

    // Reports of packed nodes all arrive, each with its own Report ID.
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_keyboard_key(nodes[0], 0x04, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nodes[0], kSendMs) == MIMUSB_OK);
    fake_report_t r{};
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 1);
    MIMUSB_CHECK(mimusb_mouse_move(nodes[1], 1, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nodes[1], kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 2);

    // Closing inside a batch only stages the change.
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    const unsigned connects = fake_board_connects();
    for (auto* node : nodes)
        MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_connects() == connects);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 0);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    for (auto* spec : specs)
        mimusb_spec_release(spec);
}

void test_limits() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    // Nodes without Report IDs cannot share an interface, so the board's interface count is a hard
    // limit.
    std::vector<mimusb_node*> nodes;
    std::vector<mimusb_spec*> specs;
    mimusb_result last = MIMUSB_OK;
    const unsigned limit = SIM_ENDPOINTS;
    for (unsigned i = 0; i < limit + 1; ++i) {
        specs.push_back(hid_spec_no_id());
        test_hid_node_config no{};
        test_fill_node_config(&no, static_cast<uint8_t>(i));
        no.ep_in.policy.depth = 1; // the queues of all the IN endpoints share the board's pool
        mimusb_node* node = nullptr;
        last = test_node_open_hid(device, specs.back(), &no, &kCommit, &node);
        if (last != MIMUSB_OK)
            break;
        nodes.push_back(node);
    }
    MIMUSB_CHECK(nodes.size() == limit && last == MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(fake_board_interface_count() == limit);
    // The failed open left the others working.
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    // Interface numbers are real: the highest goes first, or the others would have a gap.
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        MIMUSB_CHECK(mimusb_node_close(*it, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    for (auto* spec : specs)
        mimusb_spec_release(spec);
}

// Interface numbers are real: a number that leaves a gap is refused by the library before the
// board sees it, and the board, given such a configuration anyway, refuses the commit
// (firmware/tests covers that). Here the whole path: 0, then a gap, then the gap filled.
void test_interface_numbers() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    if (SIM_ENDPOINTS < 3) {
        MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
        return;
    }
    std::vector<mimusb_spec*> specs;
    mimusb_node* nodes[3] = {};
    for (unsigned i : {0U, 2U, 1U, 2U}) {
        specs.push_back(hid_spec_no_id());
        test_hid_node_config c{};
        test_fill_node_config(&c, static_cast<uint8_t>(i));
        mimusb_node* node = nullptr;
        const mimusb_result r = test_node_open_hid(device, specs.back(), &c, &kCommit, &node);
        if (i == 2U && nodes[1] == nullptr) {
            MIMUSB_CHECK(r == MIMUSB_ERR_PARAM && node == nullptr &&
                         mimusb_last_error()->offset == 1U);
        } else {
            MIMUSB_CHECK(r == MIMUSB_OK);
            nodes[i] = node;
        }
    }
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 3U);
    // The target sees interface numbers 0, 1, 2 in its configuration descriptor.
    uint16_t length = 0;
    const uint8_t* c = fake_board_config_descriptor(&length);
    unsigned seen = 0;
    for (uint16_t k = 0; k + 1U < length; k = static_cast<uint16_t>(k + c[k])) {
        if (c[k + 1] == 0x04) {
            MIMUSB_CHECK(c[k + 2] == seen);
            ++seen;
        }
    }
    MIMUSB_CHECK(seen == 3U);
    for (int i = 2; i >= 0; --i)
        MIMUSB_CHECK(mimusb_node_close(nodes[i], &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    for (auto* spec : specs)
        mimusb_spec_release(spec);
}

} // namespace sim
