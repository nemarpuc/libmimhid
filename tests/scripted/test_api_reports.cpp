// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Reports on a line that misbehaves: mimusb_report_send and mimusb_node_submit when the board
 * stops reading (the write stalls) and when the cable is pulled, and what each leaves for a
 * resend. */
#include "mimusb_proto.h"
#include "scripted_support.hpp"
#include "unit/profile_support.hpp"

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace scripted;

namespace {

struct Opened {
    mimusb_device* device{};
    mimusb_node* node{};
    mimusb_spec* spec{};
};

Opened open_keyboard(Board& board) {
    Opened o;
    o.device = open_on(board, board_options());
    if (o.device == nullptr)
        return o;
    o.spec = keyboard_spec();
    const test_hid_node_config config = node_config();
    MIMUSB_CHECK(test_node_open_hid(o.device, o.spec, &config, &kCommit, &o.node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(o.device, 1000) == MIMUSB_OK);
    return o;
}

// mimusb_node_endpoint_write passes the bytes as they are, as frames of the Node's IN endpoint (a
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
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, bytes, sizeof(bytes), kSendMs) ==
                 MIMUSB_OK);
    Received r;
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &r) && r.payload.size() == 3U &&
                 r.payload[0] == 0xAA && r.payload[2] == 0xCC);
    // An empty report is a report too.
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, nullptr, 0, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &r) && r.payload.empty());

    // More than a packet is a transfer of two frames, as for every endpoint; one that would need
    // more than 65535 frames is refused.
    std::array<std::uint8_t, 65> long_report{};
    board.clear();
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, long_report.data(), long_report.size(),
                                            kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &r) && r.payload.size() == 64U);
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &r) && r.payload.size() == 1U);
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, bytes, 1U << 22, kSendMs) ==
                 MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, nullptr, 1, kSendMs) == MIMUSB_ERR_PARAM);
    // Only an IN endpoint of the Node is written to.
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x82, bytes, 1, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_write(nullptr, 0x81, bytes, 1, kSendMs) == MIMUSB_ERR_PARAM);
    // Input calls on a generated node, and the raw call refused on it.
    MIMUSB_CHECK(mimusb_raw_submit(o.node, bytes, 1, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_submit(o.node, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_submit(nullptr, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_submit_blocking(o.node, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_submit_blocking(nullptr, kSendMs) == MIMUSB_ERR_PARAM);
    // Nothing changed: a submit sends nothing and succeeds.
    board.clear();
    MIMUSB_CHECK(mimusb_node_submit(o.node, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit_blocking(o.node, kSendMs) == MIMUSB_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MIMUSB_CHECK(board.count((MIMUSB_T_EP_IN_BASE | 1)) == 0U);
    mimusb_spec_release(o.spec);
    MIMUSB_CHECK(mimusb_device_close(o.device, kControlMs, kDrainMs) == MIMUSB_OK);
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
    mimusb_result r = MIMUSB_OK;
    for (int i = 0; i < 100000 && r == MIMUSB_OK; ++i)
        r = mimusb_node_endpoint_write(o.node, 0x81, bytes.data(), bytes.size(), 5);
    MIMUSB_CHECK(r == MIMUSB_ERR_TIMEOUT || r == MIMUSB_ERR_SHORT_TRANSFER);
    // The line is full: a key press cannot go out, and stays pending.
    MIMUSB_CHECK(mimusb_keyboard_key(o.node, 0x04, 1) == MIMUSB_OK);
    r = mimusb_node_submit(o.node, 5);
    MIMUSB_CHECK(r == MIMUSB_ERR_TIMEOUT || r == MIMUSB_ERR_SHORT_TRANSFER);
    MIMUSB_CHECK(mimusb_last_error()->node_id == mimusb_node_id(o.node));
    const mimusb_result first = r;
    // A control reply cannot go out either.
    r = mimusb_device_control_reply(o.device, 1, MIMUSB_CONTROL_ACTION_LEAVE, nullptr, 0, 5);
    MIMUSB_CHECK(r == MIMUSB_ERR_TIMEOUT || r == MIMUSB_ERR_SHORT_TRANSFER);
    // The blocking submit gives up at its deadline.
    r = mimusb_node_submit_blocking(o.node, 20);
    MIMUSB_CHECK(r == MIMUSB_ERR_TIMEOUT || r == MIMUSB_ERR_SHORT_TRANSFER);
    board.set_reading(true);
    // A timeout kept the press pending, so it goes out now; a short write lost the frame.
    if (first == MIMUSB_ERR_TIMEOUT) {
        MIMUSB_CHECK(mimusb_node_submit_blocking(o.node, 2000) == MIMUSB_OK);
    }
    mimusb_spec_release(o.spec);
    MIMUSB_CHECK(mimusb_device_close(o.device, kControlMs, 2000) == MIMUSB_OK);
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
    while (mimusb_device_latched_error(o.device) != MIMUSB_ERR_NO_DEVICE &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIMUSB_CHECK(mimusb_device_latched_error(o.device) == MIMUSB_ERR_NO_DEVICE);
    const std::uint8_t bytes[1] = {0};
    MIMUSB_CHECK(mimusb_node_endpoint_write(o.node, 0x81, bytes, 1, kSendMs) ==
                 MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_device_control_reply(o.device, 1, MIMUSB_CONTROL_ACTION_LEAVE, nullptr, 0,
                                             kSendMs) == MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_device_wait_ready(o.device, 10) == MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(o.device, 0, 0, kControlMs) ==
                 MIMUSB_ERR_NO_DEVICE);
    mimusb_node* other = nullptr;
    const test_hid_node_config config = node_config(1);
    MIMUSB_CHECK(test_node_open_hid(o.device, o.spec, &config, &kCommit, &other) ==
                 MIMUSB_ERR_NO_DEVICE);
    mimusb_usb_endpoint endpoint{};
    endpoint.address = 0x82;
    endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    endpoint.max_packet_size = 64;
    endpoint.interval = 1;
    endpoint.policy = {MIMUSB_FULL_DROP_NEWEST, 4};
    const mimusb_usb_item item = mimusb_usb_item_make_endpoint(endpoint);
    const mimusb_usb_setting setting = {&item, 1};
    mimusb_usb_interface itf{};
    itf.number = 1;
    itf.class_code = 0xFF;
    itf.settings = &setting;
    itf.setting_count = 1;
    mimusb_node_config generic;
    mimusb_node_config_init(&generic);
    generic.interfaces = &itf;
    generic.interface_count = 1;
    MIMUSB_CHECK(test_node_open_interfaces(o.device, &generic, &kCommit, &other) ==
                 MIMUSB_ERR_NO_DEVICE);
    MIMUSB_CHECK(mimusb_keyboard_key(o.node, 0x04, 1) == MIMUSB_ERR_NO_DEVICE);
    mimusb_spec_release(o.spec);
    MIMUSB_CHECK(mimusb_device_close(o.device, kControlMs, kDrainMs) == MIMUSB_ERR_NO_DEVICE);
}

// A touchscreen with Scan Time: the first report starts the clock at zero, later ones carry the
// time since, and each submit sends one report of the frame.
void test_scan_time() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_touchscreen_options options = mimusb_test::touchscreen_options();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    const test_hid_node_config config = node_config();
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(test_node_open_hid(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    board.clear();
    MIMUSB_CHECK(mimusb_touchscreen_contact(node, 1, 1, 100, 100, nullptr) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit_blocking(node, 1000) == MIMUSB_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    MIMUSB_CHECK(mimusb_touchscreen_contact(node, 1, 1, 200, 100, nullptr) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit_blocking(node, 1000) == MIMUSB_OK);
    Received first;
    Received second;
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &first) &&
                 board.take((MIMUSB_T_EP_IN_BASE | 1), &second));
    MIMUSB_CHECK(first.payload.size() == second.payload.size() && first.payload != second.payload);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace

void test_api_reports() {
    test_scan_time();
    test_endpoint_write_of_a_profile_node();
    test_stalled_line();
    test_link_lost();
}
