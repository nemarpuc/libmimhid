// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Nodes opened with mimusb_node_open as the serial protocol sees them: the endpoint policies at the
 * commit, data frames split on packet boundaries, the wait policy's count against the board's
 * EP_DONE events, EP_STALL and EP_FLUSH, and the endpoint events. */
#include "mimusb_proto.h"
#include "scripted_support.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace scripted;

namespace {

struct Seen {
    mimusb_event_kind kind;
    std::uint16_t node_id;
    std::uint32_t value;
    std::vector<std::uint8_t> data;
    std::uint32_t endpoint_address;
    std::uint32_t interface_number;
};
std::mutex g_mutex;
std::vector<Seen> g_seen;

void MIMUSB_CALL record(void*, const mimusb_event* e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_seen.push_back({e->kind, e->node_id, e->value,
                      std::vector<std::uint8_t>(e->data, e->data + e->length), e->endpoint_address,
                      e->interface_number});
}

bool next(Seen* out, int timeout_ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_seen.empty()) {
                *out = g_seen.front();
                g_seen.erase(g_seen.begin());
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

mimusb_usb_endpoint ep(std::uint8_t address, std::uint8_t type, std::uint8_t on_full,
                       std::uint8_t depth) {
    mimusb_usb_endpoint e{};
    e.address = address;
    e.transfer_type = type;
    e.max_packet_size = 64;
    e.interval = type == MIMUSB_TRANSFER_INTERRUPT ? 1 : 0;
    e.policy = {on_full, depth};
    return e;
}

std::vector<Received> frames_of(Board& board, std::uint8_t type) {
    std::vector<Received> out;
    for (const Received& r : board.received()) {
        if (r.type == type)
            out.push_back(r);
    }
    return out;
}

void wait_for_frames(Board& board, std::uint8_t type, std::size_t count) {
    for (int i = 0; i < 1000 && frames_of(board, type).size() < count; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void test_generic_frames() {
    Board board;
    if (!board.start())
        return;
    mimusb_device_config o = board_options();
    o.transport.event_sink = record;
    mimusb_device* device = open_on(board, o);
    if (device == nullptr)
        return;

    mimusb_usb_endpoint eps[] = {ep(0x81, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_WAIT, 3),
                                 ep(0x82, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_DROP_OLDEST, 2),
                                 ep(0x01, MIMUSB_TRANSFER_BULK, 0, 0)};
    const mimusb_usb_item items[] = {mimusb_usb_item_make_endpoint(eps[0]),
                                     mimusb_usb_item_make_endpoint(eps[1]),
                                     mimusb_usb_item_make_endpoint(eps[2])};
    const mimusb_usb_setting setting = {items, 3};
    mimusb_usb_interface itf{};
    itf.class_code = 0xFF;
    itf.settings = &setting;
    itf.setting_count = 1;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    mimusb_node* node = nullptr;
    board.clear();
    MIMUSB_CHECK(test_node_open_interfaces(device, &config, &kCommit, &node) == MIMUSB_OK &&
                 node != nullptr);
    if (node == nullptr)
        return;

    // The commit sends the policy of every IN endpoint: seq, address, depth, policy.
    const std::vector<Received> policies = frames_of(board, MIMUSB_T_EP_CONFIG);
    MIMUSB_CHECK(policies.size() == 2U && policies[0].payload.size() == 4U &&
                 policies[0].payload[1] == 0x81 && policies[0].payload[2] == 3 &&
                 policies[0].payload[3] == MIMUSB_FULL_WAIT && policies[1].payload[1] == 0x82 &&
                 policies[1].payload[2] == 2 && policies[1].payload[3] == MIMUSB_FULL_DROP_OLDEST);
    // No bytes of an answer go to the board for a Node that has no rules.
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_BEGIN) > 0U && board.count(MIMUSB_T_RULE) == 0U);
    for (const Received& r : frames_of(board, MIMUSB_T_DESC_BEGIN))
        MIMUSB_CHECK(r.payload[1] != MIMUSB_DESC_ANSWER);

    // Data frames: 0x40 | endpoint number, split on packet boundaries, no zero-length packet
    // added; a zero-length write is a frame without payload.
    std::vector<std::uint8_t> data(130, 7);
    board.clear();
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, data.data(), 130, kSendMs) == MIMUSB_OK);
    wait_for_frames(board, 0x42, 3);
    std::vector<Received> sent = frames_of(board, 0x42);
    MIMUSB_CHECK(sent.size() == 3U && sent[0].payload.size() == 64U &&
                 sent[1].payload.size() == 64U && sent[2].payload.size() == 2U);
    board.clear();
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, data.data(), 128, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, nullptr, 0, kSendMs) == MIMUSB_OK);
    wait_for_frames(board, 0x42, 3);
    sent = frames_of(board, 0x42);
    MIMUSB_CHECK(sent.size() == 3U && sent[0].payload.size() == 64U &&
                 sent[1].payload.size() == 64U && sent[2].payload.empty());
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, data.data(), 1U << 22, kSendMs) ==
                 MIMUSB_ERR_OVERFLOW);

    // The wait policy: depth 3. A fourth write times out with nothing sent.
    const auto write = [&](std::uint32_t timeout) {
        return mimusb_node_endpoint_write(node, 0x81, data.data(), 4, timeout);
    };
    board.clear();
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(write(50) == MIMUSB_OK);
    MIMUSB_CHECK(write(50) == MIMUSB_ERR_TIMEOUT);
    wait_for_frames(board, 0x41, 3);
    MIMUSB_CHECK(frames_of(board, 0x41).size() == 3U);
    // EP_DONE frees slots by the count it carries.
    board.send_event(MIMUSB_EV_EP_DONE, {0x81, 2, 0});
    MIMUSB_CHECK(write(500) == MIMUSB_OK && write(500) == MIMUSB_OK &&
                 write(50) == MIMUSB_ERR_TIMEOUT);
    // A count above what is outstanding clamps at zero: three writes fit, not four.
    board.send_event(MIMUSB_EV_EP_DONE, {0x81, 200, 0});
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(write(500) == MIMUSB_OK);
    MIMUSB_CHECK(write(50) == MIMUSB_ERR_TIMEOUT);
    // An unmount resets the count.
    board.send_event(MIMUSB_EV_UNMOUNTED, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(write(500) == MIMUSB_OK);
    // A blocked writer is woken by EP_DONE.
    mimusb_result woken = MIMUSB_ERR_INTERNAL;
    std::thread writer([&] { woken = write(3000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    board.send_event(MIMUSB_EV_EP_DONE, {0x81, 1, 0});
    writer.join();
    MIMUSB_CHECK(woken == MIMUSB_OK);
    // A zero timeout is refused with the wait policy only.
    MIMUSB_CHECK(write(0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, data.data(), 4, 0) == MIMUSB_OK);

    // Flush and stall: seq, address[, stall]. Each resets the count.
    board.clear();
    MIMUSB_CHECK(mimusb_node_endpoint_flush(node, 0x81, kControlMs) == MIMUSB_OK);
    std::vector<Received> f = frames_of(board, MIMUSB_T_EP_FLUSH);
    MIMUSB_CHECK(f.size() == 1U && f[0].payload.size() == 2U && f[0].payload[1] == 0x81);
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(write(500) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x81, kControlMs) == MIMUSB_OK);
    f = frames_of(board, MIMUSB_T_EP_STALL);
    MIMUSB_CHECK(f.size() == 1U && f[0].payload.size() == 3U && f[0].payload[1] == 0x81 &&
                 f[0].payload[2] == 1);
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(write(500) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_clear_stall(node, 0x81, kControlMs) == MIMUSB_OK);
    f = frames_of(board, MIMUSB_T_EP_STALL);
    MIMUSB_CHECK(f.size() == 2U && f[1].payload[2] == 0);
    MIMUSB_CHECK(write(500) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_flush(node, 0x01, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x01, kControlMs) == MIMUSB_OK);
    // The board refuses: the answer is the result and the count is unchanged.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_EP_STALL)
            return Reply::nak(MIMUSB_NAK_STATE);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x82, kControlMs) == MIMUSB_ERR_BUSY);
    board.set_policy(nullptr);

    // A new policy goes to the board and the old one stays when it refuses.
    board.clear();
    mimusb_endpoint_policy p{MIMUSB_FULL_DROP_NEWEST, 5};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x82, &p, kControlMs) == MIMUSB_OK);
    f = frames_of(board, MIMUSB_T_EP_CONFIG);
    MIMUSB_CHECK(f.size() == 1U && f[0].payload.size() == 4U && f[0].payload[1] == 0x82 &&
                 f[0].payload[2] == 5 && f[0].payload[3] == MIMUSB_FULL_DROP_NEWEST);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_EP_CONFIG)
            return Reply::nak(MIMUSB_NAK_OVERFLOW);
        return std::nullopt;
    });
    p = {MIMUSB_FULL_WAIT, 200};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x82, &p, kControlMs) ==
                 MIMUSB_ERR_OVERFLOW);
    board.set_policy(nullptr);
    // ...so a write to it still follows the drop policy: no wait, no timeout needed.
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x82, data.data(), 4, kSendMs) == MIMUSB_OK);

    // Events: a packet on an OUT endpoint, the Halt feature, lost transfers.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_seen.clear();
    }
    const std::uint8_t packet[] = {1, 2, 3};
    board.send_frame(MIMUSB_T_EP_OUT_BASE | 1U, packet, sizeof(packet));
    board.send_frame(MIMUSB_T_EP_OUT_BASE | 1U, packet, 0);
    board.send_event(MIMUSB_EV_EP_HALT, {0x81, 1});
    board.send_event(MIMUSB_EV_EP_HALT, {0x01, 0});
    board.send_event(MIMUSB_EV_TRANSFER_LOST, {0x82, 0x34, 0x12});
    Seen s;
    MIMUSB_CHECK(next(&s) && s.kind == MIMUSB_EVENT_ENDPOINT_DATA && s.endpoint_address == 0x01 &&
                 s.interface_number == 0 && s.node_id == mimusb_node_id(node) &&
                 s.data == std::vector<std::uint8_t>(packet, packet + 3));
    MIMUSB_CHECK(next(&s) && s.kind == MIMUSB_EVENT_ENDPOINT_DATA && s.data.empty());
    MIMUSB_CHECK(next(&s) && s.kind == MIMUSB_EVENT_ENDPOINT_HALT && s.value == 1 &&
                 s.endpoint_address == 0x81 && s.node_id == mimusb_node_id(node));
    MIMUSB_CHECK(next(&s) && s.kind == MIMUSB_EVENT_ENDPOINT_HALT && s.value == 0 &&
                 s.endpoint_address == 0x01);
    MIMUSB_CHECK(next(&s) && s.kind == MIMUSB_EVENT_TRANSFER_LOST && s.value == 0x1234 &&
                 s.endpoint_address == 0x82 && s.node_id == mimusb_node_id(node));

    // A profile Node has the same endpoint calls: its IN endpoint is one of the generic driver's.
    mimusb_spec* spec = keyboard_spec();
    test_hid_node_config hid = node_config(1);
    hid.ep_in.address = 0x85; // 0x82 is the vendor Node's
    mimusb_node* keys = nullptr;
    MIMUSB_CHECK(test_node_open_hid(device, spec, &hid, &kCommit, &keys) == MIMUSB_OK);
    if (keys != nullptr) {
        // A write goes out as a data frame of the endpoint; another Node's endpoint is refused.
        board.clear();
        MIMUSB_CHECK(mimusb_node_endpoint_write(keys, 0x85, data.data(), 3, kSendMs) == MIMUSB_OK);
        wait_for_frames(board, 0x45, 1);
        MIMUSB_CHECK(frames_of(board, 0x45).size() == 1U &&
                     frames_of(board, 0x45)[0].payload.size() == 3U);
        MIMUSB_CHECK(mimusb_node_endpoint_write(keys, 0x82, data.data(), 1, kSendMs) ==
                     MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_node_endpoint_stall(keys, 0x85, kControlMs) == MIMUSB_OK);
        MIMUSB_CHECK(board.count(MIMUSB_T_EP_STALL) == 1U);
        MIMUSB_CHECK(mimusb_node_endpoint_clear_stall(keys, 0x85, kControlMs) == MIMUSB_OK);
        MIMUSB_CHECK(mimusb_node_endpoint_flush(keys, 0x85, kControlMs) == MIMUSB_OK);
        MIMUSB_CHECK(board.count(MIMUSB_T_EP_FLUSH) == 1U);
        // The policy: any of the three, sent with the least depth of the interface's Nodes.
        board.clear();
        p = {MIMUSB_FULL_WAIT, 2};
        MIMUSB_CHECK(mimusb_node_endpoint_set_policy(keys, hid.ep_in.address, &p, kControlMs) ==
                     MIMUSB_OK);
        f = frames_of(board, MIMUSB_T_EP_CONFIG);
        MIMUSB_CHECK(f.size() == 1U && f[0].payload[1] == hid.ep_in.address &&
                     f[0].payload[2] == 2 && f[0].payload[3] == MIMUSB_FULL_WAIT);
        board.clear();
        p = {MIMUSB_FULL_DROP_NEWEST, 2};
        MIMUSB_CHECK(mimusb_node_endpoint_set_policy(keys, hid.ep_in.address, &p, kControlMs) ==
                     MIMUSB_OK);
        f = frames_of(board, MIMUSB_T_EP_CONFIG);
        MIMUSB_CHECK(f.size() == 1U && f[0].payload[1] == hid.ep_in.address &&
                     f[0].payload[2] == 2 && f[0].payload[3] == MIMUSB_FULL_DROP_NEWEST);
        MIMUSB_CHECK(mimusb_node_close(keys, &kCommit, kDrainMs) == MIMUSB_OK);
    }
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace

void test_api_generic() { test_generic_frames(); }
