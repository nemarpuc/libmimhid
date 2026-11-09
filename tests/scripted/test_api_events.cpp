// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Board events as the event sink sees them: each kind with the node it belongs to, Output reports
 * matched to a node by Report ID, and events too short to carry their data, which are dropped. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"
#include "transport/frame.hpp"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace scripted;

namespace {

struct Seen {
    mimhid_event_kind kind;
    std::uint16_t node_id;
    std::uint16_t report_id;
    std::uint32_t value;
    std::vector<std::uint8_t> data;
    std::uint32_t token;
    std::uint32_t flags;
};

std::mutex g_mutex;
std::vector<Seen> g_seen;

void MIMHID_CALL record(void*, const mimhid_event* e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_seen.push_back({e->kind, e->node_id, e->report_id, e->value,
                      std::vector<std::uint8_t>(e->data, e->data + e->length), e->control_token,
                      e->control_flags});
}

// A sink that throws must not take the reader thread down.
void MIMHID_CALL throwing(void*, const mimhid_event*) { throw 1; }

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

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_seen.clear();
}

// A raw node on interface 0 with Output reports under Report IDs 2 and 3.
constexpr std::uint8_t kTwoIds[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x15,
                                    0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x02,
                                    0x91, 0x02, 0x85, 0x03, 0x09, 0x03, 0x91, 0x02, 0xC0};

void test_event_kinds() {
    Board board;
    if (!board.start())
        return;
    mimhid_device_config o = board_options();
    o.transport.event_sink = record;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    const mimhid_node_config config = node_config();
    mimhid_node* raw = nullptr;
    MIMHID_CHECK(mimhid_node_open_raw(device, kTwoIds, sizeof(kTwoIds), &config, &kCommit, &raw) ==
                 MIMHID_OK);
    Seen s{};
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_MOUNTED);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 1);
    clear();

    // Events too short for their data, and unknown kinds, never reach the sink.
    board.send_event(MIMHID_EV_SET_REPORT, {0, 2});
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 1});
    board.send_event(MIMHID_EV_SET_PROTOCOL, {0});
    board.send_event(MIMHID_EV_SET_IDLE, {0});
    board.send_event(MIMHID_EV_CONTROL_SETUP, {1, 1, 0, 0, 0, 0, 0, 0, 0});
    board.send_event(MIMHID_EV_CONTROL_DATA, {});
    board.send_event(MIMHID_EV_REPORT_LOST, {0, 1});
    board.send_event(0x7E, {1, 2, 3});
    const std::uint8_t empty_event[1] = {0};
    board.send_frame(MIMHID_T_EVENT, empty_event, 0); // no kind at all
    MIMHID_CHECK(!next(&s, 50));

    // An Output report through the endpoint names its Report ID in its first byte.
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 2, 0, 3, 0x5A});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 3 &&
                 s.node_id == mimhid_node_id(raw) && s.data.size() == 1U && s.data[0] == 0x5A);
    // A first byte that is no Report ID of the interface: the data stays whole.
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 2, 0, 9, 0x5A});
    MIMHID_CHECK(next(&s) && s.report_id == 0 && s.data.size() == 2U && s.data[0] == 9);
    // The length field bounds the data even when the frame carries more.
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 1, 0, 2, 0x11, 0x22});
    MIMHID_CHECK(next(&s) && s.report_id == 2 && s.data.empty());
    // SET_REPORT names type and Report ID; type 3 is a Feature report.
    board.send_event(MIMHID_EV_SET_REPORT, {0, 3, 2, 0x77});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_FEATURE_REPORT && s.report_id == 2 &&
                 s.data.size() == 1U && s.node_id == mimhid_node_id(raw));
    board.send_event(MIMHID_EV_SET_REPORT, {0, 2, 3, 0x01});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 3);
    // An interface no node has: no node id.
    board.send_event(MIMHID_EV_SET_REPORT, {5, 2, 3, 0x01});
    MIMHID_CHECK(next(&s) && s.node_id == 0);
    board.send_event(MIMHID_EV_SET_PROTOCOL, {0, 0});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SET_PROTOCOL && s.value == 0);
    board.send_event(MIMHID_EV_SET_IDLE, {0, 4});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SET_IDLE && s.value == 4);
    board.send_event(MIMHID_EV_REPORT_LOST, {0, 0x34, 0x12});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_REPORT_LOST && s.value == 0x1234);
    board.send_event(MIMHID_EV_CONTROL_SETUP, {7, 1, 0xC0, 1, 0, 0, 0, 0, 4, 0});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_CONTROL_SETUP && s.token == 7 && s.flags == 1 &&
                 s.data.size() == 8U && s.data[0] == 0xC0);
    board.send_event(MIMHID_EV_CONTROL_DATA, {7, 0xAB});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_CONTROL_DATA && s.token == 7 &&
                 s.data.size() == 1U);
    board.send_event(MIMHID_EV_SUSPEND, {1});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SUSPEND && s.value == 1);
    board.send_event(MIMHID_EV_SUSPEND, {});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SUSPEND && s.value == 0);
    board.send_event(MIMHID_EV_RESUME, {});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_RESUME);
    board.send_event(MIMHID_EV_UNMOUNTED, {});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_UNMOUNTED);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 0);

    // A frame split across two writes, with a pause shorter than the parser's idle time, is one
    // frame; garbage before it is skipped.
    std::vector<std::uint8_t> frame(16);
    const std::uint8_t mounted[1] = {MIMHID_EV_MOUNTED};
    frame.resize(mimhid::transport::encode_frame(MIMHID_T_EVENT, mounted, 1, frame.data()));
    const std::uint8_t junk[3] = {0x00, 0xFF, 0x13};
    board.send_bytes(junk, sizeof(junk));
    board.send_bytes(frame.data(), 2);
    board.send_bytes(frame.data() + 2, frame.size() - 2);
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_MOUNTED);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

void test_throwing_sink() {
    Board board;
    if (!board.start())
        return;
    mimhid_device_config o = board_options();
    o.transport.event_sink = throwing;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    board.send_event(MIMHID_EV_RESUME, {});
    board.send_event(MIMHID_EV_MOUNTED, {});
    MIMHID_CHECK(mimhid_device_wait_ready(device, 1000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_api_events() {
    test_event_kinds();
    test_throwing_sink();
}
