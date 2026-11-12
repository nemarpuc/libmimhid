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

// A raw Node on interface 0 with Output reports under Report IDs 2 and 3.
// (An Input report under ID 2 too: a raw Spec needs the table of the Input reports it sends.)
constexpr std::uint8_t kTwoIds[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x15,
                                    0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x01,
                                    0x81, 0x02, 0x09, 0x02, 0x91, 0x02, 0x85, 0x03, 0x09, 0x03,
                                    0x91, 0x02, 0xC0};

// The setup packet of a HID class request to interface `itf` (HID 1.11 7.2): SET_REPORT 0x09,
// SET_IDLE 0x0A, SET_PROTOCOL 0x0B, host to device, class, interface.
std::vector<std::uint8_t> class_setup(const std::uint8_t request, const std::uint16_t value,
                                      const std::uint8_t itf, const std::uint16_t length) {
    return {0x21, request, static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>(value >> 8U), itf, 0,
            static_cast<std::uint8_t>(length & 0xFFU), static_cast<std::uint8_t>(length >> 8U)};
}

// CONTROL_SETUP of a transfer an ACCEPT rule took: token, flags (ACCEPTED, 0x10), the setup packet.
void send_accepted(Board& board, const std::uint8_t token, const std::vector<std::uint8_t>& setup) {
    std::vector<std::uint8_t> payload{MIMHID_EV_CONTROL_SETUP, token, MIMHID_CONTROL_F_ACCEPTED};
    payload.insert(payload.end(), setup.begin(), setup.end());
    board.send_frame(MIMHID_T_EVENT, payload.data(), payload.size());
}

void test_event_kinds() {
    Board board;
    if (!board.start())
        return;
    mimhid_device_config o = board_options();
    o.transport.event_sink = record;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    mimhid_hid_node_config config = node_config();
    config.ep_out.address = 0x01;
    config.ep_out.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    config.ep_out.interval = 1;
    config.ep_out.max_packet_size = 64;
    mimhid_raw_options options;
    mimhid_raw_options_init(&options);
    options.descriptor = kTwoIds;
    options.descriptor_length = sizeof(kTwoIds);
    options.requires_output = 1;
    mimhid_raw_report input_report{};
    input_report.report_id.enabled = 1;
    input_report.report_id.value = 2;
    input_report.wire_length = 2;
    options.reports = &input_report;
    options.report_count = 1;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_raw(&options, &spec) == MIMHID_OK);
    mimhid_node* raw = nullptr;
    MIMHID_CHECK(mimhid_node_open_profile(device, spec, &config, &kCommit, &raw) == MIMHID_OK);
    Seen s{};
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_MOUNTED);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 1);
    clear();

    // Events too short for their data, and unknown kinds, never reach the sink.
    board.send_event(MIMHID_EV_CONTROL_SETUP, {1, 1, 0, 0, 0, 0, 0, 0, 0});
    board.send_event(MIMHID_EV_CONTROL_DATA, {});
    board.send_event(MIMHID_EV_TRANSFER_LOST, {0x81, 1});
    board.send_event(0x7E, {1, 2, 3});
    const std::uint8_t empty_event[1] = {0};
    board.send_frame(MIMHID_T_EVENT, empty_event, 0); // no kind at all
    MIMHID_CHECK(!next(&s, 50));

    // An Output report on the OUT endpoint names its Report ID in its first byte.
    const std::uint8_t out_frame = static_cast<std::uint8_t>(MIMHID_T_EP_OUT_BASE | 1);
    const std::uint8_t with_id[2] = {3, 0x5A};
    board.send_frame(out_frame, with_id, sizeof(with_id));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 3 &&
                 s.node_id == mimhid_node_id(raw) && s.data.size() == 1U && s.data[0] == 0x5A);
    // A first byte that is no Report ID of the interface: the data stays whole.
    const std::uint8_t no_id[2] = {9, 0x5A};
    board.send_frame(out_frame, no_id, sizeof(no_id));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 0 &&
                 s.data.size() == 2U && s.data[0] == 9);
    // A zero-length packet is an Output report of no bytes.
    board.send_frame(out_frame, nullptr, 0);
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.data.empty());
    // SET_REPORT is taken by the library's ACCEPT rule; its type and Report ID are in wValue and the
    // data stage follows as CONTROL_DATA of the same token (HID 1.11 7.2.2). Type 3 is a Feature
    // report, type 2 an Output report; the Report ID the data starts with is not repeated.
    send_accepted(board, 5, class_setup(0x09, 0x0302, 0, 2));
    MIMHID_CHECK(!next(&s, 30)); // nothing until the data stage
    board.send_event(MIMHID_EV_CONTROL_DATA, {5, 2, 0x77});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_FEATURE_REPORT && s.report_id == 2 &&
                 s.data.size() == 1U && s.data[0] == 0x77 && s.node_id == mimhid_node_id(raw));
    send_accepted(board, 6, class_setup(0x09, 0x0203, 0, 2));
    board.send_event(MIMHID_EV_CONTROL_DATA, {6, 3, 0x01});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 3 &&
                 s.data.size() == 1U && s.data[0] == 0x01);
    // Without a data stage (wLength 0) the report is empty and comes at once.
    send_accepted(board, 7, class_setup(0x09, 0x0203, 0, 0));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_OUTPUT_REPORT && s.report_id == 3 &&
                 s.data.empty());
    // A data stage whose token the library does not know is an ordinary CONTROL_DATA event.
    board.send_event(MIMHID_EV_CONTROL_DATA, {99, 0x01});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_CONTROL_DATA && s.token == 99);
    // An interface no Node has: no rule of the library matches, so the program hears of a taken
    // control transfer (flag ACCEPTED), not of a report.
    send_accepted(board, 8, class_setup(0x09, 0x0203, 5, 0));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_CONTROL_SETUP &&
                 (s.flags & MIMHID_CONTROL_FLAG_ACCEPTED) != 0U && s.token == 8);
    // SET_PROTOCOL and SET_IDLE: the value is in wValue, and the library keeps the answers of GET_PROTOCOL
    // and GET_IDLE (HID 1.11 7.2.3 to 7.2.6): it tells the board with RULE_DATA, which comes after the
    // event (the reader thread does not wait for the board).
    board.clear();
    send_accepted(board, 9, class_setup(0x0B, 0, 0, 0));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SET_PROTOCOL && s.value == 0 &&
                 s.node_id == mimhid_node_id(raw));
    Received update;
    MIMHID_CHECK(board.take(MIMHID_T_RULE_DATA, &update) && update.payload.size() == 3U &&
                 update.payload[2] == 0U);
    send_accepted(board, 10, class_setup(0x0A, 4U << 8U, 0, 0));
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_SET_IDLE && s.value == 4);
    MIMHID_CHECK(board.take(MIMHID_T_RULE_DATA, &update) && update.payload.size() == 3U &&
                 update.payload[2] == 4U);
    board.send_event(MIMHID_EV_TRANSFER_LOST, {0x81, 0x34, 0x12});
    // The board names the endpoint; the event names the Node that sends on it (0 for none).
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_TRANSFER_LOST && s.value == 0x1234 &&
                 s.node_id == mimhid_node_id(raw));
    board.send_event(MIMHID_EV_TRANSFER_LOST, {0x85, 0x01, 0x00});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_TRANSFER_LOST && s.value == 1 && s.node_id == 0);
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
    // The target configures the device again: the idle rate and the protocol are back at their
    // first values (HID 1.11 7.2.4, 7.2.6), and the board is told.
    board.clear();
    board.send_event(MIMHID_EV_MOUNTED, {});
    MIMHID_CHECK(next(&s) && s.kind == MIMHID_EVENT_MOUNTED);
    int resets = 0;
    while (resets < 2 && board.take(MIMHID_T_RULE_DATA, &update, 500))
        ++resets;
    MIMHID_CHECK(resets == 2);
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
    mimhid_spec_release(spec);
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
