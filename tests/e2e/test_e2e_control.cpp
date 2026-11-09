// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Control transfers from the target: routing rules, held transfers and the replies to them. */
#include "e2e_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace e2e {

namespace {

mimhid_control_rule control_rule(std::uint8_t bm, std::uint8_t request, std::uint32_t timeout_ms,
                                 mimhid_control_action when_late) {
    mimhid_control_rule r{};
    r.request_type_mask = 0xFF;
    r.request_type_value = bm;
    r.request_mask = 0xFF;
    r.request_value = request;
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
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_control_rules == 8 && limits.max_control_data_bytes == 1024);

    // Arguments.
    const mimhid_control_rule good = control_rule(0xC0, 0x42, 100, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(nullptr, 0, &good, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 2, &good, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, nullptr, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    mimhid_control_rule many[MIMHID_CONTROL_MAX_RULES + 1];
    for (auto& r : many)
        r = good;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, many, MIMHID_CONTROL_MAX_RULES + 1,
                                                   kControlMs) == MIMHID_ERR_PARAM);
    // USB 2.0 9.2.6.1: 5 seconds is the most any command may take.
    MIMHID_CHECK(MIMHID_CONTROL_MAX_TIMEOUT_MS == 5000);
    mimhid_control_rule longest = good;
    longest.timeout_ms = 5000;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &longest, 1, kControlMs) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &good, 1, kControlMs) == MIMHID_OK);
    mimhid_control_rule bad = good;
    bad.timeout_ms = 0;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = 0;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = 5;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    bad = good;
    bad.timeout_ms = MIMHID_CONTROL_MAX_TIMEOUT_MS + 1;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1, kControlMs) ==
                 MIMHID_ERR_PARAM);
    bad = good;
    bad.value_value = 1; // outside its mask of zero
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1, kControlMs) ==
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
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 1, nullptr, 0, kControlMs) == MIMHID_OK);
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
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &vendor_in, 1, kControlMs) ==
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
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &quick, 1, kControlMs) == MIMHID_OK);
    const auto start = std::chrono::steady_clock::now();
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    const auto waited = std::chrono::steady_clock::now() - start;
    MIMHID_CHECK(waited >= std::chrono::milliseconds(55) &&
                 waited < std::chrono::milliseconds(1000));

    // A standard request answered after its time by an empty data stage, and one left to the stack.
    const mimhid_control_rule std_get = control_rule(0x80, 6, 40, MIMHID_CONTROL_ACTION_ANSWER);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &std_get, 1, kControlMs) ==
                 MIMHID_OK);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 0 &&
                 r.setup[0] == 0xC0 && r.setup[1] == 6); // retyped
    const mimhid_control_rule std_leave = control_rule(0x80, 6, 40, MIMHID_CONTROL_ACTION_LEAVE);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &std_leave, 1, kControlMs) ==
                 MIMHID_OK);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT && r.setup[0] == 0x80);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6) && ev.flags == MIMHID_CONTROL_FLAG_HELD);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_LEAVE, nullptr,
                                             0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT && r.setup[0] == 0x80);

    // A request without a data stage: ANSWER is an acknowledgement.
    const mimhid_control_rule no_data = control_rule(0x00, 3, 500, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &no_data, 1, kControlMs) ==
                 MIMHID_OK);
    target_control(0x00, 3, 1, 0, 0);
    MIMHID_CHECK(wait_setup(&ev, 0x00, 3));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER,
                                             nullptr, 0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED);

    // Host to device with a data stage: the program sees the data and gives the verdict.
    const mimhid_control_rule set_report = control_rule(0x21, 9, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &set_report, 1, kControlMs) ==
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
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &slow, 1, kControlMs) == MIMHID_OK);
    g_sink_reply.action = MIMHID_CONTROL_ACTION_ANSWER;
    g_sink_reply.data.clear();
    g_sink_reply.enabled.store(true);
    target_control(0x21, 9, 0x0302, 0, 3, sent, sizeof(sent));
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    g_sink_reply.enabled.store(false);

    // Taking the routing away, and closing the Device, leave the board answering for itself.
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, nullptr, 0, kControlMs) == MIMHID_OK);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    target_control(0x80, 6, 0x0100, 0, 18);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 1, &std_get, 1, kControlMs) ==
                 MIMHID_OK);
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

} // namespace e2e
