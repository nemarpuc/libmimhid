// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The running link below the C ABI, against the scripted board: commands and their answers,
 * the limits of a frame, events handed to the handler, and a reader that sees the cable go. */
#include "mimusb_proto.h"
#include "scripted_support.hpp"
#include "transport/link.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace scripted;
namespace tr = mimusb::transport;

namespace {

void test_commands_and_events() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMUSB_CHECK(link.open(board.path()).ok);
    std::atomic<int> events{0};
    link.start([&](const tr::BoardEvent&) { ++events; });

    const std::uint8_t payload[2] = {7, 8};
    tr::CommandResult r = link.command(MIMUSB_T_RESET, payload, sizeof(payload), 200, 1);
    MIMUSB_CHECK(r.ok() && r.status == tr::CommandStatus::ack && r.data_length == 0);
    Received got;
    MIMUSB_CHECK(board.take(MIMUSB_T_RESET, &got, 0) && got.payload.size() == 3U &&
                 got.payload[1] == 7 && got.payload[2] == 8);

    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_RESET)
            return Reply::ack(std::vector<std::uint8_t>(20, 0xEE)); // more than the slot holds
        if (q.type == MIMUSB_T_EP_CONFIG)
            return Reply::nak(MIMUSB_NAK_OVERFLOW);
        if (q.type == MIMUSB_T_RULE_DATA)
            return Reply::silent();
        return std::nullopt;
    });
    r = link.command(MIMUSB_T_RESET, nullptr, 0, 200, 1);
    MIMUSB_CHECK(r.ok() && r.data_length == 16 && r.data[15] == 0xEE);
    r = link.command(MIMUSB_T_EP_CONFIG, payload, 2, 200, 1);
    MIMUSB_CHECK(r.status == tr::CommandStatus::nak && r.nak_code == MIMUSB_NAK_OVERFLOW && !r);
    r = link.command(MIMUSB_T_RULE_DATA, payload, 2, 20, 2);
    MIMUSB_CHECK(r.status == tr::CommandStatus::timeout);
    MIMUSB_CHECK(board.count(MIMUSB_T_RULE_DATA) == 2U);
    board.set_policy(nullptr);

    // A payload that cannot be framed is refused before anything is written.
    const std::vector<std::uint8_t> huge(MIMUSB_PROTO_MAX_PAYLOAD, 0);
    r = link.command(MIMUSB_T_RESET, huge.data(), huge.size(), 20, 1);
    MIMUSB_CHECK(r.status == tr::CommandStatus::io_error);
    std::int32_t os_error = 0;
    std::vector<std::uint8_t> too_long(MIMUSB_PROTO_MAX_PAYLOAD + 1U, 0);
    MIMUSB_CHECK(link.send_report(0, too_long.data(), too_long.size(), 20, &os_error) ==
                 tr::IoStatus::error);
    MIMUSB_CHECK(link.send_control_reply(1, 0, huge.data(), MIMUSB_PROTO_MAX_PAYLOAD - 2U, 20,
                                         &os_error) == tr::IoStatus::error);
    MIMUSB_CHECK(link.send_control_reply(1, 0, payload, 2, 20, &os_error) == tr::IoStatus::ok);
    MIMUSB_CHECK(board.take(MIMUSB_T_CONTROL_REPLY, &got) && got.payload.size() == 5U &&
                 got.payload[1] == 1 && got.payload[3] == 7);

    // Counters that do not decode are an I/O error.
    board.set_policy([](const Received& q) -> std::optional<Reply> {
        if (q.type == MIMUSB_T_COUNTERS)
            return Reply::ack({1});
        return std::nullopt;
    });
    tr::LinkCounters counters;
    MIMUSB_CHECK(link.counters(false, 200, &counters).status == tr::CommandStatus::io_error);
    board.set_policy(nullptr);
    MIMUSB_CHECK(link.counters(true, 200, &counters).ok() && counters.errors() == 0U);

    // Mount and unmount set ready; every event is handed on as it came (the library, not the link,
    // judges what an event carries).
    MIMUSB_CHECK(!link.ready());
    board.send_event(MIMUSB_EV_MOUNTED, {});
    MIMUSB_CHECK(link.wait_ready(500) && link.ready());
    board.send_event(MIMUSB_EV_UNMOUNTED, {});
    board.send_event(MIMUSB_EV_CONTROL_SETUP, {0, 0x10});
    board.send_event(MIMUSB_EV_SUSPEND, {0});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (events.load() < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MIMUSB_CHECK(events.load() == 4); // mounted, unmounted, the two others
    MIMUSB_CHECK(!link.ready());
    MIMUSB_CHECK(!link.wait_ready(10));
    link.close();
}

// The reader sees the hang-up: the link is failed, waits end at once, and writes are refused.
void test_reader_sees_the_hang_up() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMUSB_CHECK(link.open(board.path()).ok);
    link.start(nullptr);
    board.send_event(MIMUSB_EV_MOUNTED, {}); // no handler: nothing to call
    MIMUSB_CHECK(link.wait_ready(500));
    board.set_policy([](const Received&) -> std::optional<Reply> { return Reply::silent(); });
    std::atomic<bool> done{false};
    tr::CommandResult pending;
    std::thread waiter([&] {
        pending = link.command(MIMUSB_T_RESET, nullptr, 0, 2000, 3);
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto start = std::chrono::steady_clock::now();
    board.hang_up();
    waiter.join();
    MIMUSB_CHECK(done.load() && pending.status == tr::CommandStatus::closed);
    MIMUSB_CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    MIMUSB_CHECK(link.failed() && !link.ready());
    MIMUSB_CHECK(link.command(MIMUSB_T_RESET, nullptr, 0, 20, 1).status ==
                 tr::CommandStatus::closed);
    MIMUSB_CHECK(link.send_report(0, nullptr, 0, 20, nullptr) == tr::IoStatus::closed);
    MIMUSB_CHECK(link.send_control_reply(1, 0, nullptr, 0, 20, nullptr) == tr::IoStatus::closed);
    MIMUSB_CHECK(!link.wait_ready(10));
    link.close();
}

// A command on a port whose writes fail reports how they failed.
void test_command_write_failures() {
    Board board;
    if (!board.start())
        return;
    tr::Link link;
    MIMUSB_CHECK(link.open(board.path()).ok);
    link.start(nullptr);
    board.set_reading(false);
    // The board does not read, so no answer comes: each command times out, waiting for one or,
    // once the pseudo-terminal is full, in the write itself; one whose frame went out in part is
    // a short write. None hangs, and none is taken for an answer.
    std::vector<std::uint8_t> payload(1000, 0x55);
    tr::CommandStatus status = tr::CommandStatus::timeout;
    for (int i = 0; i < 300 && status == tr::CommandStatus::timeout; ++i)
        status = link.command(MIMUSB_T_RESET, payload.data(), payload.size(), 1, 1).status;
    MIMUSB_CHECK(status == tr::CommandStatus::short_write || status == tr::CommandStatus::timeout);
    board.set_reading(true);
    link.close();
}

} // namespace

void test_link() {
    test_commands_and_events();
    test_reader_sees_the_hang_up();
    test_command_write_failures();
}
