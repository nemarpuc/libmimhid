// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Control transfers from the target: the rules (ASK, ANSWER, ANSWER_LAST_SENT, ACCEPT, STALL), their
 * order, changing them while the target is attached, held transfers and the replies to them. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sim {

namespace {

mimhid_control_rule control_rule(std::uint8_t bm, std::uint8_t request, std::uint32_t timeout_ms,
                                 mimhid_control_action when_late) {
    mimhid_control_rule r{};
    r.request_type_mask = 0xFF;
    r.request_type_value = bm;
    r.request_mask = 0xFF;
    r.request_value = request;
    r.action = MIMHID_CONTROL_ASK;
    r.timeout_ms = timeout_ms;
    r.when_late = when_late;
    return r;
}

// Sends a request from the target. Events of earlier transfers that nobody read are dropped first.
void target_control(std::uint8_t bm, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                    std::uint16_t length, const std::uint8_t* out = nullptr,
                    std::size_t out_length = 0) {
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    const std::uint8_t setup[8] = {bm,
                                   request,
                                   static_cast<std::uint8_t>(value),
                                   static_cast<std::uint8_t>(value >> 8),
                                   static_cast<std::uint8_t>(index),
                                   static_cast<std::uint8_t>(index >> 8),
                                   static_cast<std::uint8_t>(length),
                                   static_cast<std::uint8_t>(length >> 8)};
    fake_board_target_control(setup, out, out_length);
}

// The next report of the request with this type and number.
bool wait_setup(EventRecord* ev, std::uint8_t bm, std::uint8_t request, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_event(MIMHID_EVENT_CONTROL_SETUP, ev, 100))
            continue;
        if (ev->data.size() == 8 && ev->data[0] == bm && ev->data[1] == request)
            return true;
    }
    return false;
}

bool next_control(fake_control_result_t* r, int timeout_ms = 2000) {
    return fake_board_wait_control(r, static_cast<std::uint32_t>(timeout_ms)) != 0;
}

} // namespace

void test_control() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_node* node = open_vendor_node(device); // the rules live in a USB configuration
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_control_rules == 160 && limits.max_control_data_bytes == 1024 &&
                 limits.max_answer_bytes == 4096);

    // Arguments.
    const mimhid_control_rule good = control_rule(0xC0, 0x42, 100, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(nullptr, 0, &good, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 2, &good, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, nullptr, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    // More rules than the board holds are refused before anything is sent.
    std::vector<mimhid_control_rule> many(limits.max_control_rules + 1U, good);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, many.data(), many.size(), kControlMs) ==
                 MIMHID_ERR_OVERFLOW);
    // USB 2.0 9.2.6.1: 5 seconds is the most any command may take.
    MIMHID_CHECK(MIMHID_CONTROL_MAX_TIMEOUT_MS == 5000);
    mimhid_control_rule longest = good;
    longest.timeout_ms = 5000;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &longest, 1, kControlMs) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &good, 1, kControlMs) == MIMHID_OK);
    mimhid_control_rule bad = good;
    bad.timeout_ms = 0;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = 0;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.action = static_cast<mimhid_control_rule_action>(0);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = MIMHID_CONTROL_ACTION_LEAVE; // an ASK rule ends in STALL or DROP
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    bad = good;
    bad.when_late = 5;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    bad = good;
    bad.timeout_ms = MIMHID_CONTROL_MAX_TIMEOUT_MS + 1;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    bad = good;
    bad.value_value = 1; // outside its mask of zero
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    const std::uint8_t one[1] = {1};
    MIMHID_CHECK(mimhid_device_control_reply(device, 0, MIMHID_CONTROL_ACTION_ANSWER, nullptr, 0,
                                             kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 256, MIMHID_CONTROL_ACTION_ANSWER, nullptr, 0,
                                             kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, 0, nullptr, 0, kSendMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, MIMHID_CONTROL_ACTION_STALL, one, 1,
                                             kSendMs) == MIMHID_ERR_PARAM);
    std::vector<std::uint8_t> too_big(1025);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, MIMHID_CONTROL_ACTION_ANSWER,
                                             too_big.data(), too_big.size(),
                                             kSendMs) == MIMHID_ERR_OVERFLOW);

    // Reporting every transfer, holding none.
    fake_control_result_t r{};
    EventRecord ev;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 1, nullptr, 0, kControlMs) == MIMHID_OK);
    target_control(0x80, 6, 0x0302, 0x0409, 255);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6) && ev.flags == 0 && ev.token != 0 &&
                 ev.data.size() == 8 && ev.data[0] == 0x80 && ev.data[1] == 6 &&
                 ev.data[2] == 0x02 && ev.data[3] == 0x03 && ev.data[4] == 0x09 &&
                 ev.data[5] == 0x04 && ev.data[6] == 255);
    target_control(0x00, 5, 9, 0, 0); // SET_ADDRESS
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(wait_setup(&ev, 0x00, 5) && ev.flags == MIMHID_CONTROL_FLAG_FIXED);

    // An answer with data, given by the program's own thread.
    const mimhid_control_rule vendor_in =
        control_rule(0xC0, 0x42, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &vendor_in, 1, kControlMs) ==
                 MIMHID_OK);
    target_control(0xC0, 0x42, 0x1234, 0x5678, 64);
    MIMHID_CHECK(wait_setup(&ev, 0xC0, 0x42) && ev.flags == MIMHID_CONTROL_FLAG_HELD &&
                 ev.data[2] == 0x34 && ev.data[3] == 0x12);
    const std::vector<std::uint8_t> payload = {9, 8, 7, 6, 5};
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER,
                                             payload.data(), payload.size(), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED &&
                 r.length == payload.size() &&
                 std::memcmp(r.data, payload.data(), payload.size()) == 0);

    // The same, answered from inside the event sink; the whole answer goes out even when wLength is
    // shorter, as a device that overruns the host's length does.
    g_sink_reply.device = device;
    g_sink_reply.action = MIMHID_CONTROL_ACTION_ANSWER;
    g_sink_reply.data = payload;
    g_sink_reply.enabled.store(true);
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED &&
                 r.length == payload.size() &&
                 std::memcmp(r.data, payload.data(), payload.size()) == 0);
    MIMHID_CHECK(g_sink_reply.result.load() == MIMHID_OK);

    // A zero-length request answered with data still gets its data stage, as from the real device;
    // an empty answer is the status stage alone.
    target_control(0xC0, 0x42, 0, 0, 0);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED &&
                 r.length == payload.size() &&
                 std::memcmp(r.data, payload.data(), payload.size()) == 0);
    g_sink_reply.data.clear();
    target_control(0xC0, 0x42, 0, 0, 0);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED);
    g_sink_reply.data = payload;
    g_sink_reply.action = MIMHID_CONTROL_ACTION_STALL;
    g_sink_reply.data.clear();
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    g_sink_reply.enabled.store(false);

    // Nobody answers: the rule's action after its time (a stall here).
    const mimhid_control_rule quick = control_rule(0xC0, 0x42, 60, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &quick, 1, kControlMs) == MIMHID_OK);
    const auto start = std::chrono::steady_clock::now();
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    const auto waited = std::chrono::steady_clock::now() - start;
    MIMHID_CHECK(waited >= std::chrono::milliseconds(55) &&
                 waited < std::chrono::milliseconds(1000));

    // A request the program leaves to the board: the stack gets it as it came.
    const mimhid_control_rule std_get = control_rule(0x80, 6, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &std_get, 1, kControlMs) == MIMHID_OK);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_LEAVE, nullptr,
                                             0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT && r.setup[0] == 0x80);

    // A request without a data stage: ANSWER is an acknowledgement.
    const mimhid_control_rule no_data = control_rule(0x00, 3, 500, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &no_data, 1, kControlMs) ==
                 MIMHID_OK);
    target_control(0x00, 3, 1, 0, 0);
    MIMHID_CHECK(wait_setup(&ev, 0x00, 3));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER,
                                             nullptr, 0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED);

    // Host to device with a data stage: the program sees the data and gives the verdict.
    const mimhid_control_rule set_report = control_rule(0x21, 9, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &set_report, 1, kControlMs) ==
                 MIMHID_OK);
    const std::uint8_t sent[3] = {0x11, 0x22, 0x33};
    for (int round = 0; round < 2; ++round) {
        target_control(0x21, 9, 0x0302, 0, 3, sent, sizeof(sent));
        MIMHID_CHECK(wait_setup(&ev, 0x21, 9) && ev.flags == MIMHID_CONTROL_FLAG_HELD);
        MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER,
                                                 nullptr, 0, kSendMs) == MIMHID_OK);
        EventRecord data_ev;
        MIMHID_CHECK(wait_event(MIMHID_EVENT_CONTROL_DATA, &data_ev) && data_ev.token == ev.token &&
                     data_ev.data.size() == 3 && std::memcmp(data_ev.data.data(), sent, 3) == 0);
        MIMHID_CHECK(mimhid_device_control_reply(device, data_ev.token,
                                                 round == 0 ? MIMHID_CONTROL_ACTION_ANSWER
                                                            : MIMHID_CONTROL_ACTION_STALL,
                                                 nullptr, 0, kSendMs) == MIMHID_OK);
        MIMHID_CHECK(next_control(&r) &&
                     r.outcome == (round == 0 ? FAKE_CONTROL_ACKED : FAKE_CONTROL_STALLED));
        MIMHID_CHECK(r.length == 3 && std::memcmp(r.data, sent, 3) == 0);
    }
    // A host-to-device transfer whose verdict never comes: the rule's action.
    const mimhid_control_rule slow = control_rule(0x21, 9, 50, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &slow, 1, kControlMs) == MIMHID_OK);
    g_sink_reply.action = MIMHID_CONTROL_ACTION_ANSWER;
    g_sink_reply.data.clear();
    g_sink_reply.enabled.store(true);
    target_control(0x21, 9, 0x0302, 0, 3, sent, sizeof(sent));
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    g_sink_reply.enabled.store(false);

    // Taking the routing away, and closing the Device, leave the board answering for itself.
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, nullptr, 0, kControlMs) == MIMHID_OK);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    target_control(0x80, 6, 0x0100, 0, 18);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 1, &std_get, 1, kControlMs) ==
                 MIMHID_OK);
    (void)node; // closed with the Device
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    device = open_device(base_options());
    if (device == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    target_control(0x80, 6, 0x0100, 0, 18);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        for (const auto& e : g_events)
            MIMHID_CHECK(e.kind != MIMHID_EVENT_CONTROL_SETUP);
    }
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The rules of a Node: each action end to end, their order, and changing them while the target is
// attached.
void test_control_rules() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    fake_control_result_t r{};
    EventRecord ev;
    const std::uint8_t answer_bytes[3] = {0xA1, 0xA2, 0xA3};

    // The Node's rules, in order. 0: ANSWER vendor request 0x50 (device to host). 1: the same
    // request 0x50 again, which never matches (the first wins). 2: ANSWER_LAST_SENT of endpoint
    // 0x81 for request 0x51. 3: the same for request 0x52 with the filter on the first byte 0xAA.
    // 4: ACCEPT of vendor request 0x40 (host to device). 5: STALL of request 0x53.
    mimhid_control_rule rules[6]{};
    for (auto& rule : rules) {
        rule.request_type_mask = 0xFF;
        rule.request_mask = 0xFF;
    }
    rules[0].request_type_value = 0xC0;
    rules[0].request_value = 0x50;
    rules[0].action = MIMHID_CONTROL_ANSWER;
    rules[0].data = answer_bytes;
    rules[0].length = sizeof(answer_bytes);
    rules[1] = rules[0];
    static const std::uint8_t other[1] = {0x11};
    rules[1].data = other;
    rules[1].length = 1;
    rules[2].request_type_value = 0xC0;
    rules[2].request_value = 0x51;
    rules[2].action = MIMHID_CONTROL_ANSWER_LAST_SENT;
    rules[2].endpoint_address = 0x81;
    rules[3] = rules[2];
    rules[3].request_value = 0x52;
    rules[3].first_byte_filter = 1;
    rules[3].first_byte = 0xAA;
    rules[4].request_type_value = 0x40;
    rules[4].request_value = 0x40;
    rules[4].action = MIMHID_CONTROL_ACCEPT;
    rules[5].request_type_value = 0xC0;
    rules[5].request_value = 0x53;
    rules[5].action = MIMHID_CONTROL_STALL;
    mimhid_node* node = open_vendor_node(device, rules, 6);
    if (node == nullptr)
        return;

    // ANSWER: the bytes, cut to wLength when the host asks for less (tud_control_xfer).
    target_control(0xC0, 0x50, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 3 &&
                 std::memcmp(r.data, answer_bytes, 3) == 0);
    target_control(0xC0, 0x50, 0, 0, 2);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2 &&
                 std::memcmp(r.data, answer_bytes, 2) == 0);

    // ANSWER_LAST_SENT: before any transfer the request is stalled (there is nothing to answer
    // with); after one, it answers with what was sent, whole; the filter lets only the reports
    // whose first byte matches count.
    fake_board_clear_transfers();
    target_control(0xC0, 0x51, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    const std::uint8_t first[3] = {0x01, 0x02, 0x03};
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, first, sizeof(first), kSendMs) == MIMHID_OK);
    fake_transfer_t t;
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 2000) && t.length == 3);
    target_control(0xC0, 0x51, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 3 &&
                 std::memcmp(r.data, first, 3) == 0);
    target_control(0xC0, 0x52, 0, 0, 8); // the filter wants 0xAA first: 0x01 is not that
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    const std::uint8_t second[2] = {0xAA, 0x09};
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, second, sizeof(second), kSendMs) ==
                 MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 2000) && t.length == 2);
    target_control(0xC0, 0x52, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2 &&
                 std::memcmp(r.data, second, 2) == 0);
    // The rule without a filter now answers with the newest transfer.
    target_control(0xC0, 0x51, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2 &&
                 std::memcmp(r.data, second, 2) == 0);

    // ACCEPT without a data stage: acknowledged, and the program hears of it with the ACCEPTED flag.
    target_control(0x40, 0x40, 7, 0, 0);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED);
    MIMHID_CHECK(wait_setup(&ev, 0x40, 0x40) && ev.flags == MIMHID_CONTROL_FLAG_ACCEPTED &&
                 ev.token != 0 && ev.data[2] == 7);
    // With a data stage: the data comes as CONTROL_DATA of the same token.
    const std::uint8_t out[4] = {1, 2, 3, 4};
    target_control(0x40, 0x40, 8, 0, sizeof(out), out, sizeof(out));
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED && r.length == 4);
    MIMHID_CHECK(wait_setup(&ev, 0x40, 0x40) && ev.flags == MIMHID_CONTROL_FLAG_ACCEPTED);
    EventRecord data_ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_CONTROL_DATA, &data_ev) && data_ev.token == ev.token &&
                 data_ev.data.size() == 4 && std::memcmp(data_ev.data.data(), out, 4) == 0);

    // STALL; and a request no rule matches goes to the stack, which stalls what it does not know.
    target_control(0xC0, 0x53, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    target_control(0xC0, 0x77, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);

    // The bytes of an ANSWER rule change at once, to as many as the rule was given at most.
    const std::uint8_t changed[2] = {0xB1, 0xB2};
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 0, changed, sizeof(changed), kControlMs) ==
                 MIMHID_OK);
    target_control(0xC0, 0x50, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2 &&
                 std::memcmp(r.data, changed, 2) == 0);
    const std::uint8_t longer[4] = {1, 2, 3, 4};
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 0, longer, sizeof(longer), kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 4, changed, sizeof(changed), kControlMs) ==
                 MIMHID_ERR_PARAM); // rule 4 is no ANSWER rule
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 9, changed, sizeof(changed), kControlMs) ==
                 MIMHID_ERR_PARAM);

    // The Device's rules come after the Node's: the first match wins, so a Device rule for a request
    // the Node answers is never reached; one for another request is. They change while attached.
    mimhid_control_rule device_rules[2]{};
    for (auto& rule : device_rules) {
        rule.request_type_mask = 0xFF;
        rule.request_mask = 0xFF;
        rule.request_type_value = 0xC0;
    }
    device_rules[0].request_value = 0x50;
    device_rules[0].action = MIMHID_CONTROL_STALL;
    device_rules[1].request_value = 0x60;
    device_rules[1].action = MIMHID_CONTROL_ANSWER;
    device_rules[1].data = answer_bytes;
    device_rules[1].length = sizeof(answer_bytes);
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, device_rules, 2, kControlMs) ==
                 MIMHID_OK);
    target_control(0xC0, 0x50, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2); // the Node's
    target_control(0xC0, 0x60, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 3 &&
                 std::memcmp(r.data, answer_bytes, 3) == 0);
    // The table was sent again with rule 0 shortened to two bytes; it keeps the capacity it was
    // given (three), so it grows again, and does not take a fourth byte.
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 0, longer, 3, kControlMs) == MIMHID_OK);
    target_control(0xC0, 0x50, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 3 &&
                 std::memcmp(r.data, longer, 3) == 0);
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 0, longer, 4, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_set_rule_data(node, 0, changed, sizeof(changed), kControlMs) ==
                 MIMHID_OK);
    // Live: new rules replace the Device's, the Node's stay.
    device_rules[1].request_value = 0x61;
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, device_rules, 2, kControlMs) ==
                 MIMHID_OK);
    target_control(0xC0, 0x60, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    target_control(0xC0, 0x61, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 3);
    target_control(0xC0, 0x50, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 2);
    // The bytes of a Device rule: number 1 of the Device's own.
    const std::uint8_t newer[1] = {0xC9};
    MIMHID_CHECK(mimhid_device_set_rule_data(device, 1, newer, sizeof(newer), kControlMs) ==
                 MIMHID_OK);
    target_control(0xC0, 0x61, 0, 0, 8);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 1 &&
                 r.data[0] == 0xC9);
    MIMHID_CHECK(mimhid_device_set_rule_data(device, 0, newer, sizeof(newer), kControlMs) ==
                 MIMHID_ERR_PARAM); // a STALL rule has no bytes

    // The limits: the answers together are at most the board's answer memory.
    mimhid_control_rule heavy{};
    heavy.request_type_mask = 0xFF;
    heavy.request_type_value = 0xC0;
    heavy.request_mask = 0xFF;
    heavy.request_value = 0x70;
    heavy.action = MIMHID_CONTROL_ANSWER;
    std::vector<std::uint8_t> big(4096 - sizeof(answer_bytes) + 1U, 0x5A);
    heavy.data = big.data();
    heavy.length = big.size();
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &heavy, 1, kControlMs) ==
                 MIMHID_ERR_OVERFLOW);
    big.pop_back(); // exactly what is left beside the Node's rule 0 (3 bytes)
    big.resize(4096 - sizeof(answer_bytes) - 1U); // (the Node's other answer has one byte)
    heavy.length = big.size();
    MIMHID_CHECK(mimhid_device_set_control_rules(device, 0, &heavy, 1, kControlMs) == MIMHID_OK);
    target_control(0xC0, 0x70, 0, 0, 5000);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED &&
                 r.length == big.size() && r.data[0] == 0x5A);

    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace sim
