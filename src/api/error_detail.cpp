// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * This file owns thread-local diagnostics and stable result names. It performs
 * no allocation or synchronization.
 */

#include "api/internal.hpp"
#include "mimhid_proto.h"

#include <cstddef>
#include <cstdint>

namespace {
thread_local mimhid_error_detail g_error{MIMHID_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};

} // namespace

namespace mimhid::detail {

void set_error(const mimhid_result result, const char* field, const char* reason,
               const std::int32_t transport_status, const std::int32_t frame_type,
               const std::uint16_t node_id, const std::uint16_t report_id,
               const std::uint32_t offset, const std::uint32_t length) noexcept {
    g_error = mimhid_error_detail{
        result,           field,      reason == nullptr ? "An unspecified error occurred." : reason,
        transport_status, frame_type, node_id,
        report_id,        offset,     length};
}

void clear_error() noexcept {
    g_error = mimhid_error_detail{MIMHID_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};
}

void restore_error(const mimhid_error_detail& detail) noexcept { g_error = detail; }

mimhid_result finish_result(const mimhid_result result, const char* field, const char* reason,
                            const std::int32_t transport_status, const std::int32_t frame_type,
                            const std::uint16_t node_id, const std::uint16_t report_id,
                            const std::uint32_t offset, const std::uint32_t length) noexcept {
    if (result == MIMHID_OK) {
        clear_error();
    } else if (g_error.code != result) {
        set_error(result, field, reason, transport_status, frame_type, node_id, report_id, offset,
                  length);
    }
    return result;
}

const char* result_name(const mimhid_result result) noexcept {
    switch (result) {
    case MIMHID_OK:
        return "MIMHID_OK";
    case MIMHID_ERR_PARAM:
        return "MIMHID_ERR_PARAM";
    case MIMHID_ERR_UNSET_FIELD:
        return "MIMHID_ERR_UNSET_FIELD";
    case MIMHID_ERR_UNSUPPORTED:
        return "MIMHID_ERR_UNSUPPORTED";
    case MIMHID_ERR_PROTOCOL:
        return "MIMHID_ERR_PROTOCOL";
    case MIMHID_ERR_ACCESS:
        return "MIMHID_ERR_ACCESS";
    case MIMHID_ERR_BUSY:
        return "MIMHID_ERR_BUSY";
    case MIMHID_ERR_NO_DEVICE:
        return "MIMHID_ERR_NO_DEVICE";
    case MIMHID_ERR_TIMEOUT:
        return "MIMHID_ERR_TIMEOUT";
    case MIMHID_ERR_SHORT_TRANSFER:
        return "MIMHID_ERR_SHORT_TRANSFER";
    case MIMHID_ERR_DESCRIPTOR_REJECTED:
        return "MIMHID_ERR_DESCRIPTOR_REJECTED";
    case MIMHID_ERR_IO:
        return "MIMHID_ERR_IO";
    case MIMHID_ERR_OVERFLOW:
        return "MIMHID_ERR_OVERFLOW";
    case MIMHID_ERR_INTERNAL:
        return "MIMHID_ERR_INTERNAL";
    default:
        return nullptr;
    }
}

void log_message(const mimhid_device* device, const mimhid_log_level level,
                 const char* message) noexcept {
    if (device == nullptr || message == nullptr || level < MIMHID_LOG_ERROR ||
        device->config.transport.log_level == MIMHID_LOG_DISABLED ||
        device->config.transport.log_level < level ||
        device->config.transport.log_sink == nullptr) {
        return;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        device->config.transport.log_sink(device->config.transport.log_user, level, message);
    } catch (...) {
        // A sink is foreign callback code. It cannot report a failure through this void
        // callback and must never unwind through libmimhid, so logging is best-effort.
        return;
    }
#else
    device->config.transport.log_sink(device->config.transport.log_user, level, message);
#endif
}

const char* frame_type_name(const std::int32_t frame_type) noexcept {
    if (frame_type >= MIMHID_T_REPORT_BASE && frame_type <= MIMHID_T_REPORT_LAST)
        return "REPORT";
    switch (frame_type) {
    case MIMHID_T_PING:
        return "PING";
    case MIMHID_T_DEVICE_CONFIG:
        return "DEVICE_CONFIG";
    case MIMHID_T_NODE_BEGIN:
        return "NODE_BEGIN";
    case MIMHID_T_NODE_DATA:
        return "NODE_DATA";
    case MIMHID_T_COMMIT:
        return "COMMIT";
    case MIMHID_T_FEATURE:
        return "FEATURE";
    case MIMHID_T_RESET:
        return "RESET";
    case MIMHID_T_REMOTE_WAKEUP:
        return "REMOTE_WAKEUP";
    case MIMHID_T_SET_BAUD:
        return "SET_BAUD";
    case MIMHID_T_CONTROL_CONFIG:
        return "CONTROL_CONFIG";
    case MIMHID_T_CONTROL_REPLY:
        return "CONTROL_REPLY";
    case MIMHID_T_LOSS_CONFIG:
        return "LOSS_CONFIG";
    case MIMHID_T_QUEUE_DEPTH:
        return "QUEUE_DEPTH";
    case MIMHID_T_PONG:
        return "PONG";
    case MIMHID_T_ACK:
        return "ACK";
    case MIMHID_T_NAK:
        return "NAK";
    case MIMHID_T_EVENT:
        return "EVENT";
    default:
        return nullptr;
    }
}

bool valid_boolean(const std::uint32_t value) noexcept { return value == 0U || value == 1U; }

bool valid_struct(const void* value, const std::uint32_t actual_size,
                  const std::uint32_t expected_size, const char* field) noexcept {
    if (value == nullptr) {
        set_error(MIMHID_ERR_PARAM, field, "The required structure pointer is null.");
        return false;
    }
    if (actual_size != expected_size) {
        set_error(MIMHID_ERR_PARAM, field,
                  "struct_size must equal the size of the ABI structure used by this release.");
        return false;
    }
    return true;
}

} // namespace mimhid::detail

extern "C" {

const mimhid_error_detail* MIMHID_CALL mimhid_last_error(void) try {
    return &g_error;
} catch (...) {
    mimhid::detail::set_error(MIMHID_ERR_INTERNAL, "last_error",
                              "An internal C++ operation failed inside the public C ABI call.");
    return &g_error;
}

const char* MIMHID_CALL mimhid_result_name(const mimhid_result result) try {
    const char* name = mimhid::detail::result_name(result);
    if (name == nullptr) {
        mimhid::detail::set_error(MIMHID_ERR_PARAM, "result",
                                  "The result value is outside the public mimhid_result enum.");
        return "MIMHID_RESULT_UNKNOWN";
    }
    mimhid::detail::clear_error();
    return name;
} catch (...) {
    mimhid::detail::set_error(MIMHID_ERR_INTERNAL, "result.name",
                              "An internal C++ operation failed inside the public C ABI call.");
    return "MIMHID_ERR_INTERNAL";
}

const char* MIMHID_CALL mimhid_frame_type_name(const std::int32_t frame_type) try {
    const char* name = mimhid::detail::frame_type_name(frame_type);
    if (name == nullptr) {
        mimhid::detail::set_error(MIMHID_ERR_PARAM, "frame_type",
                                  "The value is not a frame type of the serial protocol.");
        return "UNKNOWN";
    }
    mimhid::detail::clear_error();
    return name;
} catch (...) {
    mimhid::detail::set_error(MIMHID_ERR_INTERNAL, "frame_type.name",
                              "An internal C++ operation failed inside the public C ABI call.");
    return "UNKNOWN";
}

std::uint32_t MIMHID_CALL mimhid_version(void) try {
    mimhid::detail::clear_error();
    return (MIMHID_VERSION_MAJOR << 24U) | (MIMHID_VERSION_MINOR << 16U) | MIMHID_VERSION_PATCH;
} catch (...) {
    mimhid::detail::set_error(MIMHID_ERR_INTERNAL, "version",
                              "An internal C++ operation failed inside the public C ABI call.");
    return 0U;
}

} // extern "C"
