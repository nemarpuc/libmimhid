// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* mimhid_node_open_raw: a descriptor sent as given, its arguments, the board's room for nodes and
 * descriptors, a refused commit that gives the slot back, and the batch. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"

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
    mimhid_node_config config = node_config();
    config.iface.name = "raw interface";
    mimhid_node* node = nullptr;
    board.clear();
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    Received data;
    MIMHID_CHECK(board.take(MIMHID_T_NODE_DATA, &data, 0) &&
                 data.payload.size() == 4U + sizeof(kDescriptor) && data.payload[4] == 0x05);
    Received begin;
    MIMHID_CHECK(board.take(MIMHID_T_NODE_BEGIN, &begin, 0) && begin.payload.size() > 3U &&
                 begin.payload[3] == sizeof(kDescriptor));
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    // Its reports go through mimhid_report_send, without checks.
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(board.take(MIMHID_T_REPORT_BASE, &data) && data.payload.size() == 2U);
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
    pong.max_nodes = 1;
    pong.max_descriptor_bytes = 32;
    board.set_pong(pong);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    const mimhid_node_config config = node_config();
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
    mimhid_node_config unset = config;
    unset.ep_in.max_packet_size = 0;
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &unset, &kCommit,
                                      &node) == MIMHID_ERR_UNSET_FIELD);
    // Longer than the board holds.
    const std::vector<std::uint8_t> big(33, 0x00);
    MIMHID_CHECK(mimhid_node_open_raw(device, big.data(), big.size(), &config, &kCommit, &node) ==
                 MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(node == nullptr);

    // A refused commit gives the slot back; the board then holds its one node.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_NODE_BEGIN)
            return Reply::nak(MIMHID_NAK_PARAM);
        return std::nullopt;
    });
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_ERR_PARAM);
    board.set_policy(nullptr);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    mimhid_node* second = nullptr;
    const mimhid_node_config other = node_config(1);
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &other, &kCommit,
                                      &second) == MIMHID_ERR_OVERFLOW);
    mimhid_spec* spec = keyboard_spec();
    MIMHID_CHECK(mimhid_node_open(device, spec, &other, &kCommit, &second) == MIMHID_ERR_OVERFLOW);
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
    const mimhid_node_config config = node_config();
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    board.clear();
    MIMHID_CHECK(mimhid_node_open_raw(device, kDescriptor, sizeof(kDescriptor), &config, &kCommit,
                                      &node) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_NODE_BEGIN) == 0U);
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_batch_end(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(board.count(MIMHID_T_NODE_BEGIN) == 1U);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_report_send(node, report, sizeof(report), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_raw_node() {
    test_open_raw_sends_the_descriptor();
    test_open_raw_arguments();
    test_open_raw_in_a_batch();
}
