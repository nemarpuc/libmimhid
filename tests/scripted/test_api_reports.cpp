// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Reports on a line that misbehaves: mimhid_report_send and mimhid_node_submit when the board
 * stops reading (the write stalls) and when the cable is pulled, and what each leaves for a
 * resend. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"
#include "unit/profile_support.hpp"

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace scripted;

namespace {

struct Opened {
    mimhid_device* device{};
    mimhid_node* node{};
    mimhid_spec* spec{};
};

Opened open_keyboard(Board& board) {
    Opened o;
    o.device = open_on(board, board_options());
    if (o.device == nullptr)
        return o;
    o.spec = keyboard_spec();
    const mimhid_hid_node_config config = node_config();
    MIMHID_CHECK(mimhid_node_open_profile(o.device, o.spec, &config, &kCommit, &o.node) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid_device_wait_ready(o.device, 1000) == MIMHID_OK);
    return o;
}

// mimhid_node_endpoint_write passes the bytes as they are, as frames of the Node's IN endpoint (a
// profile Node writes there too: its reports go the same way).
void test_endpoint_write_of_a_profile_node() {
    Board board;
    if (!board.start())
        return;
    Opened o = open_keyboard(board);
    if (o.node == nullptr)
        return;
    board.clear();
    const std::uint8_t bytes[3] = {0xAA, 0xBB, 0xCC};
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, bytes, sizeof(bytes), kSendMs) ==
                 MIMHID_OK);
    Received r;
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &r) && r.payload.size() == 3U &&
                 r.payload[0] == 0xAA && r.payload[2] == 0xCC);
    // An empty report is a report too.
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, nullptr, 0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &r) && r.payload.empty());

    // More than a packet is a transfer of two frames, as for every endpoint; one that would need
    // more than 65535 frames is refused.
    std::array<std::uint8_t, 65> long_report{};
    board.clear();
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, long_report.data(), long_report.size(),
                                            kSendMs) == MIMHID_OK);
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &r) && r.payload.size() == 64U);
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &r) && r.payload.size() == 1U);
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, bytes, 1U << 22, kSendMs) ==
                 MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, nullptr, 1, kSendMs) == MIMHID_ERR_PARAM);
    // Only an IN endpoint of the Node is written to.
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x82, bytes, 1, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_write(nullptr, 0x81, bytes, 1, kSendMs) == MIMHID_ERR_PARAM);
    // Input calls on a generated node, and the raw call refused on it.
    MIMHID_CHECK(mimhid_raw_submit(o.node, bytes, 1, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_submit(o.node, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_submit(nullptr, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_submit_blocking(o.node, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_submit_blocking(nullptr, kSendMs) == MIMHID_ERR_PARAM);
    // Nothing changed: a submit sends nothing and succeeds.
    board.clear();
    MIMHID_CHECK(mimhid_node_submit(o.node, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(o.node, kSendMs) == MIMHID_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MIMHID_CHECK(board.count((MIMHID_T_EP_IN_BASE | 1)) == 0U);
    mimhid_spec_release(o.spec);
    MIMHID_CHECK(mimhid_device_close(o.device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The board stops reading: writes fill the pseudo-terminal and then stall. A write that sent
// nothing is a timeout and keeps the state for a resend; once the board reads again the resend
// goes out.
void test_stalled_line() {
    Board board;
    if (!board.start())
        return;
    Opened o = open_keyboard(board);
    if (o.node == nullptr)
        return;
    board.set_reading(false);
    std::array<std::uint8_t, 64> bytes{};
    mimhid_result r = MIMHID_OK;
    for (int i = 0; i < 100000 && r == MIMHID_OK; ++i)
        r = mimhid_node_endpoint_write(o.node, 0x81, bytes.data(), bytes.size(), 5);
    MIMHID_CHECK(r == MIMHID_ERR_TIMEOUT || r == MIMHID_ERR_SHORT_TRANSFER);
    // The line is full: a key press cannot go out, and stays pending.
    MIMHID_CHECK(mimhid_keyboard_key(o.node, 0x04, 1) == MIMHID_OK);
    r = mimhid_node_submit(o.node, 5);
    MIMHID_CHECK(r == MIMHID_ERR_TIMEOUT || r == MIMHID_ERR_SHORT_TRANSFER);
    MIMHID_CHECK(mimhid_last_error()->node_id == mimhid_node_id(o.node));
    const mimhid_result first = r;
    // A control reply cannot go out either.
    r = mimhid_device_control_reply(o.device, 1, MIMHID_CONTROL_ACTION_LEAVE, nullptr, 0, 5);
    MIMHID_CHECK(r == MIMHID_ERR_TIMEOUT || r == MIMHID_ERR_SHORT_TRANSFER);
    // The blocking submit gives up at its deadline.
    r = mimhid_node_submit_blocking(o.node, 20);
    MIMHID_CHECK(r == MIMHID_ERR_TIMEOUT || r == MIMHID_ERR_SHORT_TRANSFER);
    board.set_reading(true);
    // A timeout kept the press pending, so it goes out now; a short write lost the frame.
    if (first == MIMHID_ERR_TIMEOUT) {
        MIMHID_CHECK(mimhid_node_submit_blocking(o.node, 2000) == MIMHID_OK);
    }
    mimhid_spec_release(o.spec);
    MIMHID_CHECK(mimhid_device_close(o.device, kControlMs, 2000) == MIMHID_OK);
}

// The cable is pulled: every call that needs the link says so, and the close still frees all.
void test_link_lost() {
    Board board;
    if (!board.start())
        return;
    Opened o = open_keyboard(board);
    if (o.node == nullptr)
        return;
    board.hang_up();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (mimhid_device_latched_error(o.device) != MIMHID_ERR_NO_DEVICE &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIMHID_CHECK(mimhid_device_latched_error(o.device) == MIMHID_ERR_NO_DEVICE);
    const std::uint8_t bytes[1] = {0};
    MIMHID_CHECK(mimhid_node_endpoint_write(o.node, 0x81, bytes, 1, kSendMs) ==
                 MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_control_reply(o.device, 1, MIMHID_CONTROL_ACTION_LEAVE, nullptr, 0,
                                             kSendMs) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_wait_ready(o.device, 10) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(o.device, 0, 0, kControlMs) ==
                 MIMHID_ERR_NO_DEVICE);
    mimhid_node* other = nullptr;
    const mimhid_hid_node_config config = node_config(1);
    MIMHID_CHECK(mimhid_node_open_profile(o.device, o.spec, &config, &kCommit, &other) ==
                 MIMHID_ERR_NO_DEVICE);
    mimhid_usb_endpoint endpoint{};
    endpoint.address = 0x82;
    endpoint.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    endpoint.max_packet_size = 64;
    endpoint.interval = 1;
    endpoint.policy = {MIMHID_FULL_DROP_NEWEST, 4};
    mimhid_usb_interface itf{};
    itf.number = 1;
    itf.class_code = 0xFF;
    itf.endpoints = &endpoint;
    itf.endpoint_count = 1;
    mimhid_node_config generic;
    mimhid_node_config_init(&generic);
    generic.interfaces = &itf;
    generic.interface_count = 1;
    MIMHID_CHECK(mimhid_node_open(o.device, &generic, &kCommit, &other) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_keyboard_key(o.node, 0x04, 1) == MIMHID_ERR_NO_DEVICE);
    mimhid_spec_release(o.spec);
    MIMHID_CHECK(mimhid_device_close(o.device, kControlMs, kDrainMs) == MIMHID_ERR_NO_DEVICE);
}

// A touchscreen with Scan Time: the first report starts the clock at zero, later ones carry the
// time since, and each submit sends one report of the frame.
void test_scan_time() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_touchscreen_options options = mimhid_test::touchscreen_options();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    const mimhid_hid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open_profile(device, spec, &config, &kCommit, &node) == MIMHID_OK);
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    board.clear();
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 1, 1, 100, 100, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 1, 1, 200, 100, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    Received first;
    Received second;
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &first) &&
                 board.take((MIMHID_T_EP_IN_BASE | 1), &second));
    MIMHID_CHECK(first.payload.size() == second.payload.size() && first.payload != second.payload);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_reports() {
    test_scan_time();
    test_endpoint_write_of_a_profile_node();
    test_stalled_line();
    test_link_lost();
}
