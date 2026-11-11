// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Configuration commands against a board that NAKs them with every code, does not answer, or
 * answers late: how each answer reaches the caller, the retry with the same sequence number, and
 * what is left staged for a later commit. Then the device calls that send one command each. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

using namespace scripted;

namespace {

// Each NAK code of the board is one result of the C ABI, with the code and the frame in the
// error detail.
void test_nak_codes() {
    struct Case {
        std::uint8_t nak;
        mimhid_result result;
    };
    const Case cases[] = {
        {MIMHID_NAK_PARAM, MIMHID_ERR_PARAM},
        {MIMHID_NAK_OVERFLOW, MIMHID_ERR_OVERFLOW},
        {MIMHID_NAK_STATE, MIMHID_ERR_BUSY},
        {MIMHID_NAK_DESCRIPTOR, MIMHID_ERR_DESCRIPTOR_REJECTED},
        {MIMHID_NAK_UNSUPPORTED, MIMHID_ERR_UNSUPPORTED},
        {MIMHID_NAK_BUSY, MIMHID_ERR_BUSY},
        {0x7F, MIMHID_ERR_INTERNAL}, // a code this library does not know
    };
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_spec* spec = keyboard_spec();
    const mimhid_node_config config = node_config();
    for (const Case& c : cases) {
        const std::uint8_t code = c.nak;
        board.set_policy([code](const Received& r) -> std::optional<Reply> {
            if (r.type == MIMHID_T_DESC_BEGIN)
                return Reply::nak(code);
            return std::nullopt;
        });
        mimhid_node* node = nullptr;
        MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == c.result);
        MIMHID_CHECK(node == nullptr);
        const mimhid_error_detail* e = mimhid_last_error();
        MIMHID_CHECK(e->code == c.result && e->transport_status == code &&
                     e->frame_type == MIMHID_T_DESC_BEGIN && e->node_id == 0);
        MIMHID_CHECK(std::strcmp(mimhid_frame_type_name(MIMHID_T_DESC_BEGIN), "DESC_BEGIN") == 0);
    }
    // The refused opens left no slot taken: with the board answering again, the node opens.
    board.set_policy(nullptr);
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_id(node) == 1);
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// A command that hears nothing back is sent again, unchanged and with the same sequence number,
// so that the board runs it once; after the last attempt the caller gets a timeout.
void test_retry_and_timeout() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_DESC_RESET)
            return Reply::silent();
        return std::nullopt;
    });
    board.clear();
    mimhid_spec* spec = keyboard_spec();
    const mimhid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == MIMHID_ERR_TIMEOUT);
    MIMHID_CHECK(mimhid_last_error()->frame_type == MIMHID_T_DESC_RESET);
    std::vector<Received> sent;
    for (const Received& r : board.received()) {
        if (r.type == MIMHID_T_DESC_RESET)
            sent.push_back(r);
    }
    MIMHID_CHECK(sent.size() == 3U);
    if (sent.size() == 3U) {
        MIMHID_CHECK(sent[0].payload == sent[1].payload && sent[1].payload == sent[2].payload);
    }

    // The board answers only the second attempt: that answer counts.
    auto attempts = std::make_shared<std::atomic<int>>(0);
    board.set_policy([attempts](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_DESC_RESET && attempts->fetch_add(1) == 0)
            return Reply::silent();
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == MIMHID_OK);
    MIMHID_CHECK(attempts->load() == 2);
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// An ACK of another sequence number or another command, or a NAK cut short, answers nothing.
void test_stray_answers_are_ignored() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    Board* b = &board;
    board.set_policy([b](const Received& r) -> std::optional<Reply> {
        if (r.type != MIMHID_T_LOSS_CONFIG)
            return std::nullopt;
        const std::uint8_t seq = r.payload[0];
        const std::uint8_t wrong_seq[2] = {static_cast<std::uint8_t>(seq + 1), r.type};
        const std::uint8_t wrong_type[2] = {seq, MIMHID_T_RESET};
        const std::uint8_t short_nak[2] = {seq, r.type};
        const std::uint8_t short_ack[1] = {seq};
        b->send_frame(MIMHID_T_ACK, wrong_seq, sizeof(wrong_seq));
        b->send_frame(MIMHID_T_ACK, wrong_type, sizeof(wrong_type));
        b->send_frame(MIMHID_T_NAK, short_nak, sizeof(short_nak));
        b->send_frame(MIMHID_T_ACK, short_ack, sizeof(short_ack));
        return Reply::silent();
    });
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, 10, 50) == MIMHID_ERR_TIMEOUT);
    board.set_policy(nullptr);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, 10, kControlMs) == MIMHID_OK);
    // Closing turns the loss reporting off again.
    board.clear();
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    Received off;
    MIMHID_CHECK(board.take(MIMHID_T_LOSS_CONFIG, &off, 0) && off.payload.size() == 4U &&
                 off.payload[1] == 0U);
}

// Commit, connect and disconnect, and the batch, against a board that accepts or refuses them.
void test_commit_connect_disconnect() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    // With no node there is nothing to present: a commit sends nothing.
    board.clear();
    MIMHID_CHECK(mimhid_device_connect(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_RESET) == 0U && board.count(MIMHID_T_COMMIT) == 0U);
    mimhid_spec* keyboard = keyboard_spec();
    const mimhid_node_config first = node_config();
    mimhid_node* attached = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, keyboard, &first, &kCommit, &attached) == MIMHID_OK);
    mimhid_spec_release(keyboard);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 1);
    // Configured: connect sends a COMMIT that attaches, nothing else.
    board.clear();
    MIMHID_CHECK(mimhid_device_connect(device, &kCommit) == MIMHID_OK);
    Received r;
    MIMHID_CHECK(board.take(MIMHID_T_COMMIT, &r, 0) && r.payload.size() == 4U &&
                 r.payload[1] == 0U);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_RESET) == 0U);
    // Disconnect detaches; the next connect configures the board again.
    MIMHID_CHECK(mimhid_device_disconnect(device, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(board.take(MIMHID_T_COMMIT, &r, 0) && r.payload[1] == 1U);
    MIMHID_CHECK(mimhid_device_disconnect(device, 0) == MIMHID_ERR_UNSET_FIELD);
    board.clear();
    MIMHID_CHECK(mimhid_device_connect(device, &kCommit) == MIMHID_OK);
    // DESC_RESET, then the device, the configuration and the report descriptor of the interface.
    MIMHID_CHECK(board.count(MIMHID_T_DESC_RESET) == 1U && board.count(MIMHID_T_DESC_BEGIN) >= 3U);
    // A refused disconnect leaves the device configured.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_COMMIT)
            return Reply::nak(MIMHID_NAK_STATE);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_disconnect(device, kControlMs) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &kCommit) == MIMHID_ERR_BUSY);
    board.set_policy(nullptr);
    // A commit with a reconnect delay keeps the device off the bus that long, then attaches.
    board.clear();
    MIMHID_CHECK(mimhid_device_commit(device, 5, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_COMMIT) == 2U); // detach, then attach
    mimhid_commit_timeouts bad = kCommit;
    bad.struct_size = 4;
    MIMHID_CHECK(mimhid_device_commit(device, 0, &bad) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_connect(device, &bad) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_commit(device, 0, nullptr) == MIMHID_ERR_PARAM);

    // A batch whose commit fails stays open, so the caller can retry.
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_ERR_BUSY);
    mimhid_spec* spec = keyboard_spec();
    const mimhid_node_config config = node_config(1);
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(node, 0x04, 1) == MIMHID_ERR_BUSY); // staged only
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_DESC_DATA)
            return Reply::nak(MIMHID_NAK_DESCRIPTOR);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_ERR_DESCRIPTOR_REJECTED);
    MIMHID_CHECK(mimhid_last_error()->frame_type == MIMHID_T_DESC_DATA);
    board.set_policy(nullptr);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(node, 0x04, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(node, kSendMs) == MIMHID_OK);
    // An empty batch commits nothing.
    board.clear();
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_COMMIT) == 0U);
    // A commit inside a batch ends it.
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_ERR_BUSY);
    mimhid_spec_release(spec);

    // A board that does not detach on close: the close reports it, and still frees everything.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_COMMIT)
            return Reply::silent();
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_close(device, 20, kDrainMs) == MIMHID_ERR_TIMEOUT);
}

// The target never mounts: waiting times out, and the device says it is not ready.
void test_wait_ready_timeout() {
    Board board;
    if (!board.start())
        return;
    board.set_mount_on_commit(false);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_connect(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 30) == MIMHID_ERR_TIMEOUT);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 0 && mimhid_device_is_ready(nullptr) == 0);
    MIMHID_CHECK(mimhid_device_wait_ready(nullptr, 30) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The link counters: read and reset, a broken answer, a refusal, and the arguments.
void test_link_counters() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_link_counters counters;
    mimhid_link_counters_init(&counters);
    MIMHID_CHECK(mimhid_device_link_counters(device, 1, kControlMs, &counters) == MIMHID_OK);
    MIMHID_CHECK(counters.bad_crc == 0 && counters.framing == 0 && counters.ring_overrun == 0);
    Received r;
    MIMHID_CHECK(board.take(MIMHID_T_COUNTERS, &r, 0) && r.payload.size() == 2U &&
                 r.payload[1] == MIMHID_COUNTERS_F_RESET);
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, &counters) == MIMHID_OK);
    MIMHID_CHECK(board.take(MIMHID_T_COUNTERS, &r, 0) && r.payload[1] == 0U);

    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_COUNTERS)
            return Reply::ack({1, 2, 3}); // shorter than five counters
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, &counters) == MIMHID_ERR_IO);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_COUNTERS)
            return Reply::nak(MIMHID_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, &counters) ==
                 MIMHID_ERR_UNSUPPORTED);
    board.set_policy(nullptr);

    MIMHID_CHECK(mimhid_device_link_counters(device, 0, 0, &counters) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_link_counters(nullptr, 0, kControlMs, &counters) ==
                 MIMHID_ERR_PARAM);
    counters.struct_size = 2;
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, &counters) == MIMHID_ERR_PARAM);
    mimhid_link_counters_init(&counters);
    counters.reserved = 1;
    MIMHID_CHECK(mimhid_device_link_counters(device, 0, kControlMs, &counters) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// Remote wakeup is one command; the board refuses it while the target has not suspended.
void test_remote_wakeup() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_REMOTE_WAKEUP) == 1U);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_REMOTE_WAKEUP)
            return Reply::nak(MIMHID_NAK_STATE);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, kControlMs) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_remote_wakeup(device, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_remote_wakeup(nullptr, kControlMs) == MIMHID_ERR_PARAM);
    board.set_policy(nullptr);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// Control routing: the rules reach the board as given, a board with fewer rules refuses more,
// and closing removes the routing.
void test_control_routing() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_control_rules = 1;
    board.set_pong(pong);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_control_rule rules[2]{};
    for (mimhid_control_rule& rule : rules) {
        rule.request_type_mask = 0x60;
        rule.request_type_value = 0x40;
        rule.timeout_ms = 300;
        rule.when_late = MIMHID_CONTROL_ACTION_STALL;
    }
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, rules, 2, kControlMs) ==
                 MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, rules, 1, kControlMs) == MIMHID_OK);
    Received r;
    MIMHID_CHECK(board.take(MIMHID_T_CONTROL_CONFIG, &r, 0) && r.payload.size() == 1U + 2U + 15U);
    if (r.payload.size() == 18U) {
        MIMHID_CHECK(r.payload[1] == 0 && r.payload[2] == 1 && r.payload[3] == 0x60 &&
                     r.payload[4] == 0x40 && r.payload[15] == (300 & 0xFF) &&
                     r.payload[16] == (300 >> 8) &&
                     r.payload[17] == MIMHID_CONTROL_ACTION_STALL - 1);
    }
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_CONTROL_CONFIG)
            return Reply::nak(MIMHID_NAK_PARAM);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 1, nullptr, 0, kControlMs) ==
                 MIMHID_ERR_PARAM);
    board.set_policy(nullptr);
    board.clear();
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(board.take(MIMHID_T_CONTROL_CONFIG, &r, 0) && r.payload.size() == 3U &&
                 r.payload[1] == 0 && r.payload[2] == 0);
}

// A new USB identity is checked against the board and committed.
void test_set_usb_identity() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_spec* spec = keyboard_spec();
    const mimhid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, spec, &config, &kCommit, &node) == MIMHID_OK);
    mimhid_spec_release(spec);
    mimhid_device_config o = board_options();
    o.usb_device.product_id = 0x0002;
    o.usb_device.serial = "S1";
    o.usb_config.string = "config";
    board.clear();
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_OK);
    Received r;
    // The first DESC_DATA is the device descriptor: seq, kind, index, language (2), offset (2),
    // then the descriptor, whose idProduct is at bytes 10 and 11 (USB 2.0 9.6.1).
    MIMHID_CHECK(board.take(MIMHID_T_DESC_DATA, &r, 0) && r.payload.size() == 7U + 18U &&
                 r.payload[1] == MIMHID_DESC_DEVICE && r.payload[7 + 10] == 0x02 &&
                 r.payload[7 + 11] == 0x00);
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, nullptr, &o.usb_config, &kCommit) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, nullptr, &kCommit) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, nullptr) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_usb_identity(nullptr, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_ERR_PARAM);
    o.usb_device.bcd_usb = 0;
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_ERR_UNSET_FIELD);
    // Inside a batch the identity is only staged.
    o = board_options();
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    board.clear();
    MIMHID_CHECK(mimhid_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_RESET) == 0U);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_RESET) == 1U);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// Every device call refuses a null Device; the close refuses a zero timeout and keeps the Device.
void test_device_arguments() {
    MIMHID_CHECK(mimhid_device_commit(nullptr, 0, &kCommit) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_connect(nullptr, &kCommit) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_disconnect(nullptr, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_batch_begin(nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_batch_end(nullptr, &kCommit) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_latched_error(nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_loss_reporting(nullptr, 0, 0, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_routing(nullptr, 0, nullptr, 0, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(nullptr, 1, MIMHID_CONTROL_ACTION_LEAVE, nullptr, 0,
                                             kSendMs) == MIMHID_ERR_PARAM);
    mimhid_board_limits limits;
    mimhid_board_limits_init(&limits);
    MIMHID_CHECK(mimhid_device_limits(nullptr, &limits) == MIMHID_ERR_PARAM);

    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_close(device, 0, kDrainMs) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_device_latched_error(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_commands() {
    test_nak_codes();
    test_retry_and_timeout();
    test_stray_answers_are_ignored();
    test_commit_connect_disconnect();
    test_wait_ready_timeout();
    test_link_counters();
    test_remote_wakeup();
    test_control_routing();
    test_set_usb_identity();
    test_device_arguments();
}
