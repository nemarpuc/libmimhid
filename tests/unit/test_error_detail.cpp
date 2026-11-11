// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The diagnostics every call leaves behind: the stable names of results and frame types (and what
 * an unknown value gives), the thread-local last error, the version, the log filter and the
 * structure-size check. */
#include "api/internal.hpp"
#include "mimhid.h"
#include "mimhid_proto.h"
#include "test.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool named(const char* name, const char* expected) {
    return name != nullptr && std::strcmp(name, expected) == 0 &&
           mimhid_last_error()->code == MIMHID_OK;
}

void test_result_names() {
    const struct {
        mimhid_result result;
        const char* name;
    } names[] = {
        {MIMHID_OK, "MIMHID_OK"},
        {MIMHID_ERR_PARAM, "MIMHID_ERR_PARAM"},
        {MIMHID_ERR_UNSET_FIELD, "MIMHID_ERR_UNSET_FIELD"},
        {MIMHID_ERR_UNSUPPORTED, "MIMHID_ERR_UNSUPPORTED"},
        {MIMHID_ERR_PROTOCOL, "MIMHID_ERR_PROTOCOL"},
        {MIMHID_ERR_ACCESS, "MIMHID_ERR_ACCESS"},
        {MIMHID_ERR_BUSY, "MIMHID_ERR_BUSY"},
        {MIMHID_ERR_NO_DEVICE, "MIMHID_ERR_NO_DEVICE"},
        {MIMHID_ERR_TIMEOUT, "MIMHID_ERR_TIMEOUT"},
        {MIMHID_ERR_SHORT_TRANSFER, "MIMHID_ERR_SHORT_TRANSFER"},
        {MIMHID_ERR_DESCRIPTOR_REJECTED, "MIMHID_ERR_DESCRIPTOR_REJECTED"},
        {MIMHID_ERR_IO, "MIMHID_ERR_IO"},
        {MIMHID_ERR_OVERFLOW, "MIMHID_ERR_OVERFLOW"},
        {MIMHID_ERR_INTERNAL, "MIMHID_ERR_INTERNAL"},
    };
    for (const auto& entry : names)
        MIMHID_CHECK(named(mimhid_result_name(entry.result), entry.name));
    // The gaps in the enum (5, 9, 15) and values past its end are not results.
    for (const int value : {5, 9, 15, 17, -1}) {
        MIMHID_CHECK(std::strcmp(mimhid_result_name(static_cast<mimhid_result>(value)),
                                 "MIMHID_RESULT_UNKNOWN") == 0);
        MIMHID_CHECK(mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                     std::strcmp(mimhid_last_error()->field, "result") == 0);
    }
}

void test_frame_type_names() {
    const struct {
        std::int32_t type;
        const char* name;
    } names[] = {
        {MIMHID_T_PING, "PING"},
        {MIMHID_T_DESC_RESET, "DESC_RESET"},
        {MIMHID_T_DESC_BEGIN, "DESC_BEGIN"},
        {MIMHID_T_DESC_DATA, "DESC_DATA"},
        {MIMHID_T_COMMIT, "COMMIT"},
        {MIMHID_T_FEATURE, "FEATURE"},
        {MIMHID_T_RESET, "RESET"},
        {MIMHID_T_REMOTE_WAKEUP, "REMOTE_WAKEUP"},
        {MIMHID_T_BOOTLOADER, "BOOTLOADER"},
        {MIMHID_T_SET_BAUD, "SET_BAUD"},
        {MIMHID_T_CONTROL_CONFIG, "CONTROL_CONFIG"},
        {MIMHID_T_CONTROL_REPLY, "CONTROL_REPLY"},
        {MIMHID_T_LOSS_CONFIG, "LOSS_CONFIG"},
        {MIMHID_T_EP_CONFIG, "EP_CONFIG"},
        {MIMHID_T_PONG, "PONG"},
        {MIMHID_T_ACK, "ACK"},
        {MIMHID_T_NAK, "NAK"},
        {MIMHID_T_EVENT, "EVENT"},
        {MIMHID_T_EP_IN_BASE, "EP_IN"},
        {MIMHID_T_EP_IN_LAST, "EP_IN"},
    };
    for (const auto& entry : names)
        MIMHID_CHECK(named(mimhid_frame_type_name(entry.type), entry.name));
    // Every type the protocol defines has a name.
    for (const std::int32_t type : {MIMHID_T_PROBE, MIMHID_T_COUNTERS}) {
        const char* name = mimhid_frame_type_name(type);
        MIMHID_CHECK(name != nullptr && std::strcmp(name, "UNKNOWN") != 0);
    }
    for (const std::int32_t type : {0x00, 0x3F, 0x50, 0x80, 0xFF, -1, 0x1000}) {
        MIMHID_CHECK(std::strcmp(mimhid_frame_type_name(type), "UNKNOWN") == 0);
        MIMHID_CHECK(mimhid_last_error()->code == MIMHID_ERR_PARAM &&
                     std::strcmp(mimhid_last_error()->field, "frame_type") == 0);
    }
}

void test_last_error() {
    namespace d = mimhid::detail;
    d::set_error(MIMHID_ERR_IO, "field", nullptr, -5, MIMHID_T_RESET, 3, 4, 5, 6);
    const mimhid_error_detail* e = mimhid_last_error();
    MIMHID_CHECK(e->code == MIMHID_ERR_IO && std::strcmp(e->field, "field") == 0);
    MIMHID_CHECK(e->reason != nullptr && e->reason[0] != '\0'); // a null reason gets a text
    MIMHID_CHECK(e->transport_status == -5 && e->frame_type == MIMHID_T_RESET && e->node_id == 3 &&
                 e->report_id == 4 && e->offset == 5 && e->length == 6);
    // finish_result keeps a detail already set for the same result and fills in another.
    MIMHID_CHECK(d::finish_result(MIMHID_ERR_IO, "other", "other") == MIMHID_ERR_IO);
    MIMHID_CHECK(std::strcmp(mimhid_last_error()->field, "field") == 0);
    MIMHID_CHECK(d::finish_result(MIMHID_ERR_BUSY, "other", "busy") == MIMHID_ERR_BUSY);
    MIMHID_CHECK(std::strcmp(mimhid_last_error()->field, "other") == 0);
    const mimhid_error_detail saved = *mimhid_last_error();
    MIMHID_CHECK(d::finish_result(MIMHID_OK, "ignored", "ignored") == MIMHID_OK);
    MIMHID_CHECK(mimhid_last_error()->code == MIMHID_OK && mimhid_last_error()->field == nullptr);
    d::restore_error(saved);
    MIMHID_CHECK(mimhid_last_error()->code == MIMHID_ERR_BUSY);
    // The detail belongs to the thread.
    mimhid_result seen = MIMHID_ERR_INTERNAL;
    std::thread other([&] { seen = mimhid_last_error()->code; });
    other.join();
    MIMHID_CHECK(seen == MIMHID_OK && mimhid_last_error()->code == MIMHID_ERR_BUSY);
    d::clear_error();
}

void test_version() {
    const std::uint32_t version = mimhid_version();
    MIMHID_CHECK(version >> 24U == MIMHID_VERSION_MAJOR &&
                 ((version >> 16U) & 0xFFU) == MIMHID_VERSION_MINOR &&
                 (version & 0xFFFFU) == MIMHID_VERSION_PATCH);
    MIMHID_CHECK(mimhid_last_error()->code == MIMHID_OK);
}

struct Logged {
    std::vector<std::string> lines;
    std::vector<mimhid_log_level> levels;
};

void record(void* user, const mimhid_log_level level, const char* message) {
    auto* logged = static_cast<Logged*>(user);
    logged->levels.push_back(level);
    logged->lines.emplace_back(message);
}

void throwing_sink(void*, mimhid_log_level, const char*) { throw 1; }

void test_log_filter() {
    namespace d = mimhid::detail;
    mimhid_device device;
    Logged logged;
    device.config.transport.log_sink = record;
    device.config.transport.log_user = &logged;
    device.config.transport.log_level = MIMHID_LOG_INFO;
    d::log_message(&device, MIMHID_LOG_ERROR, "error");
    d::log_message(&device, MIMHID_LOG_INFO, "info");
    d::log_message(&device, MIMHID_LOG_TRACE, "trace");       // above the level
    d::log_message(&device, MIMHID_LOG_DISABLED, "disabled"); // not a message level
    d::log_message(&device, MIMHID_LOG_INFO, nullptr);        // no text
    d::log_message(nullptr, MIMHID_LOG_ERROR, "no device");
    MIMHID_CHECK(logged.lines.size() == 2U && logged.lines[0] == "error" &&
                 logged.levels[1] == MIMHID_LOG_INFO);
    device.config.transport.log_level = MIMHID_LOG_DISABLED;
    d::log_message(&device, MIMHID_LOG_ERROR, "off");
    device.config.transport.log_level = MIMHID_LOG_TRACE;
    device.config.transport.log_sink = nullptr;
    d::log_message(&device, MIMHID_LOG_ERROR, "no sink");
    MIMHID_CHECK(logged.lines.size() == 2U);
    // A sink that throws does not unwind into the library.
    device.config.transport.log_sink = throwing_sink;
    d::log_message(&device, MIMHID_LOG_ERROR, "thrown");
    MIMHID_CHECK(true);
}

void test_valid_struct() {
    namespace d = mimhid::detail;
    const std::uint32_t value = 0;
    MIMHID_CHECK(d::valid_struct(&value, 4, 4, "s"));
    MIMHID_CHECK(!d::valid_struct(nullptr, 4, 4, "s") &&
                 mimhid_last_error()->code == MIMHID_ERR_PARAM);
    MIMHID_CHECK(!d::valid_struct(&value, 3, 4, "s") &&
                 std::strcmp(mimhid_last_error()->field, "s") == 0);
    MIMHID_CHECK(d::valid_boolean(0) && d::valid_boolean(1) && !d::valid_boolean(2));
    d::clear_error();
}

} // namespace

void test_error_detail() {
    test_result_names();
    test_frame_type_names();
    test_last_error();
    test_version();
    test_log_filter();
    test_valid_struct();
}
