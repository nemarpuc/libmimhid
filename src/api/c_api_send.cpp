// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Public C ABI: sending reports, and the serial write path they share with the close paths.
 */

#include "api/support.hpp"

#include <array>
#include <chrono>

namespace tr = mimhid::transport;

using mimhid::detail::has_non_neutral_state;
using mimhid::detail::kMaximumReportBytes;
using mimhid::detail::make_neutral;
using mimhid::detail::public_node_id;
using mimhid::detail::report_id;
using mimhid::detail::require_timeout;
using mimhid::detail::send_neutral;
using mimhid::detail::set_error;
using mimhid::detail::touch_frame_in_progress;

namespace {

const char* completion_reason(const mimhid_result result) noexcept {
    switch (result) {
    case MIMHID_ERR_TIMEOUT:
        return "The serial write made no progress before the send timeout; nothing was sent and "
               "the state stays pending for a resend.";
    case MIMHID_ERR_SHORT_TRANSFER:
        return "The serial write stopped part way through a frame; the board drops a frame that "
               "fails its checksum, so delivery cannot be established.";
    case MIMHID_ERR_NO_DEVICE:
        return "The serial port is gone.";
    default:
        return "The serial write failed.";
    }
}

mimhid_result consume_completion(mimhid_node* node, const char* field) noexcept {
    const mimhid_result result =
        node->completion_result.exchange(MIMHID_OK, std::memory_order_acq_rel);
    if (result == MIMHID_OK) {
        node->completion_native_status.store(0, std::memory_order_relaxed);
        node->completion_report_length.store(0U, std::memory_order_relaxed);
        node->completion_report_id.store(0U, std::memory_order_relaxed);
        return MIMHID_OK;
    }
    const std::int32_t native_status =
        node->completion_native_status.exchange(0, std::memory_order_relaxed);
    const std::uint32_t length =
        node->completion_report_length.exchange(0U, std::memory_order_relaxed);
    const std::uint16_t completed_report_id =
        node->completion_report_id.exchange(0U, std::memory_order_relaxed);
    set_error(result, field, completion_reason(result), native_status, 0, public_node_id(node),
              completed_report_id, 0U, length);
    return result;
}

// Writes one serialized report and settles the node state machine with the outcome, exactly
// as the completion of an asynchronous transfer would. A negative native status means that no
// byte reached the serial driver, so the pending state is kept for a resend.
mimhid_result write_report(mimhid_node* node, const std::uint8_t* report, std::size_t length,
                           const char* field, std::uint32_t send_timeout_ms) noexcept {
    mimhid_device* device = node->device;
    std::int32_t os_error = 0;
    const tr::IoStatus status = device->link.send_report(node->config.ep_in.address, report, length,
                                                         send_timeout_ms, &os_error);
    mimhid_result result = MIMHID_OK;
    std::int32_t native = 0;
    switch (status) {
    case tr::IoStatus::ok:
        break;
    case tr::IoStatus::timeout:
        result = MIMHID_ERR_TIMEOUT;
        native = -1;
        break;
    case tr::IoStatus::short_write:
        result = MIMHID_ERR_SHORT_TRANSFER;
        native = 0;
        break;
    case tr::IoStatus::closed:
        result = MIMHID_ERR_NO_DEVICE;
        native = -1;
        break;
    case tr::IoStatus::error:
        result = MIMHID_ERR_IO;
        native = -1;
        break;
    }
    if (result == MIMHID_ERR_NO_DEVICE)
        device->latched.store(MIMHID_ERR_NO_DEVICE, std::memory_order_release);
    mimhid::detail::transfer_complete(node, result, native);
    return consume_completion(node, field);
}

mimhid_result flush_node(mimhid_node* node, const std::uint32_t timeout_ms,
                         const bool drop_battery) {
    if (timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "timeout_ms", "A nonzero timeout is required.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const mimhid_result completion = consume_completion(node, "node.completion");
        if (completion != MIMHID_OK)
            return completion;
        if (drop_battery)
            node->battery_dirty = false;
        if (!node->dirty && !node->battery_dirty)
            return MIMHID_OK;
        // A write may wait for what is left of the deadline, and at least a millisecond.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const mimhid_result submit = mimhid_node_submit(
            node, left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 1U);
        if (submit != MIMHID_OK)
            return submit;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }
    if (!node->dirty && !node->battery_dirty)
        return MIMHID_OK;
    set_error(MIMHID_ERR_TIMEOUT, "node.submit_blocking",
              "The report sequence did not finish before the deadline.", 0, 0, public_node_id(node),
              report_id(node), 0U, static_cast<std::uint32_t>(node->spec->layout.wire_bytes));
    return MIMHID_ERR_TIMEOUT;
}

} // namespace

namespace mimhid::detail {

// Sends a node's pending reports until it is clean, for the close paths.
mimhid_result send_neutral(mimhid_node* node, std::uint32_t timeout_ms) {
    if (node->generic)
        return MIMHID_OK; // no report state: what the target sees of it ends with the commit
    mimhid_result result = MIMHID_OK;
    if (node->transfer_inflight.load(std::memory_order_acquire) || touch_frame_in_progress(node)) {
        result = flush_node(node, timeout_ms, true);
        if (result != MIMHID_OK)
            return result;
    }
    const mimhid_result terminal = consume_completion(node, "node.completion");
    if (terminal != MIMHID_OK)
        return terminal;
    node->battery_dirty = false;
    if (node->device->link.failed())
        return MIMHID_OK; // the target keeps whatever it last saw; nothing can be sent
    make_neutral(node);
    if (node->dirty)
        return flush_node(node, timeout_ms, true);
    return MIMHID_OK;
}

} // namespace mimhid::detail

extern "C" {

mimhid_result MIMHID_CALL mimhid_node_submit(mimhid_node* node,
                                             const std::uint32_t send_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(MIMHID_ERR_PARAM, "node.submit",
                  "A live Node attached to a Device and Spec is required.");
        return MIMHID_ERR_PARAM;
    }
    if (send_timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "send_timeout_ms",
                  "This timeout must be set; it has no default.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (node->spec->kind == MIMHID_PROFILE_RAW) {
        set_error(MIMHID_ERR_UNSUPPORTED, "node.submit",
                  "Raw nodes require mimhid_raw_submit() with an explicit report buffer.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (const mimhid_result link = mimhid::detail::link_available(node, "node.submit");
        link != MIMHID_OK) {
        return link;
    }
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMHID_ERR_BUSY, "node.submit", "The node already has one report in flight.");
        return MIMHID_ERR_BUSY;
    }
    const mimhid_result completion = consume_completion(node, "node.completion");
    if (completion != MIMHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    // Input always goes first; the battery report is sent only by a submit
    // that has no input report to send.
    const bool battery_report = !node->dirty && node->battery_dirty;
    if (!node->dirty && !battery_report) {
        node->transfer_inflight.store(false, std::memory_order_release);
        mimhid::detail::clear_error();
        return MIMHID_OK;
    }
    const mimhid::hid::ReportLayout& layout =
        battery_report ? node->spec->battery_layout : node->spec->layout;
    std::array<std::uint8_t, kMaximumReportBytes> buffer; // only wire_bytes of it is used
    if (layout.wire_bytes > buffer.size() ||
        layout.wire_bytes > node->device->maximum_report_bytes) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(MIMHID_ERR_OVERFLOW, "node.submit",
                  "The report is longer than the board or the caller allows.");
        return MIMHID_ERR_OVERFLOW;
    }
    auto* scan_touch =
        battery_report ? nullptr : std::get_if<mimhid::detail::TouchState>(&node->state);
    const auto* scan_config = std::get_if<mimhid::detail::TouchConfig>(&node->spec->config);
    const bool update_scan_time = scan_touch != nullptr && scan_touch->packet_cursor == 0U &&
                                  scan_config != nullptr &&
                                  scan_config->fields.enable_scan_time == 1U;
    const std::uint32_t previous_scan_time =
        update_scan_time ? scan_touch->scan_time : std::uint32_t{0};
    const auto previous_scan_epoch =
        update_scan_time ? scan_touch->scan_epoch : std::chrono::steady_clock::time_point{};
    const bool previous_scan_epoch_active = update_scan_time && scan_touch->scan_epoch_active;
    const auto rollback_scan_time = [&]() noexcept {
        if (update_scan_time) {
            scan_touch->scan_time = previous_scan_time;
            scan_touch->scan_epoch = previous_scan_epoch;
            scan_touch->scan_epoch_active = previous_scan_epoch_active;
        }
    };
    if (update_scan_time) {
        const auto now = std::chrono::steady_clock::now();
        if (!scan_touch->scan_epoch_active) {
            scan_touch->scan_epoch = now;
            scan_touch->scan_epoch_active = true;
            scan_touch->scan_time = 0U;
        } else {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(now - scan_touch->scan_epoch)
                    .count();
            const std::uint64_t ticks =
                elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed) / 100U;
            const std::uint64_t modulus =
                static_cast<std::uint64_t>(scan_config->fields.scan_time.logical_maximum) + 1U;
            scan_touch->scan_time = static_cast<std::uint32_t>(ticks % modulus);
        }
    }
    std::size_t report_length = 0U;
    const mimhid_result serialized =
        battery_report
            ? mimhid::detail::serialize_battery(node, buffer.data(), buffer.size(), &report_length)
            : mimhid::detail::serialize_node(node, buffer.data(), buffer.size(), &report_length);
    if (serialized != MIMHID_OK) {
        rollback_scan_time();
        node->transfer_inflight.store(false, std::memory_order_release);
        return serialized;
    }
    if (node->device->config.transport.validate_reports == 1U) {
        const mimhid_result validated = mimhid::detail::validate_serialized_report(
            node->spec, layout, buffer.data(), report_length);
        if (validated != MIMHID_OK) {
            rollback_scan_time();
            node->transfer_inflight.store(false, std::memory_order_release);
            return validated;
        }
    }
    node->submitted_battery = battery_report;
    node->submitted_non_neutral = !battery_report && has_non_neutral_state(node);
    node->submitted_report_length = report_length;
    node->submitted_report_id = battery_report ? layout.report_id : report_id(node);
    const mimhid_result result =
        write_report(node, buffer.data(), report_length, "node.submit", send_timeout_ms);
    if (result != MIMHID_OK)
        rollback_scan_time();
    else
        mimhid::detail::clear_error();
    return result;
}
MIMHID_C_RESULT_CATCH("node.submit")

mimhid_result MIMHID_CALL mimhid_node_submit_blocking(mimhid_node* node,
                                                      const std::uint32_t timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(MIMHID_ERR_PARAM, "node.submit_blocking",
                  "A live Node with an attached Device and Spec is required.");
        return MIMHID_ERR_PARAM;
    }
    const mimhid_result result = flush_node(node, timeout_ms, false);
    return mimhid::detail::finish_result(result, "node.submit_blocking",
                                         "The blocking report sequence failed.");
}
MIMHID_C_RESULT_CATCH("node.submit_blocking")

mimhid_result MIMHID_CALL mimhid_raw_submit(mimhid_node* node, const std::uint8_t* report,
                                            const std::size_t length,
                                            const std::uint32_t send_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr ||
        node->spec->kind != MIMHID_PROFILE_RAW || report == nullptr) {
        set_error(MIMHID_ERR_PARAM, "raw.submit", "A live raw node and a report are required.");
        return MIMHID_ERR_PARAM;
    }
    if (send_timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "send_timeout_ms",
                  "This timeout must be set; it has no default.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (const mimhid_result link = mimhid::detail::link_available(node, "raw.submit");
        link != MIMHID_OK) {
        return link;
    }
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMHID_ERR_BUSY, "raw.submit", "The raw node already has one report in flight.");
        return MIMHID_ERR_BUSY;
    }
    const mimhid_result completion = consume_completion(node, "raw.completion");
    if (completion != MIMHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    const auto* raw = std::get_if<mimhid::detail::RawConfig>(&node->spec->config);
    std::uint16_t submitted_id = 0U;
    if (raw != nullptr) {
        const auto accepted =
            std::find_if(raw->reports.begin(), raw->reports.end(), [=](const auto& item) {
                return item.wire_length == length &&
                       ((item.report_id.enabled == 0U) ||
                        (length != 0U && report[0] == item.report_id.value));
            });
        if (accepted == raw->reports.end()) {
            node->transfer_inflight.store(false, std::memory_order_release);
            set_error(MIMHID_ERR_PARAM, "raw.report",
                      "The report ID or exact wire length is not accepted by the raw spec.");
            return MIMHID_ERR_PARAM;
        }
        const std::size_t accepted_index =
            static_cast<std::size_t>(accepted - raw->reports.begin());
        const std::uint8_t valid_mask = raw->trailing_valid_masks[accepted_index];
        if ((report[length - 1U] & static_cast<std::uint8_t>(~valid_mask)) != 0U) {
            node->transfer_inflight.store(false, std::memory_order_release);
            set_error(MIMHID_ERR_PARAM, "raw.report.trailing_padding",
                      "Unused trailing Input-report bits must be zero.", 0, 0, public_node_id(node),
                      accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U,
                      static_cast<std::uint32_t>(length - 1U), 1U);
            return MIMHID_ERR_PARAM;
        }
        submitted_id = accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U;
    } else {
        submitted_id = (node->has_report_ids && length > 0U) ? report[0] : 0U;
    }
    if (length > kMaximumReportBytes || length > node->device->maximum_report_bytes) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(MIMHID_ERR_OVERFLOW, "raw.report",
                  "The report is longer than the board or the caller allows.");
        return MIMHID_ERR_OVERFLOW;
    }
    node->dirty = true;
    node->submitted_non_neutral = false;
    node->submitted_report_length = length;
    node->submitted_report_id = submitted_id;
    const mimhid_result result = write_report(node, report, length, "raw.submit", send_timeout_ms);
    if (result == MIMHID_OK)
        mimhid::detail::clear_error();
    return result;
}
MIMHID_C_RESULT_CATCH("raw.submit")

mimhid_result MIMHID_CALL mimhid_report_send(mimhid_node* node, const uint8_t* report_bytes,
                                             size_t report_length,
                                             const uint32_t send_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        (report_bytes == nullptr && report_length != 0U)) {
        set_error(MIMHID_ERR_PARAM, "report_send",
                  "A live Node and valid report buffer are required.");
        return MIMHID_ERR_PARAM;
    }
    if (node->generic) {
        set_error(MIMHID_ERR_UNSUPPORTED, "report_send",
                  "A Node opened with mimhid_node_open sends with mimhid_node_endpoint_write.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (const mimhid_result t = require_timeout(send_timeout_ms, "send_timeout_ms"); t != MIMHID_OK)
        return t;
    if (const mimhid_result link = mimhid::detail::link_available(node, "report_send");
        link != MIMHID_OK) {
        return link;
    }
    mimhid_device* device = node->device;
    if (report_length > device->maximum_report_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "report_send",
                  "The report is longer than the board allows.");
        return MIMHID_ERR_OVERFLOW;
    }
    std::int32_t os_error = 0;
    const tr::IoStatus status = device->link.send_report(node->config.ep_in.address, report_bytes,
                                                         report_length, send_timeout_ms, &os_error);
    switch (status) {
    case tr::IoStatus::ok:
        mimhid::detail::clear_error();
        return MIMHID_OK;
    case tr::IoStatus::timeout:
        set_error(MIMHID_ERR_TIMEOUT, "report_send", "Report send timed out.", os_error);
        return MIMHID_ERR_TIMEOUT;
    case tr::IoStatus::short_write:
        set_error(MIMHID_ERR_SHORT_TRANSFER, "report_send", "Short write during report send.",
                  os_error);
        return MIMHID_ERR_SHORT_TRANSFER;
    case tr::IoStatus::closed:
        device->latched.store(MIMHID_ERR_NO_DEVICE, std::memory_order_release);
        set_error(MIMHID_ERR_NO_DEVICE, "report_send", "The serial device was disconnected.",
                  os_error);
        return MIMHID_ERR_NO_DEVICE;
    case tr::IoStatus::error:
        set_error(MIMHID_ERR_IO, "report_send", "I/O error during report send.", os_error);
        return MIMHID_ERR_IO;
    }
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("report_send")

} // extern "C"
