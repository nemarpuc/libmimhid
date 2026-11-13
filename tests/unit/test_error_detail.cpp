// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The diagnostics every call leaves behind: the stable names of results and frame types (and what
 * an unknown value gives), the thread-local last error, the version, the log filter and the
 * structure-size check. */
#include "api/internal.hpp"
#include "classes/hid/hid_class.hpp"
#include "mimusb.h"
#include "mimusb_proto.h"
#include "test.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool named(const char* name, const char* expected) {
    return name != nullptr && std::strcmp(name, expected) == 0 &&
           mimusb_last_error()->code == MIMUSB_OK;
}

void test_result_names() {
    const struct {
        mimusb_result result;
        const char* name;
    } names[] = {
        {MIMUSB_OK, "MIMUSB_OK"},
        {MIMUSB_ERR_PARAM, "MIMUSB_ERR_PARAM"},
        {MIMUSB_ERR_UNSET_FIELD, "MIMUSB_ERR_UNSET_FIELD"},
        {MIMUSB_ERR_UNSUPPORTED, "MIMUSB_ERR_UNSUPPORTED"},
        {MIMUSB_ERR_PROTOCOL, "MIMUSB_ERR_PROTOCOL"},
        {MIMUSB_ERR_ACCESS, "MIMUSB_ERR_ACCESS"},
        {MIMUSB_ERR_BUSY, "MIMUSB_ERR_BUSY"},
        {MIMUSB_ERR_NO_DEVICE, "MIMUSB_ERR_NO_DEVICE"},
        {MIMUSB_ERR_TIMEOUT, "MIMUSB_ERR_TIMEOUT"},
        {MIMUSB_ERR_SHORT_TRANSFER, "MIMUSB_ERR_SHORT_TRANSFER"},
        {MIMUSB_ERR_DESCRIPTOR_REJECTED, "MIMUSB_ERR_DESCRIPTOR_REJECTED"},
        {MIMUSB_ERR_IO, "MIMUSB_ERR_IO"},
        {MIMUSB_ERR_OVERFLOW, "MIMUSB_ERR_OVERFLOW"},
        {MIMUSB_ERR_INTERNAL, "MIMUSB_ERR_INTERNAL"},
    };
    for (const auto& entry : names)
        MIMUSB_CHECK(named(mimusb_result_name(entry.result), entry.name));
    // The gaps in the enum (5, 9, 15) and values past its end are not results.
    for (const int value : {5, 9, 15, 17, -1}) {
        MIMUSB_CHECK(std::strcmp(mimusb_result_name(static_cast<mimusb_result>(value)),
                                 "MIMUSB_RESULT_UNKNOWN") == 0);
        MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                     std::strcmp(mimusb_last_error()->field, "result") == 0);
    }
}

void test_frame_type_names() {
    const struct {
        std::int32_t type;
        const char* name;
    } names[] = {
        {MIMUSB_T_PING, "PING"},
        {MIMUSB_T_DESC_RESET, "DESC_RESET"},
        {MIMUSB_T_DESC_BEGIN, "DESC_BEGIN"},
        {MIMUSB_T_DESC_DATA, "DESC_DATA"},
        {MIMUSB_T_COMMIT, "COMMIT"},
        {MIMUSB_T_RULE, "RULE"},
        {MIMUSB_T_RULES_APPLY, "RULES_APPLY"},
        {MIMUSB_T_RULE_DATA, "RULE_DATA"},
        {MIMUSB_T_RESET, "RESET"},
        {MIMUSB_T_REMOTE_WAKEUP, "REMOTE_WAKEUP"},
        {MIMUSB_T_BOOTLOADER, "BOOTLOADER"},
        {MIMUSB_T_SET_BAUD, "SET_BAUD"},
        {MIMUSB_T_CONTROL_REPLY, "CONTROL_REPLY"},
        {MIMUSB_T_LOSS_CONFIG, "LOSS_CONFIG"},
        {MIMUSB_T_EP_CONFIG, "EP_CONFIG"},
        {MIMUSB_T_PONG, "PONG"},
        {MIMUSB_T_ACK, "ACK"},
        {MIMUSB_T_NAK, "NAK"},
        {MIMUSB_T_EVENT, "EVENT"},
        {MIMUSB_T_EP_IN_BASE, "EP_IN"},
        {MIMUSB_T_EP_IN_LAST, "EP_IN"},
    };
    for (const auto& entry : names)
        MIMUSB_CHECK(named(mimusb_frame_type_name(entry.type), entry.name));
    // Every type the protocol defines has a name.
    for (const std::int32_t type : {MIMUSB_T_PROBE, MIMUSB_T_COUNTERS}) {
        const char* name = mimusb_frame_type_name(type);
        MIMUSB_CHECK(name != nullptr && std::strcmp(name, "UNKNOWN") != 0);
    }
    for (const std::int32_t type : {0x00, 0x3F, 0x50, 0x80, 0xFF, -1, 0x1000}) {
        MIMUSB_CHECK(std::strcmp(mimusb_frame_type_name(type), "UNKNOWN") == 0);
        MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_ERR_PARAM &&
                     std::strcmp(mimusb_last_error()->field, "frame_type") == 0);
    }
}

void test_last_error() {
    namespace d = mimusb::detail;
    d::set_error(MIMUSB_ERR_IO, "field", nullptr, -5, MIMUSB_T_RESET, 3, 4, 5, 6);
    const mimusb_error_detail* e = mimusb_last_error();
    MIMUSB_CHECK(e->code == MIMUSB_ERR_IO && std::strcmp(e->field, "field") == 0);
    MIMUSB_CHECK(e->reason != nullptr && e->reason[0] != '\0'); // a null reason gets a text
    MIMUSB_CHECK(e->transport_status == -5 && e->frame_type == MIMUSB_T_RESET && e->node_id == 3 &&
                 e->report_id == 4 && e->offset == 5 && e->length == 6);
    // finish_result keeps a detail already set for the same result and fills in another.
    MIMUSB_CHECK(d::finish_result(MIMUSB_ERR_IO, "other", "other") == MIMUSB_ERR_IO);
    MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "field") == 0);
    MIMUSB_CHECK(d::finish_result(MIMUSB_ERR_BUSY, "other", "busy") == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "other") == 0);
    const mimusb_error_detail saved = *mimusb_last_error();
    MIMUSB_CHECK(d::finish_result(MIMUSB_OK, "ignored", "ignored") == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_OK && mimusb_last_error()->field == nullptr);
    d::restore_error(saved);
    MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_ERR_BUSY);
    // The detail belongs to the thread.
    mimusb_result seen = MIMUSB_ERR_INTERNAL;
    std::thread other([&] { seen = mimusb_last_error()->code; });
    other.join();
    MIMUSB_CHECK(seen == MIMUSB_OK && mimusb_last_error()->code == MIMUSB_ERR_BUSY);
    d::clear_error();
}

void test_version() {
    const std::uint32_t version = mimusb_version();
    MIMUSB_CHECK(version >> 24U == MIMUSB_VERSION_MAJOR &&
                 ((version >> 16U) & 0xFFU) == MIMUSB_VERSION_MINOR &&
                 (version & 0xFFFFU) == MIMUSB_VERSION_PATCH);
    MIMUSB_CHECK(mimusb_last_error()->code == MIMUSB_OK);
}

struct Logged {
    std::vector<std::string> lines;
    std::vector<mimusb_log_level> levels;
};

void record(void* user, const mimusb_log_level level, const char* message) {
    auto* logged = static_cast<Logged*>(user);
    logged->levels.push_back(level);
    logged->lines.emplace_back(message);
}

void throwing_sink(void*, mimusb_log_level, const char*) { throw 1; }

void test_log_filter() {
    namespace d = mimusb::detail;
    mimusb_device device;
    Logged logged;
    device.config.transport.log_sink = record;
    device.config.transport.log_user = &logged;
    device.config.transport.log_level = MIMUSB_LOG_INFO;
    d::log_message(&device, MIMUSB_LOG_ERROR, "error");
    d::log_message(&device, MIMUSB_LOG_INFO, "info");
    d::log_message(&device, MIMUSB_LOG_TRACE, "trace");       // above the level
    d::log_message(&device, MIMUSB_LOG_DISABLED, "disabled"); // not a message level
    d::log_message(&device, MIMUSB_LOG_INFO, nullptr);        // no text
    d::log_message(nullptr, MIMUSB_LOG_ERROR, "no device");
    MIMUSB_CHECK(logged.lines.size() == 2U && logged.lines[0] == "error" &&
                 logged.levels[1] == MIMUSB_LOG_INFO);
    device.config.transport.log_level = MIMUSB_LOG_DISABLED;
    d::log_message(&device, MIMUSB_LOG_ERROR, "off");
    device.config.transport.log_level = MIMUSB_LOG_TRACE;
    device.config.transport.log_sink = nullptr;
    d::log_message(&device, MIMUSB_LOG_ERROR, "no sink");
    MIMUSB_CHECK(logged.lines.size() == 2U);
    // A sink that throws does not unwind into the library.
    device.config.transport.log_sink = throwing_sink;
    d::log_message(&device, MIMUSB_LOG_ERROR, "thrown");
    MIMUSB_CHECK(true);
}

void test_valid_struct() {
    namespace d = mimusb::detail;
    const std::uint32_t value = 0;
    MIMUSB_CHECK(d::valid_struct(&value, 4, 4, "s"));
    MIMUSB_CHECK(!d::valid_struct(nullptr, 4, 4, "s") &&
                 mimusb_last_error()->code == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(!d::valid_struct(&value, 3, 4, "s") &&
                 std::strcmp(mimusb_last_error()->field, "s") == 0);
    MIMUSB_CHECK(d::valid_boolean(0) && d::valid_boolean(1) && !d::valid_boolean(2));
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
