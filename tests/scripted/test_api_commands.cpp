// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Configuration commands against a board that NAKs them with every code, does not answer, or
 * answers late: how each answer reaches the caller, the retry with the same sequence number, and
 * what is left staged for a later commit. Then the device calls that send one command each. */
#include "mimusb_proto.h"
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
        mimusb_result result;
    };
    const Case cases[] = {
        {MIMUSB_NAK_PARAM, MIMUSB_ERR_PARAM},
        {MIMUSB_NAK_OVERFLOW, MIMUSB_ERR_OVERFLOW},
        {MIMUSB_NAK_STATE, MIMUSB_ERR_BUSY},
        {MIMUSB_NAK_DESCRIPTOR, MIMUSB_ERR_DESCRIPTOR_REJECTED},
        {MIMUSB_NAK_UNSUPPORTED, MIMUSB_ERR_UNSUPPORTED},
        {MIMUSB_NAK_BUSY, MIMUSB_ERR_BUSY},
        {0x7F, MIMUSB_ERR_INTERNAL}, // a code this library does not know
    };
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_spec* spec = keyboard_spec();
    const mimusb_hid_node_config config = node_config();
    for (const Case& c : cases) {
        const std::uint8_t code = c.nak;
        board.set_policy([code](const Received& r) -> std::optional<Reply> {
            if (r.type == MIMUSB_T_DESC_BEGIN)
                return Reply::nak(code);
            return std::nullopt;
        });
        mimusb_node* node = nullptr;
        MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == c.result);
        MIMUSB_CHECK(node == nullptr);
        const mimusb_error_detail* e = mimusb_last_error();
        MIMUSB_CHECK(e->code == c.result && e->transport_status == code &&
                     e->frame_type == MIMUSB_T_DESC_BEGIN && e->node_id == 0);
        MIMUSB_CHECK(std::strcmp(mimusb_frame_type_name(MIMUSB_T_DESC_BEGIN), "DESC_BEGIN") == 0);
    }
    // The refused opens left no slot taken: with the board answering again, the node opens.
    board.set_policy(nullptr);
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_id(node) == 1);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// A command that hears nothing back is sent again, unchanged and with the same sequence number,
// so that the board runs it once; after the last attempt the caller gets a timeout.
void test_retry_and_timeout() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMUSB_T_DESC_RESET)
            return Reply::silent();
        return std::nullopt;
    });
    board.clear();
    mimusb_spec* spec = keyboard_spec();
    const mimusb_hid_node_config config = node_config();
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) ==
                 MIMUSB_ERR_TIMEOUT);
    MIMUSB_CHECK(mimusb_last_error()->frame_type == MIMUSB_T_DESC_RESET);
    std::vector<Received> sent;
    for (const Received& r : board.received()) {
        if (r.type == MIMUSB_T_DESC_RESET)
            sent.push_back(r);
    }
    MIMUSB_CHECK(sent.size() == 3U);
    if (sent.size() == 3U) {
        MIMUSB_CHECK(sent[0].payload == sent[1].payload && sent[1].payload == sent[2].payload);
    }

    // The board answers only the second attempt: that answer counts.
    auto attempts = std::make_shared<std::atomic<int>>(0);
    board.set_policy([attempts](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMUSB_T_DESC_RESET && attempts->fetch_add(1) == 0)
            return Reply::silent();
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(attempts->load() == 2);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// An ACK of another sequence number or another command, or a NAK cut short, answers nothing.
void test_stray_answers_are_ignored() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    Board* b = &board;
    board.set_policy([b](const Received& r) -> std::optional<Reply> {
        if (r.type != MIMUSB_T_LOSS_CONFIG)
            return std::nullopt;
        const std::uint8_t seq = r.payload[0];
        const std::uint8_t wrong_seq[2] = {static_cast<std::uint8_t>(seq + 1), r.type};
        const std::uint8_t wrong_type[2] = {seq, MIMUSB_T_RESET};
        const std::uint8_t short_nak[2] = {seq, r.type};
        const std::uint8_t short_ack[1] = {seq};
        b->send_frame(MIMUSB_T_ACK, wrong_seq, sizeof(wrong_seq));
        b->send_frame(MIMUSB_T_ACK, wrong_type, sizeof(wrong_type));
        b->send_frame(MIMUSB_T_NAK, short_nak, sizeof(short_nak));
        b->send_frame(MIMUSB_T_ACK, short_ack, sizeof(short_ack));
        return Reply::silent();
    });
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, 10, 50) == MIMUSB_ERR_TIMEOUT);
    board.set_policy(nullptr);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, 10, kControlMs) == MIMUSB_OK);
    // Closing turns the loss reporting off again.
    board.clear();
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    Received off;
    MIMUSB_CHECK(board.take(MIMUSB_T_LOSS_CONFIG, &off, 0) && off.payload.size() == 4U &&
                 off.payload[1] == 0U);
}

// Commit, connect and disconnect, and the batch, against a board that accepts or refuses them.
void test_commit_connect_disconnect() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    // With no node there is nothing to present: a commit sends nothing.
    board.clear();
    MIMUSB_CHECK(mimusb_device_connect(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_RESET) == 0U && board.count(MIMUSB_T_COMMIT) == 0U);
    mimusb_spec* keyboard = keyboard_spec();
    const mimusb_hid_node_config first = node_config();
    mimusb_node* attached = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, keyboard, &first, &kCommit, &attached) ==
                 MIMUSB_OK);
    mimusb_spec_release(keyboard);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_is_ready(device) == 1);
    // On the target already: connect sends nothing (a board refuses a COMMIT that attaches while it
    // is attached).
    board.clear();
    MIMUSB_CHECK(mimusb_device_connect(device, &kCommit) == MIMUSB_OK);
    Received r;
    MIMUSB_CHECK(board.count(MIMUSB_T_COMMIT) == 0U && board.count(MIMUSB_T_DESC_RESET) == 0U);
    // Disconnect detaches; the next connect configures the board again.
    MIMUSB_CHECK(mimusb_device_disconnect(device, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.take(MIMUSB_T_COMMIT, &r, 0) && r.payload[1] == 1U);
    MIMUSB_CHECK(mimusb_device_disconnect(device, 0) == MIMUSB_ERR_UNSET_FIELD);
    board.clear();
    MIMUSB_CHECK(mimusb_device_connect(device, &kCommit) == MIMUSB_OK);
    // DESC_RESET, then the device, the configuration and the report descriptor of the interface.
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_RESET) == 1U && board.count(MIMUSB_T_DESC_BEGIN) >= 3U);
    // A refused disconnect leaves the device configured.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_COMMIT)
            return Reply::nak(MIMUSB_NAK_STATE);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_disconnect(device, kControlMs) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_ERR_BUSY);
    board.set_policy(nullptr);
    // A commit with a reconnect delay keeps the device off the bus that long, then attaches.
    board.clear();
    MIMUSB_CHECK(mimusb_device_commit(device, 5, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_COMMIT) == 2U); // detach, then attach
    mimusb_commit_timeouts bad = kCommit;
    bad.struct_size = 4;
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &bad) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_connect(device, &bad) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, nullptr) == MIMUSB_ERR_PARAM);

    // A batch whose commit fails stays open, so the caller can retry.
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_ERR_BUSY);
    mimusb_spec* spec = keyboard_spec();
    const mimusb_hid_node_config config = node_config(1);
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_keyboard_key(node, 0x04, 1) == MIMUSB_ERR_BUSY); // staged only
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_DESC_DATA)
            return Reply::nak(MIMUSB_NAK_DESCRIPTOR);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_ERR_DESCRIPTOR_REJECTED);
    MIMUSB_CHECK(mimusb_last_error()->frame_type == MIMUSB_T_DESC_DATA);
    board.set_policy(nullptr);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_keyboard_key(node, 0x04, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(node, kSendMs) == MIMUSB_OK);
    // An empty batch commits nothing.
    board.clear();
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_COMMIT) == 0U);
    // A commit inside a batch ends it.
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_ERR_BUSY);
    mimusb_spec_release(spec);

    // A board that does not detach on close: the close reports it, and still frees everything.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_COMMIT)
            return Reply::silent();
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_close(device, 20, kDrainMs) == MIMUSB_ERR_TIMEOUT);
}

// The target never mounts: waiting times out, and the device says it is not ready.
void test_wait_ready_timeout() {
    Board board;
    if (!board.start())
        return;
    board.set_mount_on_commit(false);
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_connect(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 30) == MIMUSB_ERR_TIMEOUT);
    MIMUSB_CHECK(mimusb_device_is_ready(device) == 0 && mimusb_device_is_ready(nullptr) == 0);
    MIMUSB_CHECK(mimusb_device_wait_ready(nullptr, 30) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The link counters: read and reset, a broken answer, a refusal, and the arguments.
void test_link_counters() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_link_counters counters;
    mimusb_link_counters_init(&counters);
    MIMUSB_CHECK(mimusb_device_link_counters(device, 1, kControlMs, &counters) == MIMUSB_OK);
    MIMUSB_CHECK(counters.bad_crc == 0 && counters.framing == 0 && counters.ring_overrun == 0);
    Received r;
    MIMUSB_CHECK(board.take(MIMUSB_T_COUNTERS, &r, 0) && r.payload.size() == 2U &&
                 r.payload[1] == MIMUSB_COUNTERS_F_RESET);
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, &counters) == MIMUSB_OK);
    MIMUSB_CHECK(board.take(MIMUSB_T_COUNTERS, &r, 0) && r.payload[1] == 0U);

    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_COUNTERS)
            return Reply::ack({1, 2, 3}); // shorter than five counters
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, &counters) == MIMUSB_ERR_IO);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_COUNTERS)
            return Reply::nak(MIMUSB_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, &counters) ==
                 MIMUSB_ERR_UNSUPPORTED);
    board.set_policy(nullptr);

    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, 0, &counters) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, nullptr) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_link_counters(nullptr, 0, kControlMs, &counters) ==
                 MIMUSB_ERR_PARAM);
    counters.struct_size = 2;
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, &counters) == MIMUSB_ERR_PARAM);
    mimusb_link_counters_init(&counters);
    counters.reserved = 1;
    MIMUSB_CHECK(mimusb_device_link_counters(device, 0, kControlMs, &counters) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// Remote wakeup is one command; the board refuses it while the target has not suspended.
void test_enter_bootloader() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_enter_bootloader(device, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_enter_bootloader(nullptr, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(board.count(MIMUSB_T_BOOTLOADER) == 0U);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_BOOTLOADER)
            return Reply::nak(MIMUSB_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_enter_bootloader(device, kControlMs) == MIMUSB_ERR_UNSUPPORTED);
    board.set_policy(nullptr);
    MIMUSB_CHECK(mimusb_device_enter_bootloader(device, kControlMs) == MIMUSB_OK);
    // The frame carries the key after its sequence number.
    bool keyed = false;
    for (const Received& q : board.received()) {
        if (q.type == MIMUSB_T_BOOTLOADER)
            keyed = q.payload.size() == 5U && q.payload[1] == 'B' && q.payload[2] == 'O' &&
                    q.payload[3] == 'O' && q.payload[4] == 'T';
    }
    MIMUSB_CHECK(keyed);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

void test_remote_wakeup() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_REMOTE_WAKEUP) == 1U);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_REMOTE_WAKEUP)
            return Reply::nak(MIMUSB_NAK_STATE);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, kControlMs) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_remote_wakeup(device, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_remote_wakeup(nullptr, kControlMs) == MIMUSB_ERR_PARAM);
    board.set_policy(nullptr);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// Control rules: the rules reach the board as given with the rules of the Nodes, a board with
// fewer rules refuses more, a refused table leaves the old one, and closing removes the Device's
// rules and keeps the Nodes'.
void test_control_rules() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_control_rules = 1;
    board.set_pong(pong);
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_node* node = open_vendor_node(device);
    mimusb_control_rule rules[2]{};
    for (mimusb_control_rule& rule : rules) {
        rule.request_type_mask = 0x60;
        rule.request_type_value = 0x40;
        rule.action = MIMUSB_CONTROL_ASK;
        rule.timeout_ms = 300;
        rule.when_late = MIMUSB_CONTROL_ACTION_STALL;
    }
    board.clear();
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, rules, 2, kControlMs) ==
                 MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE) == 0U);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, rules, 1, kControlMs) == MIMUSB_OK);
    // RULE: seq, index, the four request bytes, wValue mask and value, wIndex mask and value,
    // action, and for ASK the time (2) and the action when it ran out; then RULES_APPLY: seq,
    // flags, count.
    Received r;
    MIMUSB_CHECK(board.take(MIMUSB_T_RULE, &r, 0) && r.payload.size() == 18U);
    if (r.payload.size() == 18U) {
        MIMUSB_CHECK(r.payload[1] == 0 && r.payload[2] == 0x60 && r.payload[3] == 0x40 &&
                     r.payload[14] == MIMUSB_RULE_ASK && r.payload[15] == (300 & 0xFF) &&
                     r.payload[16] == (300 >> 8) &&
                     r.payload[17] == MIMUSB_CONTROL_ACTION_STALL - 1);
    }
    MIMUSB_CHECK(board.take(MIMUSB_T_RULES_APPLY, &r, 0) && r.payload.size() == 3U &&
                 r.payload[1] == 0 && r.payload[2] == 1);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_RULES_APPLY)
            return Reply::nak(MIMUSB_NAK_PARAM);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 1, nullptr, 0, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    board.set_policy(nullptr);
    board.clear();
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    board.clear();
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The rules need a Node: a Device with none has no USB configuration to hold them, so they wait for
// the first commit, and the ones of a batch go with its commit.
void test_control_rules_wait_for_a_configuration() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_control_rule rule{};
    rule.request_type_mask = 0x60;
    rule.request_type_value = 0x40;
    rule.action = MIMUSB_CONTROL_STALL;
    board.clear();
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, &rule, 1, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE) == 0U);
    mimusb_node* node = open_vendor_node(device);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE) == 1U);
    // In a batch the new rules go with the commit that ends it.
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    board.clear();
    rule.request_value = 6;
    rule.request_mask = 0xFF;
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, &rule, 1, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE) == 0U);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE) == 1U && board.count(MIMUSB_T_COMMIT) >= 1U);
    // Arguments.
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 2, &rule, 1, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, nullptr, 1, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, &rule, 1, 0) == MIMUSB_ERR_UNSET_FIELD);
    mimusb_control_rule unset = rule;
    unset.action = static_cast<mimusb_control_rule_action>(0);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, &unset, 1, kControlMs) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// A new USB identity is checked against the board and committed.
void test_set_usb_identity() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_spec* spec = keyboard_spec();
    const mimusb_hid_node_config config = node_config();
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
    mimusb_spec_release(spec);
    mimusb_device_config o = board_options();
    o.usb_device.product_id = 0x0002;
    o.usb_device.serial = "S1";
    o.usb_config.string = "config";
    board.clear();
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_OK);
    Received r;
    // The first DESC_DATA is the device descriptor: seq, kind, index, language (2), offset (2),
    // then the descriptor, whose idProduct is at bytes 10 and 11 (USB 2.0 9.6.1).
    MIMUSB_CHECK(board.take(MIMUSB_T_DESC_DATA, &r, 0) && r.payload.size() == 7U + 18U &&
                 r.payload[1] == MIMUSB_DESC_DEVICE && r.payload[7 + 10] == 0x02 &&
                 r.payload[7 + 11] == 0x00);
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, nullptr, &o.usb_config, &kCommit) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, nullptr, &kCommit) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, nullptr) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_usb_identity(nullptr, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_ERR_PARAM);
    o.usb_device.bcd_usb = 0;
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_ERR_UNSET_FIELD);
    // Inside a batch the identity is only staged.
    o = board_options();
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    board.clear();
    MIMUSB_CHECK(mimusb_device_set_usb_identity(device, &o.usb_device, &o.usb_config, &kCommit) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_RESET) == 0U);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_RESET) == 1U);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// Every device call refuses a null Device; the close refuses a zero timeout and keeps the Device.
void test_device_arguments() {
    MIMUSB_CHECK(mimusb_device_commit(nullptr, 0, &kCommit) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_connect(nullptr, &kCommit) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_disconnect(nullptr, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_batch_begin(nullptr) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_batch_end(nullptr, &kCommit) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_latched_error(nullptr) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(nullptr, 0, 0, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_control_rules(nullptr, 0, nullptr, 0, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_control_reply(nullptr, 1, MIMUSB_CONTROL_ACTION_LEAVE, nullptr, 0,
                                             kSendMs) == MIMUSB_ERR_PARAM);
    mimusb_board_limits limits;
    mimusb_board_limits_init(&limits);
    MIMUSB_CHECK(mimusb_device_limits(nullptr, &limits) == MIMUSB_ERR_PARAM);

    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_close(device, 0, kDrainMs) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_latched_error(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
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
    test_enter_bootloader();
    test_control_rules();
    test_control_rules_wait_for_a_configuration();
    test_set_usb_identity();
    test_device_arguments();
}
