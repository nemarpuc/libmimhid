// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The running link below the C ABI, against the scripted board: commands and their answers,
 * the limits of a frame, events handed to the handler, and a reader that sees the cable go. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"
#include "transport/link.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace scripted;
namespace tr = mimhid::transport;

namespace {

void test_commands_and_events() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMHID_CHECK(link.open(board.path()).ok);
    std::atomic<int> events{0};
    link.start([&](const tr::BoardEvent&) { ++events; });

    const std::uint8_t payload[2] = {7, 8};
    tr::CommandResult r = link.command(MIMHID_T_RESET, payload, sizeof(payload), 200, 1);
    MIMHID_CHECK(r.ok() && r.status == tr::CommandStatus::ack && r.data_length == 0);
    Received got;
    MIMHID_CHECK(board.take(MIMHID_T_RESET, &got, 0) && got.payload.size() == 3U &&
                 got.payload[1] == 7 && got.payload[2] == 8);

    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_RESET)
            return Reply::ack(std::vector<std::uint8_t>(20, 0xEE)); // more than the slot holds
        if (q.type == MIMHID_T_QUEUE_DEPTH)
            return Reply::nak(MIMHID_NAK_OVERFLOW);
        if (q.type == MIMHID_T_FEATURE)
            return Reply::silent();
        return std::nullopt;
    });
    r = link.command(MIMHID_T_RESET, nullptr, 0, 200, 1);
    MIMHID_CHECK(r.ok() && r.data_length == 16 && r.data[15] == 0xEE);
    r = link.command(MIMHID_T_QUEUE_DEPTH, payload, 2, 200, 1);
    MIMHID_CHECK(r.status == tr::CommandStatus::nak && r.nak_code == MIMHID_NAK_OVERFLOW && !r);
    r = link.command(MIMHID_T_FEATURE, payload, 2, 20, 2);
    MIMHID_CHECK(r.status == tr::CommandStatus::timeout);
    MIMHID_CHECK(board.count(MIMHID_T_FEATURE) == 2U);
    board.set_policy(nullptr);

    // A payload that cannot be framed is refused before anything is written.
    const std::vector<std::uint8_t> huge(MIMHID_PROTO_MAX_PAYLOAD, 0);
    r = link.command(MIMHID_T_RESET, huge.data(), huge.size(), 20, 1);
    MIMHID_CHECK(r.status == tr::CommandStatus::io_error);
    std::int32_t os_error = 0;
    std::vector<std::uint8_t> too_long(MIMHID_PROTO_MAX_PAYLOAD + 1U, 0);
    MIMHID_CHECK(link.send_report(0, too_long.data(), too_long.size(), 20, &os_error) ==
                 tr::IoStatus::error);
    MIMHID_CHECK(link.send_control_reply(1, 0, huge.data(), MIMHID_PROTO_MAX_PAYLOAD - 2U, 20,
                                         &os_error) == tr::IoStatus::error);
    MIMHID_CHECK(link.send_control_reply(1, 0, payload, 2, 20, &os_error) == tr::IoStatus::ok);
    MIMHID_CHECK(board.take(MIMHID_T_CONTROL_REPLY, &got) && got.payload.size() == 5U &&
                 got.payload[1] == 1 && got.payload[3] == 7);

    // Counters that do not decode are an I/O error.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMHID_T_COUNTERS)
            return Reply::ack({1});
        return std::nullopt;
    });
    tr::LinkCounters counters;
    MIMHID_CHECK(link.counters(false, 200, &counters).status == tr::CommandStatus::io_error);
    board.set_policy(nullptr);
    MIMHID_CHECK(link.counters(true, 200, &counters).ok() && counters.errors() == 0U);

    // Mount and unmount set ready; Output and SET_REPORT events need three bytes to be handed on.
    MIMHID_CHECK(!link.ready());
    board.send_event(MIMHID_EV_MOUNTED, {});
    MIMHID_CHECK(link.wait_ready(500) && link.ready());
    board.send_event(MIMHID_EV_UNMOUNTED, {});
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 1});
    board.send_event(MIMHID_EV_SET_REPORT, {0});
    board.send_event(MIMHID_EV_OUTPUT_REPORT, {0, 1, 0, 9});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (events.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MIMHID_CHECK(events.load() == 3); // mounted, unmounted, the full Output report
    MIMHID_CHECK(!link.ready());
    MIMHID_CHECK(!link.wait_ready(10));
    link.close();
}

// The reader sees the hang-up: the link is failed, waits end at once, and writes are refused.
void test_reader_sees_the_hang_up() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMHID_CHECK(link.open(board.path()).ok);
    link.start(nullptr);
    board.send_event(MIMHID_EV_MOUNTED, {}); // no handler: nothing to call
    MIMHID_CHECK(link.wait_ready(500));
    board.set_policy([](const Received&) -> std::optional<Reply> { return Reply::silent(); });
    std::atomic<bool> done{false};
    tr::CommandResult pending;
    std::thread waiter([&] {
        pending = link.command(MIMHID_T_RESET, nullptr, 0, 2000, 3);
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto start = std::chrono::steady_clock::now();
    board.hang_up();
    waiter.join();
    MIMHID_CHECK(done.load() && pending.status == tr::CommandStatus::closed);
    MIMHID_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    MIMHID_CHECK(link.failed() && !link.ready());
    MIMHID_CHECK(link.command(MIMHID_T_RESET, nullptr, 0, 20, 1).status ==
                 tr::CommandStatus::closed);
    MIMHID_CHECK(link.send_report(0, nullptr, 0, 20, nullptr) == tr::IoStatus::closed);
    MIMHID_CHECK(link.send_control_reply(1, 0, nullptr, 0, 20, nullptr) == tr::IoStatus::closed);
    MIMHID_CHECK(!link.wait_ready(10));
    link.close();
}

// A command on a port whose writes fail reports how they failed.
void test_command_write_failures() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMHID_CHECK(link.open(board.path()).ok);
    link.start(nullptr);
    board.set_reading(false);
    // The board does not read, so no answer comes: each command times out, waiting for one or,
    // once the pseudo-terminal is full, in the write itself; one whose frame went out in part is
    // a short write. None hangs, and none is taken for an answer.
    std::vector<std::uint8_t> payload(1000, 0x55);
    tr::CommandStatus status = tr::CommandStatus::timeout;
    for (int i = 0; i < 300 && status == tr::CommandStatus::timeout; ++i)
        status = link.command(MIMHID_T_RESET, payload.data(), payload.size(), 1, 1).status;
    MIMHID_CHECK(status == tr::CommandStatus::short_write || status == tr::CommandStatus::timeout);
    board.set_reading(true);
    link.close();
}

} // namespace

void test_link() {
    test_commands_and_events();
    test_reader_sees_the_hang_up();
    test_command_write_failures();
}
