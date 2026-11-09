// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Finding a board and agreeing a rate, below the C ABI, with the scripted board's line check on:
 * bytes pass only while both ends run at the same rate. A board left on a fast rate, found by a
 * BREAK or by the hinted rate; a board of another version; each way a rate can fail; and the
 * searches that give up when the board stops answering. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"
#include "transport/handshaker.hpp"

#include <atomic>

using namespace scripted;
namespace tr = mimhid::transport;

namespace {

std::atomic<Board*> g_board{nullptr};
void board_takes_break() {
    if (Board* board = g_board.load())
        board->take_break();
}

struct BreakReaches {
    explicit BreakReaches(Board& board) {
        g_board.store(&board);
        tr::set_break_observer(board_takes_break);
    }
    ~BreakReaches() {
        tr::set_break_observer(nullptr);
        g_board.store(nullptr);
    }
    BreakReaches(const BreakReaches&) = delete;
    BreakReaches& operator=(const BreakReaches&) = delete;
};

void test_find() {
    Board board;
    if (!board.start())
        return;
    board.set_line_check(true);
    tr::SerialPort port;
    MIMHID_CHECK(port.open(board.path(), MIMHID_BOOT_BAUD).ok);
    tr::Handshaker handshake(port, 30);
    tr::BoardInfo info;
    MIMHID_CHECK(handshake.find(&info) && info.compatible && handshake.rate() == MIMHID_BOOT_BAUD);
    MIMHID_CHECK(handshake.to_boot(&info)); // already there: a ping

    // Left on 1,000,000 by a crashed program, behind an adapter whose BREAK does not arrive:
    // only the hint finds it, and both end on the boot rate.
    board.set_baud(1000000);
    MIMHID_CHECK(!handshake.find(&info));
    MIMHID_CHECK(!handshake.find(&info, 500000)); // a wrong hint
    MIMHID_CHECK(handshake.find(&info, 1000000));
    MIMHID_CHECK(board.baud() == MIMHID_BOOT_BAUD && handshake.rate() == MIMHID_BOOT_BAUD);

    // With a BREAK that arrives, no hint is needed.
    board.set_baud(1000000);
    {
        BreakReaches reaches(board);
        MIMHID_CHECK(handshake.find(&info));
    }
    MIMHID_CHECK(board.baud() == MIMHID_BOOT_BAUD);

    // A board of another version answering at the hinted rate is left there, described.
    Pong other;
    other.version[0] = MIMHID_VERSION_MAJOR;
    other.version[1] = static_cast<std::uint8_t>(MIMHID_VERSION_MINOR + 1);
    board.set_pong(other);
    board.set_baud(1000000);
    MIMHID_CHECK(handshake.find(&info, 1000000) && !info.compatible &&
                 info.version[1] == MIMHID_VERSION_MINOR + 1);
    MIMHID_CHECK(handshake.rate() == 1000000 && board.baud() == 1000000);
}

void test_switch_to() {
    Board board;
    if (!board.start())
        return;
    board.set_line_check(true);
    tr::SerialPort port;
    MIMHID_CHECK(port.open(board.path(), MIMHID_BOOT_BAUD).ok);
    tr::Handshaker handshake(port, 30);
    tr::BoardInfo info;
    MIMHID_CHECK(handshake.find(&info));
    MIMHID_CHECK(handshake.switch_to(0, &info) == tr::Handshake::rate_refused);
    MIMHID_CHECK(handshake.switch_to(info.max_baud + 1U, &info) == tr::Handshake::rate_refused);
    MIMHID_CHECK(handshake.switch_to(MIMHID_BOOT_BAUD, &info) == tr::Handshake::ok);
    MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::ok);
    MIMHID_CHECK(handshake.rate() == 921600 && board.baud() == 921600 && info.baud == 921600);
    MIMHID_CHECK(handshake.to_boot(&info) && board.baud() == MIMHID_BOOT_BAUD);

    // The line does not carry the rate: both go back (the BREAK reaches the board).
    board.set_carries([](std::uint32_t baud) { return baud == MIMHID_BOOT_BAUD; });
    {
        BreakReaches reaches(board);
        MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::rate_failed);
    }
    MIMHID_CHECK(handshake.rate() == MIMHID_BOOT_BAUD && board.baud() == MIMHID_BOOT_BAUD);
    // ... and when the BREAK does not reach it, nothing answers any more.
    MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::io_error);
    board.set_baud(MIMHID_BOOT_BAUD);
    board.set_carries(nullptr);

    // The board refuses, or acknowledges without the rate.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_SET_BAUD)
            return Reply::nak(MIMHID_NAK_PARAM);
        return std::nullopt;
    });
    MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::rate_refused);
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_SET_BAUD)
            return Reply::ack({0x00});
        return std::nullopt;
    });
    MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::io_error);
    // The counters are refused: the test cannot run, and the rate failed.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_COUNTERS)
            return Reply::nak(MIMHID_NAK_UNSUPPORTED);
        return std::nullopt;
    });
    {
        BreakReaches reaches(board);
        MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::rate_failed);
    }
    board.set_policy(nullptr);
    // A board whose frames hold no probe cannot be tested.
    Pong tiny;
    tiny.max_frame_payload = 0;
    board.set_pong(tiny);
    MIMHID_CHECK(handshake.find(&info));
    {
        BreakReaches reaches(board);
        MIMHID_CHECK(handshake.switch_to(921600, &info) == tr::Handshake::rate_failed);
    }
}

void test_fastest() {
    Board board;
    if (!board.start())
        return;
    board.set_line_check(true);
    tr::SerialPort port;
    MIMHID_CHECK(port.open(board.path(), MIMHID_BOOT_BAUD).ok);
    tr::Handshaker handshake(port, 30);
    tr::BoardInfo info;
    MIMHID_CHECK(handshake.find(&info));
    BreakReaches reaches(board);
    // Everything up to the board's own maximum carries: the first try wins.
    MIMHID_CHECK(handshake.fastest(&info) == 3000000U && board.baud() == 3000000U);
    MIMHID_CHECK(handshake.to_boot(&info));
    // The board stops answering SET_BAUD: the search gives up with 0.
    board.set_policy([](const Received& r) -> std::optional<Reply> {
        if (r.type == MIMHID_T_SET_BAUD)
            return Reply::silent();
        return std::nullopt;
    });
    MIMHID_CHECK(handshake.fastest(&info) == 0U);
    board.set_policy(nullptr);
    // A board that cannot go above the boot rate.
    Pong slow;
    slow.max_baud = MIMHID_BOOT_BAUD;
    board.set_pong(slow);
    MIMHID_CHECK(handshake.find(&info) && handshake.fastest(&info) == MIMHID_BOOT_BAUD);
}

} // namespace

void test_handshaker() {
    test_find();
    test_switch_to();
    test_fastest();
}
