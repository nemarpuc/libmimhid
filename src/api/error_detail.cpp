// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * This file owns thread-local diagnostics and stable result names. It performs
 * no allocation or synchronization.
 */

#include "api/internal.hpp"
#include "mimusb_proto.h"

#include <cstddef>
#include <cstdint>

namespace {
thread_local mimusb_error_detail g_error{MIMUSB_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};

} // namespace

namespace mimusb::detail {

void set_error(const mimusb_result result, const char* field, const char* reason,
               const std::int32_t transport_status, const std::int32_t frame_type,
               const std::uint16_t node_id, const std::uint16_t report_id,
               const std::uint32_t offset, const std::uint32_t length) noexcept {
    g_error = mimusb_error_detail{
        result,           field,      reason == nullptr ? "An unspecified error occurred." : reason,
        transport_status, frame_type, node_id,
        report_id,        offset,     length};
}

void clear_error() noexcept {
    g_error = mimusb_error_detail{MIMUSB_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};
}

void restore_error(const mimusb_error_detail& detail) noexcept { g_error = detail; }

mimusb_result finish_result(const mimusb_result result, const char* field, const char* reason,
                            const std::int32_t transport_status, const std::int32_t frame_type,
                            const std::uint16_t node_id, const std::uint16_t report_id,
                            const std::uint32_t offset, const std::uint32_t length) noexcept {
    if (result == MIMUSB_OK) {
        clear_error();
    } else if (g_error.code != result) {
        set_error(result, field, reason, transport_status, frame_type, node_id, report_id, offset,
                  length);
    }
    return result;
}

const char* result_name(const mimusb_result result) noexcept {
    switch (result) {
    case MIMUSB_OK:
        return "MIMUSB_OK";
    case MIMUSB_ERR_PARAM:
        return "MIMUSB_ERR_PARAM";
    case MIMUSB_ERR_UNSET_FIELD:
        return "MIMUSB_ERR_UNSET_FIELD";
    case MIMUSB_ERR_UNSUPPORTED:
        return "MIMUSB_ERR_UNSUPPORTED";
    case MIMUSB_ERR_PROTOCOL:
        return "MIMUSB_ERR_PROTOCOL";
    case MIMUSB_ERR_ACCESS:
        return "MIMUSB_ERR_ACCESS";
    case MIMUSB_ERR_BUSY:
        return "MIMUSB_ERR_BUSY";
    case MIMUSB_ERR_NO_DEVICE:
        return "MIMUSB_ERR_NO_DEVICE";
    case MIMUSB_ERR_TIMEOUT:
        return "MIMUSB_ERR_TIMEOUT";
    case MIMUSB_ERR_SHORT_TRANSFER:
        return "MIMUSB_ERR_SHORT_TRANSFER";
    case MIMUSB_ERR_DESCRIPTOR_REJECTED:
        return "MIMUSB_ERR_DESCRIPTOR_REJECTED";
    case MIMUSB_ERR_IO:
        return "MIMUSB_ERR_IO";
    case MIMUSB_ERR_OVERFLOW:
        return "MIMUSB_ERR_OVERFLOW";
    case MIMUSB_ERR_INTERNAL:
        return "MIMUSB_ERR_INTERNAL";
    default:
        return nullptr;
    }
}

void log_message(const mimusb_device* device, const mimusb_log_level level,
                 const char* message) noexcept {
    if (device == nullptr || message == nullptr || level < MIMUSB_LOG_ERROR ||
        device->config.transport.log_level == MIMUSB_LOG_DISABLED ||
        device->config.transport.log_level < level ||
        device->config.transport.log_sink == nullptr) {
        return;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        device->config.transport.log_sink(device->config.transport.log_user, level, message);
    } catch (...) {
        // A sink is foreign callback code. It cannot report a failure through this void
        // callback and must never unwind through libmimusb, so logging is best-effort.
        return;
    }
#else
    device->config.transport.log_sink(device->config.transport.log_user, level, message);
#endif
}

const char* frame_type_name(const std::int32_t frame_type) noexcept {
    if (frame_type >= MIMUSB_T_EP_IN_BASE && frame_type <= MIMUSB_T_EP_IN_LAST)
        return "EP_IN";
    if (frame_type >= MIMUSB_T_EP_OUT_BASE && frame_type <= MIMUSB_T_EP_OUT_LAST)
        return "EP_OUT";
    switch (frame_type) {
    case MIMUSB_T_PING:
        return "PING";
    case MIMUSB_T_DESC_RESET:
        return "DESC_RESET";
    case MIMUSB_T_DESC_BEGIN:
        return "DESC_BEGIN";
    case MIMUSB_T_DESC_DATA:
        return "DESC_DATA";
    case MIMUSB_T_COMMIT:
        return "COMMIT";
    case MIMUSB_T_RULE:
        return "RULE";
    case MIMUSB_T_RULES_APPLY:
        return "RULES_APPLY";
    case MIMUSB_T_RULE_DATA:
        return "RULE_DATA";
    case MIMUSB_T_RESET:
        return "RESET";
    case MIMUSB_T_REMOTE_WAKEUP:
        return "REMOTE_WAKEUP";
    case MIMUSB_T_SET_BAUD:
        return "SET_BAUD";
    case MIMUSB_T_CONTROL_REPLY:
        return "CONTROL_REPLY";
    case MIMUSB_T_LOSS_CONFIG:
        return "LOSS_CONFIG";
    case MIMUSB_T_EP_CONFIG:
        return "EP_CONFIG";
    case MIMUSB_T_PROBE:
        return "PROBE";
    case MIMUSB_T_COUNTERS:
        return "COUNTERS";
    case MIMUSB_T_EP_STALL:
        return "EP_STALL";
    case MIMUSB_T_EP_FLUSH:
        return "EP_FLUSH";
    case MIMUSB_T_BOOTLOADER:
        return "BOOTLOADER";
    case MIMUSB_T_PONG:
        return "PONG";
    case MIMUSB_T_ACK:
        return "ACK";
    case MIMUSB_T_NAK:
        return "NAK";
    case MIMUSB_T_EVENT:
        return "EVENT";
    default:
        return nullptr;
    }
}

bool valid_boolean(const std::uint32_t value) noexcept { return value == 0U || value == 1U; }

} // namespace mimusb::detail

extern "C" {

const mimusb_error_detail* MIMUSB_CALL mimusb_last_error(void) try {
    return &g_error;
} catch (...) {
    mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, "last_error",
                              "An internal C++ operation failed inside the public C ABI call.");
    return &g_error;
}

const char* MIMUSB_CALL mimusb_result_name(const mimusb_result result) try {
    const char* name = mimusb::detail::result_name(result);
    if (name == nullptr) {
        mimusb::detail::set_error(MIMUSB_ERR_PARAM, "result",
                                  "The result value is outside the public mimusb_result enum.");
        return "MIMUSB_RESULT_UNKNOWN";
    }
    mimusb::detail::clear_error();
    return name;
} catch (...) {
    mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, "result.name",
                              "An internal C++ operation failed inside the public C ABI call.");
    return "MIMUSB_ERR_INTERNAL";
}

const char* MIMUSB_CALL mimusb_frame_type_name(const std::int32_t frame_type) try {
    const char* name = mimusb::detail::frame_type_name(frame_type);
    if (name == nullptr) {
        mimusb::detail::set_error(MIMUSB_ERR_PARAM, "frame_type",
                                  "The value is not a frame type of the serial protocol.");
        return "UNKNOWN";
    }
    mimusb::detail::clear_error();
    return name;
} catch (...) {
    mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, "frame_type.name",
                              "An internal C++ operation failed inside the public C ABI call.");
    return "UNKNOWN";
}

std::uint32_t MIMUSB_CALL mimusb_version(void) try {
    mimusb::detail::clear_error();
    return (MIMUSB_VERSION_MAJOR << 24U) | (MIMUSB_VERSION_MINOR << 16U) | MIMUSB_VERSION_PATCH;
} catch (...) {
    mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, "version",
                              "An internal C++ operation failed inside the public C ABI call.");
    return 0U;
}

} // extern "C"
