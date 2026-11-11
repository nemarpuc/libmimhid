// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* mimhid_node_open_raw: a descriptor sent as given, its arguments, the board's room for nodes and
 * descriptors, a refused commit that gives the slot back, and the batch. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"

#include <cstring>
#include <optional>
#include <vector>

using namespace scripted;

namespace {

constexpr std::uint8_t kDescriptor[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85,
                                        0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                                        0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0xC0};

void test_open_raw_sends_the_descriptor() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_hid_node_config config = node_config();
    config.name = "raw interface";
    mimhid_node* node = nullptr;
    board.clear();
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    // The report descriptor of interface 0 goes as DESC_BEGIN (seq, kind, index, language (2),
    // total length (2), flags) and DESC_DATA (seq, kind, index, language (2), offset (2), bytes).
    Received data;
    Received begin;
    for (const Received& r : board.received()) {
        if (r.type == MIMHID_T_DESC_DATA && r.payload.size() > 1U &&
            r.payload[1] == MIMHID_DESC_HID_REPORT)
            data = r;
        if (r.type == MIMHID_T_DESC_BEGIN && r.payload.size() > 1U &&
            r.payload[1] == MIMHID_DESC_HID_REPORT)
            begin = r;
    }
    MIMHID_CHECK(data.payload.size() == 7U + sizeof(kDescriptor) && data.payload[7] == 0x05);
    MIMHID_CHECK(begin.payload.size() == 8U && begin.payload[2] == 0U &&
                 begin.payload[5] == sizeof(kDescriptor) && begin.payload[6] == 0U);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    // Its reports go through mimhid_report_send, without checks.
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(board.take((MIMHID_T_EP_IN_BASE | 1), &data) && data.payload.size() == 2U);
    // It has no generated input: mimhid_node_submit is refused.
    MIMHID_CHECK(mimhid_node_submit(node, kSendMs) == MIMHID_ERR_UNSUPPORTED);
    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

void test_open_raw_arguments() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_interfaces = 1;
    pong.max_descriptor_bytes = 32;
    board.set_pong(pong);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    const mimhid_hid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open_raw(device, nullptr, 4, &config, &kCommit, &node) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, 0, &config, &kCommit, &node) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open_raw(nullptr, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), nullptr, &kCommit,
                                      &node) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, nullptr,
                                      &node) == MIMHID_ERR_PARAM);
    mimhid_hid_node_config unset = config;
    unset.ep_in.max_packet_size = 0;
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &unset, &kCommit,
                                      &node) == MIMHID_ERR_UNSET_FIELD);
    // Longer than the board holds.
    const std::vector<std::uint8_t> big(33, 0x00);
    MIMHID_CHECK(mimhid_node_open_raw(device, big.data(), big.size(), &config, &kCommit, &node) ==
                 MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(node == nullptr);

    // A refused commit gives the slot back; the board then holds its one interface.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_DESC_BEGIN)
            return Reply::nak(MIMHID_NAK_PARAM);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_ERR_PARAM);
    board.set_policy(nullptr);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    mimhid_node* second = nullptr;
    const mimhid_hid_node_config other = node_config(1);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &other, &kCommit,
                                      &second) == MIMHID_ERR_OVERFLOW);
    mimhid_spec* spec = keyboard_spec();
    MIMHID_CHECK(mimhid_node_open_profile(device, spec, &other, &kCommit, &second) == MIMHID_ERR_OVERFLOW);
    mimhid_spec_release(spec);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

void test_open_raw_in_a_batch() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    const mimhid_hid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    board.clear();
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_BEGIN) == 0U);
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_DESC_BEGIN) >= 3U);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// A report descriptor with one Report ID, for Nodes that share an interface.
std::vector<std::uint8_t> descriptor_with_id(const std::uint8_t id) {
    return {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, id,
            0x09, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};
}

// iface.number is the bInterfaceNumber: a gap is refused with the missing number in the error
// detail, before anything is sent, and the Node that opens the gap makes it fine.
void test_interface_numbers_are_real() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_node* nodes[3] = {};
    const mimhid_hid_node_config c0 = node_config(0);
    const mimhid_hid_node_config c1 = node_config(1);
    const mimhid_hid_node_config c2 = node_config(2);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &c0, &kCommit,
                                      &nodes[0]) == MIMHID_OK);
    board.clear();
    mimhid_node* refused = nullptr;
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &c2, &kCommit,
                                      &refused) == MIMHID_ERR_PARAM);
    const mimhid_error_detail* e = mimhid_last_error();
    MIMHID_CHECK(refused == nullptr && std::strcmp(e->field, "node_config.interfaces.number") == 0 &&
                 e->offset == 1U);
    // The refused Node's interface was never sent to the board (the library puts the old
    // configuration back, which is the interface 0 alone).
    bool sent_interface_2 = false;
    for (const Received& q : board.received()) {
        if (q.type == MIMHID_T_DESC_BEGIN && q.payload.size() > 2U &&
            q.payload[1] == MIMHID_DESC_HID_REPORT && q.payload[2] == 2U)
            sent_interface_2 = true;
    }
    MIMHID_CHECK(!sent_interface_2);
    // The refused Node left no slot taken.
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &c1, &kCommit,
                                      &nodes[1]) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &c2, &kCommit,
                                      &nodes[2]) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_id(nodes[2]) == 3U);

    // Closing the middle one would leave 0 and 2: the Node is closed, the commit is refused.
    board.clear();
    MIMHID_CHECK(mimhid_node_close(nodes[1], &kCommit, kDrainMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_last_error()->offset == 1U && board.count(MIMHID_T_DESC_RESET) == 0U);
    // The highest goes first.
    MIMHID_CHECK(mimhid_node_close(nodes[2], &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_close(nodes[0], &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The wire names an endpoint, not a Node: many more Nodes than the old 16 can share an interface,
// and a report goes out on the endpoint of its Node's interface.
void test_more_than_sixteen_nodes() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_descriptor_bytes = 2048;
    board.set_pong(pong);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    const mimhid_hid_node_config config = node_config(0);
    std::vector<mimhid_node*> nodes;
    for (std::uint8_t id = 1; id <= 20; ++id) {
        const std::vector<std::uint8_t> descriptor = descriptor_with_id(id);
        mimhid_node* node = nullptr;
        MIMHID_CHECK(mimhid_node_open_raw(device, descriptor.data(), descriptor.size(), &config,
                                          &kCommit, &node) == MIMHID_OK);
        nodes.push_back(node);
    }
    MIMHID_CHECK(nodes.size() == 20U && mimhid_node_id(nodes.back()) == 20U);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    board.clear();
    const std::uint8_t report[2] = {20, 0x01};
    MIMHID_CHECK(mimhid_report_send(nodes.back(), report, sizeof(report), kSendMs) == MIMHID_OK);
    Received r;
    MIMHID_CHECK(board.take(MIMHID_T_EP_IN_BASE | 1, &r) && r.payload.size() == 2U);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        MIMHID_CHECK(mimhid_node_close(*it, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The endpoint of an interface is shared by its Nodes: EP_CONFIG carries the least depth asked for,
// with the policy that drops the newest report; a commit sends it again.
void test_queue_depth_is_the_least_of_the_interface() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_hid_node_config config = node_config(0);
    config.ep_in.policy.depth = 15; // what the Nodes ask for until one of them sets another
    const std::vector<std::uint8_t> da = descriptor_with_id(1);
    const std::vector<std::uint8_t> db = descriptor_with_id(2);
    mimhid_node* a = nullptr;
    mimhid_node* b = nullptr;
    MIMHID_CHECK(mimhid_node_open_raw(device, da.data(), da.size(), &config, &kCommit, &a) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid_node_open_raw(device, db.data(), db.size(), &config, &kCommit, &b) ==
                 MIMHID_OK);
    const auto set_depth = [](mimhid_node* node, std::uint8_t depth) {
        const mimhid_endpoint_policy policy{MIMHID_FULL_DROP_NEWEST, depth};
        return mimhid_node_endpoint_set_policy(node, 0x81, &policy, kControlMs);
    };
    const auto last_ep_config = [&board]() {
        Received found;
        for (const Received& r : board.received()) {
            if (r.type == MIMHID_T_EP_CONFIG)
                found = r;
        }
        return found;
    };
    board.clear();
    // payload: seq, endpoint address, depth, policy
    MIMHID_CHECK(set_depth(a, 8) == MIMHID_OK);
    Received r = last_ep_config();
    MIMHID_CHECK(r.payload.size() == 4U && r.payload[1] == 0x81 && r.payload[2] == 8U &&
                 r.payload[3] == MIMHID_EP_FULL_DROP_NEWEST);
    MIMHID_CHECK(set_depth(b, 4) == MIMHID_OK);
    r = last_ep_config();
    MIMHID_CHECK(r.payload.size() == 4U && r.payload[2] == 4U);
    MIMHID_CHECK(set_depth(a, 2) == MIMHID_OK);
    MIMHID_CHECK(last_ep_config().payload[2] == 2U);
    MIMHID_CHECK(set_depth(a, 16) == MIMHID_OK);
    MIMHID_CHECK(last_ep_config().payload[2] == 4U); // the other Node still wants 4

    // A commit sends the depth again.
    board.clear();
    MIMHID_CHECK(mimhid_device_commit(device, 0, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_EP_CONFIG) == 1U && last_ep_config().payload[2] == 4U);
    // A refused depth leaves the Node's old value: a later commit does not send the refused one.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_EP_CONFIG)
            return Reply::nak(MIMHID_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    MIMHID_CHECK(set_depth(b, 1) == MIMHID_ERR_UNSUPPORTED);
    board.set_policy(nullptr);
    board.clear();
    MIMHID_CHECK(mimhid_device_commit(device, 0, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(last_ep_config().payload[2] == 4U);
    MIMHID_CHECK(mimhid_node_close(b, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_close(a, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_raw_node() {
    test_open_raw_sends_the_descriptor();
    test_open_raw_arguments();
    test_open_raw_in_a_batch();
    test_interface_numbers_are_real();
    test_more_than_sixteen_nodes();
    test_queue_depth_is_the_least_of_the_interface();
}
