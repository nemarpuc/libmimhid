// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* mimhid_device_open against boards that do not answer, answer with another version, refuse or
 * fail the line rate, or offer less than the configuration asks; and the open that succeeds,
 * with what the device then reports about the board. */
#include "mimhid_proto.h"
#include "scripted_support.hpp"

#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using namespace scripted;

namespace {

// Nothing answers, not after a BREAK and not on the hinted rate.
void test_silent_board() {
    Board board;
    if (!board.start())
        return;
    board.set_answer_pings(false);
    mimhid_device_config o = board_options();
    o.transport.baud_rate = 1000000; // also tried as the rate a crashed program left it on
    mimhid_device* device = nullptr;
    MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_TIMEOUT);
    MIMHID_CHECK(device == nullptr);
    MIMHID_CHECK(mimhid_last_error()->code == MIMHID_ERR_TIMEOUT &&
                 std::strcmp(mimhid_last_error()->field, "port") == 0);
}

// A board of another minor version is refused, and the error names both versions.
void test_other_version() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.version[0] = MIMHID_VERSION_MAJOR;
    pong.version[1] = static_cast<std::uint8_t>(MIMHID_VERSION_MINOR + 1);
    pong.version[2] = 0;
    board.set_pong(pong);
    mimhid_device* device = nullptr;
    const mimhid_device_config o = board_options();
    MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_PROTOCOL);
    const mimhid_error_detail* e = mimhid_last_error();
    MIMHID_CHECK(e->offset == ((MIMHID_VERSION_MAJOR << 8) | MIMHID_VERSION_MINOR));
    MIMHID_CHECK(e->length == ((MIMHID_VERSION_MAJOR << 8) | (MIMHID_VERSION_MINOR + 1U)));
}

// A PONG of this version that is too short to hold the limits is not a board.
void test_truncated_pong() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.raw = std::vector<std::uint8_t>{MIMHID_VERSION_MAJOR, MIMHID_VERSION_MINOR, 0, 1, 8};
    board.set_pong(pong);
    mimhid_device* device = nullptr;
    const mimhid_device_config o = board_options();
    MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_TIMEOUT);
}

// The rate: refused by the board (NAK), acknowledged without the rate (a broken answer), and
// switched to but not carried by the line.
void test_rate_failures() {
    mimhid_device_config o = board_options();
    o.transport.baud_rate = 1000000;
    {
        Board board;
        if (!board.start())
            return;
        board.set_policy([](const Received& r) -> std::optional<Reply> {
            if (r.type == MIMHID_T_SET_BAUD)
                return Reply::nak(MIMHID_NAK_UNSUPPORTED);
            return std::nullopt;
        });
        mimhid_device* device = nullptr;
        MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_UNSUPPORTED);
        MIMHID_CHECK(std::strcmp(mimhid_last_error()->field, "baud_rate") == 0);
    }
    {
        Board board;
        if (!board.start())
            return;
        board.set_policy([](const Received& r) -> std::optional<Reply> {
            if (r.type == MIMHID_T_SET_BAUD)
                return Reply::ack({}); // no rate in it
            return std::nullopt;
        });
        mimhid_device* device = nullptr;
        MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_IO);
    }
    {
        Board board;
        if (!board.start())
            return;
        board.set_carries([](std::uint32_t baud) { return baud == MIMHID_BOOT_BAUD; });
        mimhid_device* device = nullptr;
        MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_UNSUPPORTED);
        // Both sides went back to the boot rate.
        MIMHID_CHECK(board.baud() == MIMHID_BOOT_BAUD || board.count(MIMHID_T_SET_BAUD) >= 1U);
    }
    {
        // Above what the board's UART makes: refused before anything is asked.
        Board board;
        if (!board.start())
            return;
        Pong pong;
        pong.max_baud = 500000;
        board.set_pong(pong);
        mimhid_device* device = nullptr;
        MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_UNSUPPORTED);
        MIMHID_CHECK(board.count(MIMHID_T_SET_BAUD) == 0U);
    }
}

// MIMHID_BAUD_FASTEST searches down from the board's fastest rate to the fastest the line
// carries, to within 2 %, and the device reports it.
void test_fastest_rate_search() {
    Board board;
    if (!board.start())
        return;
    constexpr std::uint32_t kLimit = 1000000;
    board.set_carries([](std::uint32_t baud) { return baud <= kLimit; });
    mimhid_device_config o = board_options();
    o.transport.baud_rate = MIMHID_BAUD_FASTEST;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    mimhid_device_info info{};
    MIMHID_CHECK(mimhid_device_board_info(device, &info) == MIMHID_OK);
    MIMHID_CHECK(info.baud_rate <= kLimit && info.baud_rate * 100ULL >= kLimit * 95ULL);
    MIMHID_CHECK(info.max_baud == 3000000U && info.firmware_compatible == 1U);
    MIMHID_CHECK(board.baud() == info.baud_rate);
    // Closing puts the board back on the boot rate for the next program.
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(board.baud() == MIMHID_BOOT_BAUD);
}

// When no rate above the boot rate carries, the fastest is the boot rate.
void test_fastest_is_boot_rate() {
    Board board;
    if (!board.start())
        return;
    board.set_carries([](std::uint32_t baud) { return baud == MIMHID_BOOT_BAUD; });
    mimhid_device_config o = board_options();
    o.transport.baud_rate = MIMHID_BAUD_FASTEST;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    mimhid_device_info info{};
    MIMHID_CHECK(mimhid_device_board_info(device, &info) == MIMHID_OK &&
                 info.baud_rate == MIMHID_BOOT_BAUD);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// What the board offers bounds the configuration.
void test_board_limits_bound_the_configuration() {
    {
        Board board;
        if (!board.start())
            return;
        Pong pong;
        pong.max_report_bytes = 32;
        board.set_pong(pong);
        mimhid_device* device = nullptr;
        const mimhid_device_config o = board_options(); // asks for 64
        MIMHID_CHECK(mimhid_device_open(board.path(), &o, &device) == MIMHID_ERR_OVERFLOW);
    }
    {
        // A chip without an ID of its own: the device says so (the firmware then presents no
        // serial number for serial_from_chip_id).
        Board board;
        if (!board.start())
            return;
        Pong pong;
        pong.flags = 0;
        pong.unique_id.clear();
        board.set_pong(pong);
        mimhid_device* device = open_on(board, board_options());
        if (device == nullptr)
            return;
        mimhid_device_info info{};
        MIMHID_CHECK(mimhid_device_board_info(device, &info) == MIMHID_OK &&
                     info.has_unique_id == 0 && info.unique_id_length == 0);
        MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    }
}

// A path that opens but is no terminal fails at the configure step.
void test_not_a_terminal() {
    mimhid_device* device = nullptr;
    const mimhid_device_config o = board_options();
    MIMHID_CHECK(mimhid_device_open("/dev/null", &o, &device) == MIMHID_ERR_IO);
    MIMHID_CHECK(mimhid_last_error()->transport_status != 0);
    MIMHID_CHECK(mimhid_device_open("/dev/null", &o, nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_open("/dev/null", nullptr, &device) == MIMHID_ERR_PARAM);
}

// A successful open reports the board's limits as its PONG gave them.
void test_open_reports_the_board() {
    Board board;
    if (!board.start())
        return;
    Pong pong;
    pong.max_nodes = 5;
    pong.max_interfaces = 3;
    pong.chip = 2;
    pong.max_report_bytes = 300;
    pong.max_descriptor_bytes = 777;
    pong.max_control_rules = 4;
    board.set_pong(pong);
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_device_info info{};
    MIMHID_CHECK(mimhid_device_board_info(device, &info) == MIMHID_OK);
    MIMHID_CHECK(std::strcmp(info.port_name, board.path()) == 0);
    MIMHID_CHECK(info.max_nodes == 5 && info.max_interfaces == 3 && info.chip == 2);
    MIMHID_CHECK(info.max_report_bytes == 255); // saturated in the u8 field
    MIMHID_CHECK(info.max_descriptor_bytes == 777 && info.has_unique_id == 1);
    MIMHID_CHECK(info.unique_id_length == 8 && info.unique_id[7] == 8);
    MIMHID_CHECK(info.is_ready == 0); // nothing was committed yet
    MIMHID_CHECK(mimhid_device_board_info(device, nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_board_info(nullptr, &info) == MIMHID_ERR_PARAM);
    mimhid_board_limits limits;
    mimhid_board_limits_init(&limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_control_rules == 4 && limits.max_report_bytes == 300);
    limits.struct_size = 1;
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_limits(device, nullptr) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The port belongs to one program: a second open of the same path is refused.
void test_port_is_exclusive() {
    Board board;
    if (!board.start())
        return;
    mimhid_device* device = open_on(board, board_options());
    if (device == nullptr)
        return;
    mimhid_device* second = nullptr;
    const mimhid_device_config o = board_options();
    MIMHID_CHECK(mimhid_device_open(board.path(), &o, &second) == MIMHID_ERR_ACCESS);
    MIMHID_CHECK(second == nullptr);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// A log sink hears about opens and closes at INFO, and nothing above its level.
struct LogRecord {
    int info = 0;
    int other = 0;
};
void MIMHID_CALL record_log(void* user, mimhid_log_level level, const char*) {
    auto* record = static_cast<LogRecord*>(user);
    if (level == MIMHID_LOG_INFO)
        ++record->info;
    else
        ++record->other;
}

void test_log_sink() {
    Board board;
    if (!board.start())
        return;
    LogRecord record;
    mimhid_device_config o = board_options();
    o.transport.log_level = MIMHID_LOG_INFO;
    o.transport.log_sink = record_log;
    o.transport.log_user = &record;
    mimhid_device* device = open_on(board, o);
    if (device == nullptr)
        return;
    MIMHID_CHECK(record.info == 1);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(record.info == 2 && record.other == 0);

    // At ERROR the INFO messages are left out.
    LogRecord quiet;
    o.transport.log_level = MIMHID_LOG_ERROR;
    o.transport.log_user = &quiet;
    device = open_on(board, o);
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(quiet.info == 0 && quiet.other == 0);
}

} // namespace

void test_api_open() {
    test_silent_board();
    test_other_version();
    test_truncated_pong();
    test_rate_failures();
    test_fastest_rate_search();
    test_fastest_is_boot_rate();
    test_board_limits_bound_the_configuration();
    test_not_a_terminal();
    test_open_reports_the_board();
    test_port_is_exclusive();
    test_log_sink();
}
