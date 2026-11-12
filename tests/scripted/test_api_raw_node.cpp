// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* An interface of class HID written by hand with mimusb_node_open (the HID descriptor, a report
 * descriptor rule, mimusb_node_endpoint_write): what goes to the board, its arguments, the board's
 * room for rules, a refused commit that gives the slot back, and the batch; and Nodes of a raw Spec
 * sharing one interface: interface numbers, many Nodes, the queue depth. */
#include "mimusb_proto.h"
#include "scripted_support.hpp"

#include <cstring>
#include <optional>
#include <vector>

using namespace scripted;

namespace {

constexpr std::uint8_t kDescriptor[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85,
                                        0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                                        0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0xC0};

// One interface of class HID the way a program writes it: the HID descriptor as the interface's
// class bytes, the report descriptor as an ANSWER rule (HID 1.11 7.1.1), an interrupt IN endpoint.
struct HandWritten {
    std::vector<std::uint8_t> descriptor;
    std::uint8_t hid[9]{};
    mimusb_usb_endpoint endpoint{};
    mimusb_usb_item items[2]{};
    mimusb_usb_setting setting{};
    mimusb_usb_interface itf{};
    mimusb_control_rule rule{};
    mimusb_node_config config{};

    HandWritten(const std::uint8_t* bytes, const std::size_t length, const std::uint8_t number) {
        descriptor.assign(bytes, bytes + length);
        MIMUSB_CHECK(mimusb_hid_descriptor_bytes(0x0111, 0, length, hid) == MIMUSB_OK);
        MIMUSB_CHECK(mimusb_hid_descriptor_rule(number, 0x22, descriptor.data(), length, &rule) ==
                     MIMUSB_OK);
        endpoint.address = static_cast<std::uint8_t>(0x81U + number);
        endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        endpoint.max_packet_size = 64;
        endpoint.interval = 1;
        endpoint.policy = {MIMUSB_FULL_DROP_NEWEST, 4};
        itf.number = number;
        itf.class_code = 0x03;
        itf.name = "hand written";
        items[0] = mimusb_usb_item_make_descriptor(hid, sizeof(hid));
        items[1] = mimusb_usb_item_make_endpoint(endpoint);
        setting.items = items;
        setting.item_count = 2;
        itf.settings = &setting;
        itf.setting_count = 1;
        mimusb_node_config_init(&config);
        config.interfaces = &itf;
        config.interface_count = 1;
        config.rules = &rule;
        config.rule_count = 1;
    }
    HandWritten(const HandWritten&) = delete;
    HandWritten& operator=(const HandWritten&) = delete;
};

void test_hand_written_hid_sends_the_rule() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    HandWritten hid(kDescriptor, sizeof(kDescriptor), 0);
    mimusb_node* node = nullptr;
    board.clear();
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, &kCommit, &node) == MIMUSB_OK);
    // The rule goes as RULE (seq, index, masks and values, action, capacity), then its bytes as
    // DESC_BEGIN (seq, kind, index, language (2), total length (2), flags) and DESC_DATA (seq,
    // kind, index, language (2), offset (2), bytes) of the kind ANSWER, the index being the rule's.
    Received data;
    Received begin;
    Received rule;
    for (const Received& r : board.received()) {
        if (r.type == MIMUSB_T_DESC_DATA && r.payload.size() > 1U &&
            r.payload[1] == MIMUSB_DESC_ANSWER)
            data = r;
        if (r.type == MIMUSB_T_DESC_BEGIN && r.payload.size() > 1U &&
            r.payload[1] == MIMUSB_DESC_ANSWER)
            begin = r;
        if (r.type == MIMUSB_T_RULE)
            rule = r;
    }
    MIMUSB_CHECK(rule.payload.size() > 1U && rule.payload[1] == 0U && rule.payload.back() == 0U &&
                 rule.payload[rule.payload.size() - 2U] == sizeof(kDescriptor));
    MIMUSB_CHECK(data.payload.size() == 7U + sizeof(kDescriptor) && data.payload[7] == 0x05);
    MIMUSB_CHECK(begin.payload.size() == 8U && begin.payload[2] == 0U &&
                 begin.payload[5] == sizeof(kDescriptor) && begin.payload[6] == 0U);
    // No frame of the old HID report descriptor kind exists any more: nothing of kind 4 goes.
    for (const Received& r : board.received()) {
        if (r.type == MIMUSB_T_DESC_BEGIN && r.payload.size() > 1U)
            MIMUSB_CHECK(r.payload[1] != 4U);
    }
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    // Its reports go through mimusb_node_endpoint_write, without checks.
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, report, sizeof(report), kSendMs) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(board.take((MIMUSB_T_EP_IN_BASE | 1), &data) && data.payload.size() == 2U);
    // It has no generated input: mimusb_node_submit is refused, it has no Spec.
    MIMUSB_CHECK(mimusb_node_submit(node, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

void test_hand_written_arguments() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_interfaces_any = 1;
    pong.max_answer_bytes = 32;
    board.set_pong(pong);
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    HandWritten hid(kDescriptor, sizeof(kDescriptor), 0);
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, &kCommit, nullptr) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_open(nullptr, &hid.config, &kCommit, &node) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_open(device, nullptr, &kCommit, &node) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, nullptr, &node) == MIMUSB_ERR_PARAM);
    mimusb_control_rule none = hid.rule;
    mimusb_node_config unset = hid.config;
    none.action = static_cast<mimusb_control_rule_action>(0);
    unset.rules = &none;
    MIMUSB_CHECK(mimusb_node_open(device, &unset, &kCommit, &node) == MIMUSB_ERR_UNSET_FIELD);
    // The helper refuses what it cannot make.
    mimusb_control_rule made{};
    MIMUSB_CHECK(mimusb_hid_descriptor_rule(0, 0x23, kDescriptor, sizeof(kDescriptor), &made) ==
                 MIMUSB_ERR_PARAM);
    std::uint8_t nine[9];
    MIMUSB_CHECK(mimusb_hid_descriptor_bytes(0, 0, 10, nine) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_hid_descriptor_bytes(0x0111, 0, 0, nine) == MIMUSB_ERR_PARAM);
    // More answer bytes than the board holds.
    const std::vector<std::uint8_t> big(33, 0x00);
    HandWritten too_big(big.data(), big.size(), 0);
    MIMUSB_CHECK(mimusb_node_open(device, &too_big.config, &kCommit, &node) == MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(node == nullptr);

    // A refused commit gives the slot back; the board then holds its one interface.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMUSB_T_DESC_BEGIN)
            return Reply::nak(MIMUSB_NAK_PARAM);
        return std::nullopt;
    });
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, &kCommit, &node) == MIMUSB_ERR_PARAM);
    board.set_policy(nullptr);
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, &kCommit, &node) == MIMUSB_OK);
    mimusb_node* second = nullptr;
    HandWritten other(kDescriptor, sizeof(kDescriptor), 1);
    MIMUSB_CHECK(mimusb_node_open(device, &other.config, &kCommit, &second) == MIMUSB_ERR_OVERFLOW);
    mimusb_spec* spec = keyboard_spec();
    const mimusb_hid_node_config profile = node_config(1);
    MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &profile, &kCommit, &second) ==
                 MIMUSB_ERR_OVERFLOW);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

void test_hand_written_in_a_batch() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    HandWritten hid(kDescriptor, sizeof(kDescriptor), 0);
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    board.clear();
    MIMUSB_CHECK(mimusb_node_open(device, &hid.config, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_BEGIN) == 0U && board.count(MIMUSB_T_RULE) == 0U);
    const std::uint8_t report[2] = {0x04, 0x01};
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, report, sizeof(report), kSendMs) ==
                 MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_DESC_BEGIN) >= 3U && board.count(MIMUSB_T_RULE) == 1U);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, report, sizeof(report), kSendMs) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// A report descriptor with one Report ID, for Nodes that share an interface.
std::vector<std::uint8_t> descriptor_with_id(const std::uint8_t id) {
    return {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, id,   0x09, 0x02, 0x15,
            0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};
}

// A raw Spec of such a descriptor: its one Input report is the Report ID and a byte.
mimusb_spec* raw_spec(const std::uint8_t id) {
    static std::vector<std::vector<std::uint8_t>> keep;
    keep.push_back(descriptor_with_id(id));
    mimusb_raw_report report{};
    report.report_id.enabled = 1;
    report.report_id.value = id;
    report.wire_length = 2;
    mimusb_raw_options options;
    mimusb_raw_options_init(&options);
    options.descriptor = keep.back().data();
    options.descriptor_length = keep.back().size();
    options.reports = &report;
    options.report_count = 1;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_raw(&options, &spec) == MIMUSB_OK);
    return spec;
}

// iface.number is the bInterfaceNumber: a gap is refused with the missing number in the error
// detail, before anything is sent, and the Node that opens the gap makes it fine.
void test_interface_numbers_are_real() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_node* nodes[3] = {};
    HandWritten h0(kDescriptor, sizeof(kDescriptor), 0);
    HandWritten h1(kDescriptor, sizeof(kDescriptor), 1);
    HandWritten h2(kDescriptor, sizeof(kDescriptor), 2);
    MIMUSB_CHECK(mimusb_node_open(device, &h0.config, &kCommit, &nodes[0]) == MIMUSB_OK);
    board.clear();
    mimusb_node* refused = nullptr;
    MIMUSB_CHECK(mimusb_node_open(device, &h2.config, &kCommit, &refused) == MIMUSB_ERR_PARAM);
    const mimusb_error_detail* e = mimusb_last_error();
    MIMUSB_CHECK(refused == nullptr &&
                 std::strcmp(e->field, "node_config.interfaces.number") == 0 && e->offset == 1U);
    // The refused Node's interface was never sent to the board: nothing of its endpoint 0x83.
    bool sent_interface_2 = false;
    for (const Received& q : board.received()) {
        if (q.type == MIMUSB_T_EP_CONFIG && q.payload.size() > 1U && q.payload[1] == 0x83)
            sent_interface_2 = true;
    }
    MIMUSB_CHECK(!sent_interface_2);
    // The refused Node left no slot taken.
    MIMUSB_CHECK(mimusb_node_open(device, &h1.config, &kCommit, &nodes[1]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_open(device, &h2.config, &kCommit, &nodes[2]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_id(nodes[2]) == 3U);

    // Closing the middle one would leave 0 and 2: the Node is closed, the commit is refused.
    board.clear();
    MIMUSB_CHECK(mimusb_node_close(nodes[1], &kCommit, kDrainMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_last_error()->offset == 1U && board.count(MIMUSB_T_DESC_RESET) == 0U);
    // The highest goes first.
    MIMUSB_CHECK(mimusb_node_close(nodes[2], &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_close(nodes[0], &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The wire names an endpoint, not a Node: many more Nodes than the old 16 can share an interface,
// and a report goes out on the endpoint of its Node's interface.
void test_more_than_sixteen_nodes() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_answer_bytes = 4096;
    pong.max_control_rules = 120; // each Report ID has its own GET_REPORT rule
    board.set_pong(pong);
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    const mimusb_hid_node_config config = node_config(0);
    std::vector<mimusb_node*> nodes;
    std::vector<mimusb_spec*> specs;
    for (std::uint8_t id = 1; id <= 20; ++id) {
        mimusb_spec* spec = raw_spec(id);
        specs.push_back(spec);
        mimusb_node* node = nullptr;
        MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &kCommit, &node) == MIMUSB_OK);
        nodes.push_back(node);
    }
    MIMUSB_CHECK(nodes.size() == 20U && mimusb_node_id(nodes.back()) == 20U);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 1000) == MIMUSB_OK);
    board.clear();
    const std::uint8_t report[2] = {20, 0x01};
    MIMUSB_CHECK(mimusb_raw_submit(nodes.back(), report, sizeof(report), kSendMs) == MIMUSB_OK);
    Received r;
    MIMUSB_CHECK(board.take(MIMUSB_T_EP_IN_BASE | 1, &r) && r.payload.size() == 2U);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        MIMUSB_CHECK(mimusb_node_close(*it, &kCommit, kDrainMs) == MIMUSB_OK);
    for (mimusb_spec* spec : specs)
        mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The endpoint of an interface is shared by its Nodes: EP_CONFIG carries the least depth asked for,
// with the policy that drops the newest report; a commit sends it again.
void test_queue_depth_is_the_least_of_the_interface() {
    Board board;
    if (!board.start())
        return;
    mimusb_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimusb_hid_node_config config = node_config(0);
    config.ep_in.policy.depth = 15; // what the Nodes ask for until one of them sets another
    mimusb_spec* sa = raw_spec(1);
    mimusb_spec* sb = raw_spec(2);
    mimusb_node* a = nullptr;
    mimusb_node* b = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, sa, &config, &kCommit, &a) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_open_profile(device, sb, &config, &kCommit, &b) == MIMUSB_OK);
    const auto set_depth = [](mimusb_node* node, std::uint8_t depth) {
        const mimusb_endpoint_policy policy{MIMUSB_FULL_DROP_NEWEST, depth};
        return mimusb_node_endpoint_set_policy(node, 0x81, &policy, kControlMs);
    };
    const auto last_ep_config = [&board]() {
        Received found;
        for (const Received& r : board.received()) {
            if (r.type == MIMUSB_T_EP_CONFIG)
                found = r;
        }
        return found;
    };
    board.clear();
    // payload: seq, endpoint address, depth, policy
    MIMUSB_CHECK(set_depth(a, 8) == MIMUSB_OK);
    Received r = last_ep_config();
    MIMUSB_CHECK(r.payload.size() == 4U && r.payload[1] == 0x81 && r.payload[2] == 8U &&
                 r.payload[3] == MIMUSB_EP_FULL_DROP_NEWEST);
    MIMUSB_CHECK(set_depth(b, 4) == MIMUSB_OK);
    r = last_ep_config();
    MIMUSB_CHECK(r.payload.size() == 4U && r.payload[2] == 4U);
    MIMUSB_CHECK(set_depth(a, 2) == MIMUSB_OK);
    MIMUSB_CHECK(last_ep_config().payload[2] == 2U);
    MIMUSB_CHECK(set_depth(a, 16) == MIMUSB_OK);
    MIMUSB_CHECK(last_ep_config().payload[2] == 4U); // the other Node still wants 4

    // A commit sends the depth again.
    board.clear();
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(board.count(MIMUSB_T_EP_CONFIG) == 1U && last_ep_config().payload[2] == 4U);
    // A refused depth leaves the Node's old value: a later commit does not send the refused one.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_EP_CONFIG)
            return Reply::nak(MIMUSB_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    MIMUSB_CHECK(set_depth(b, 1) == MIMUSB_ERR_UNSUPPORTED);
    board.set_policy(nullptr);
    board.clear();
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(last_ep_config().payload[2] == 4U);
    MIMUSB_CHECK(mimusb_node_close(b, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_close(a, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(sa);
    mimusb_spec_release(sb);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace

void test_api_raw_node() {
    test_hand_written_hid_sends_the_rule();
    test_hand_written_arguments();
    test_hand_written_in_a_batch();
    test_interface_numbers_are_real();
    test_more_than_sixteen_nodes();
    test_queue_depth_is_the_least_of_the_interface();
}
