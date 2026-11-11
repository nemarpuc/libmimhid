// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Nodes opened with mimhid_node_open as the serial protocol sees them: the endpoint policies at the
 * commit, data frames split on packet boundaries, the wait policy's count against the board's
 * EP_DONE events, EP_STALL and EP_FLUSH, and the endpoint events. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace scripted;

namespace {

struct Seen {
    mimhid_event_kind kind;
    std::uint16_t node_id;
    std::uint32_t value;
    std::vector<std::uint8_t> data;
    std::uint32_t endpoint_address;
    std::uint32_t interface_number;
};
std::mutex g_mutex;
std::vector<Seen> g_seen;

void MIMHID_CALL record(void*, const mimhid_event* e) {
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

mimhid_usb_endpoint ep(std::uint8_t address, std::uint8_t type, std::uint8_t on_full,
                       std::uint8_t depth) {
    mimhid_usb_endpoint e{};
    e.address = address;
    e.transfer_type = type;
    e.max_packet_size = 64;
    e.interval = type == MIMHID_TRANSFER_INTERRUPT ? 1 : 0;
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
    mimhid_device_config o = board_options();
    o.transport.event_sink = record;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;

    mimhid_usb_endpoint eps[] = {ep(0x81, MIMHID_TRANSFER_BULK, MIMHID_FULL_WAIT, 3),
                                 ep(0x82, MIMHID_TRANSFER_BULK, MIMHID_FULL_DROP_OLDEST, 2),
                                 ep(0x01, MIMHID_TRANSFER_BULK, 0, 0)};
    mimhid_usb_interface itf{};
    itf.class_code = 0xFF;
    itf.endpoints = eps;
    itf.endpoint_count = 3;
    mimhid_node_config config;
    mimhid_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    mimhid_node* node = nullptr;
    board.clear();
    MIMHID_CHECK(mimhid_node_open(device, &config, &kCommit, &node) == MIMHID_OK && node != nullptr);
    if (node == nullptr)
        return;

    // The commit sends the policy of every IN endpoint: seq, address, depth, policy.
    const std::vector<Received> policies = frames_of(board, MIMHID_T_EP_CONFIG);
    MIMHID_CHECK(policies.size() == 2U && policies[0].payload.size() == 4U &&
                 policies[0].payload[1] == 0x81 && policies[0].payload[2] == 3 &&
                 policies[0].payload[3] == MIMHID_FULL_WAIT && policies[1].payload[1] == 0x82 &&
                 policies[1].payload[2] == 2 && policies[1].payload[3] == MIMHID_FULL_DROP_OLDEST);
    // No report descriptor goes to the board for a Node that is not HID.
    MIMHID_CHECK(board.count(MIMHID_T_DESC_BEGIN) > 0U);
    for (const Received& r : frames_of(board, MIMHID_T_DESC_BEGIN))
        MIMHID_CHECK(r.payload[1] != MIMHID_DESC_HID_REPORT);

    // Data frames: 0x40 | endpoint number, split on packet boundaries, no zero-length packet
    // added; a zero-length write is a frame without payload.
    std::vector<std::uint8_t> data(130, 7);
    board.clear();
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, data.data(), 130, kSendMs) == MIMHID_OK);
    wait_for_frames(board, 0x42, 3);
    std::vector<Received> sent = frames_of(board, 0x42);
    MIMHID_CHECK(sent.size() == 3U && sent[0].payload.size() == 64U &&
                 sent[1].payload.size() == 64U && sent[2].payload.size() == 2U);
    board.clear();
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, data.data(), 128, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, nullptr, 0, kSendMs) == MIMHID_OK);
    wait_for_frames(board, 0x42, 3);
    sent = frames_of(board, 0x42);
    MIMHID_CHECK(sent.size() == 3U && sent[0].payload.size() == 64U &&
                 sent[1].payload.size() == 64U && sent[2].payload.empty());
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, data.data(), 1U << 22, kSendMs) ==
                 MIMHID_ERR_OVERFLOW);

    // The wait policy: depth 3. A fourth write times out with nothing sent.
    const auto write = [&](std::uint32_t timeout) {
        return mimhid_node_endpoint_write(node, 0x81, data.data(), 4, timeout);
    };
    board.clear();
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(write(50) == MIMHID_OK);
    MIMHID_CHECK(write(50) == MIMHID_ERR_TIMEOUT);
    wait_for_frames(board, 0x41, 3);
    MIMHID_CHECK(frames_of(board, 0x41).size() == 3U);
    // EP_DONE frees slots by the count it carries.
    board.send_event(MIMHID_EV_EP_DONE, {0x81, 2, 0});
    MIMHID_CHECK(write(500) == MIMHID_OK && write(500) == MIMHID_OK && write(50) == MIMHID_ERR_TIMEOUT);
    // A count above what is outstanding clamps at zero: three writes fit, not four.
    board.send_event(MIMHID_EV_EP_DONE, {0x81, 200, 0});
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(write(500) == MIMHID_OK);
    MIMHID_CHECK(write(50) == MIMHID_ERR_TIMEOUT);
    // An unmount resets the count.
    board.send_event(MIMHID_EV_UNMOUNTED, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(write(500) == MIMHID_OK);
    // A blocked writer is woken by EP_DONE.
    mimhid_result woken = MIMHID_ERR_INTERNAL;
    std::thread writer([&] { woken = write(3000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    board.send_event(MIMHID_EV_EP_DONE, {0x81, 1, 0});
    writer.join();
    MIMHID_CHECK(woken == MIMHID_OK);
    // A zero timeout is refused with the wait policy only.
    MIMHID_CHECK(write(0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, data.data(), 4, 0) == MIMHID_OK);

    // Flush and stall: seq, address[, stall]. Each resets the count.
    board.clear();
    MIMHID_CHECK(mimhid_node_endpoint_flush(node, 0x81, kControlMs) == MIMHID_OK);
    std::vector<Received> f = frames_of(board, MIMHID_T_EP_FLUSH);
    MIMHID_CHECK(f.size() == 1U && f[0].payload.size() == 2U && f[0].payload[1] == 0x81);
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(write(500) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x81, kControlMs) == MIMHID_OK);
    f = frames_of(board, MIMHID_T_EP_STALL);
    MIMHID_CHECK(f.size() == 1U && f[0].payload.size() == 3U && f[0].payload[1] == 0x81 &&
                 f[0].payload[2] == 1);
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(write(500) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_clear_stall(node, 0x81, kControlMs) == MIMHID_OK);
    f = frames_of(board, MIMHID_T_EP_STALL);
    MIMHID_CHECK(f.size() == 2U && f[1].payload[2] == 0);
    MIMHID_CHECK(write(500) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_flush(node, 0x01, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x01, kControlMs) == MIMHID_OK);
    // The board refuses: the answer is the result and the count is unchanged.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_EP_STALL)
            return Reply::nak(MIMHID_NAK_STATE);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x82, kControlMs) == MIMHID_ERR_BUSY);
    board.set_policy(nullptr);

    // A new policy goes to the board and the old one stays when it refuses.
    board.clear();
    mimhid_endpoint_policy p{MIMHID_FULL_DROP_NEWEST, 5};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x82, &p, kControlMs) == MIMHID_OK);
    f = frames_of(board, MIMHID_T_EP_CONFIG);
    MIMHID_CHECK(f.size() == 1U && f[0].payload.size() == 4U && f[0].payload[1] == 0x82 &&
                 f[0].payload[2] == 5 && f[0].payload[3] == MIMHID_FULL_DROP_NEWEST);
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_EP_CONFIG)
            return Reply::nak(MIMHID_NAK_OVERFLOW);
        return std::nullopt;
    });
    p = {MIMHID_FULL_WAIT, 200};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x82, &p, kControlMs) == MIMHID_ERR_OVERFLOW);
    board.set_policy(nullptr);
    // ...so a write to it still follows the drop policy: no wait, no timeout needed.
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x82, data.data(), 4, kSendMs) == MIMHID_OK);

    // Events: a packet on an OUT endpoint, the Halt feature, lost transfers.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_seen.clear();
    }
    const std::uint8_t packet[] = {1, 2, 3};
    board.send_frame(MIMHID_T_EP_OUT_BASE | 1U, packet, sizeof(packet));
    board.send_frame(MIMHID_T_EP_OUT_BASE | 1U, packet, 0);
    board.send_event(MIMHID_EV_EP_HALT, {0x81, 1});
    board.send_event(MIMHID_EV_EP_HALT, {0x01, 0});
    board.send_event(MIMHID_EV_TRANSFER_LOST, {0x82, 0x34, 0x12});
    Seen s;
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_ENDPOINT_DATA && s.endpoint_address == 0x01 &&
                 s.interface_number == 0 && s.node_id == mimhid_node_id(node) &&
                 s.data == std::vector<std::uint8_t>(packet, packet + 3));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_ENDPOINT_DATA && s.data.empty());
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_ENDPOINT_HALT && s.value == 1 &&
                 s.endpoint_address == 0x81 && s.node_id == mimhid_node_id(node));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_ENDPOINT_HALT && s.value == 0 &&
                 s.endpoint_address == 0x01);
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_TRANSFER_LOST && s.value == 0x1234 &&
                 s.endpoint_address == 0x82 && s.node_id == mimhid_node_id(node));

    // A profile Node has none of these calls in this release.
    mimhid_spec* spec = keyboard_spec();
    mimhid_hid_node_config hid = node_config(1);
    hid.ep_in.address = 0x85; // 0x82 is the vendor Node's
    mimhid_node* keys = nullptr;
    MIMHID_CHECK(mimhid_node_open_profile(device, spec, &hid, &kCommit, &keys) == MIMHID_OK);
    if (keys != nullptr) {
        MIMHID_CHECK(mimhid_node_endpoint_write(keys, 0x85, data.data(), 1, kSendMs) ==
                     MIMHID_ERR_UNSUPPORTED);
        MIMHID_CHECK(mimhid_node_endpoint_stall(keys, 0x85, kControlMs) == MIMHID_ERR_UNSUPPORTED);
        MIMHID_CHECK(mimhid_node_endpoint_flush(keys, 0x85, kControlMs) == MIMHID_ERR_UNSUPPORTED);
        // The HID endpoint's policy is DROP_NEWEST only; its depth goes to the board.
        board.clear();
        p = {MIMHID_FULL_WAIT, 2};
        MIMHID_CHECK(mimhid_node_endpoint_set_policy(keys, hid.ep_in.address, &p, kControlMs) ==
                     MIMHID_ERR_UNSUPPORTED);
        p = {MIMHID_FULL_DROP_NEWEST, 2};
        MIMHID_CHECK(mimhid_node_endpoint_set_policy(keys, hid.ep_in.address, &p, kControlMs) ==
                     MIMHID_OK);
        f = frames_of(board, MIMHID_T_EP_CONFIG);
        MIMHID_CHECK(f.size() == 1U && f[0].payload[1] == hid.ep_in.address && f[0].payload[2] == 2);
        MIMHID_CHECK(mimhid_node_close(keys, &kCommit, kDrainMs) == MIMHID_OK);
    }
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_generic() { test_generic_frames(); }
