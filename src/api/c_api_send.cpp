// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: sending reports, and the serial write path they share with the close paths.
 */

#include "api/support.hpp"

#include <array>
#include <chrono>

namespace tr = mimusb::transport;

using mimusb::detail::has_non_neutral_state;
using mimusb::detail::kMaximumReportBytes;
using mimusb::detail::make_neutral;
using mimusb::detail::public_node_id;
using mimusb::detail::report_id;
using mimusb::detail::require_timeout;
using mimusb::detail::send_neutral;
using mimusb::detail::set_error;
using mimusb::detail::touch_frame_in_progress;

namespace {

const char* completion_reason(const mimusb_result result) noexcept {
    switch (result) {
    case MIMUSB_ERR_TIMEOUT:
        return "The serial write made no progress before the send timeout; nothing was sent and "
               "the state stays pending for a resend.";
    case MIMUSB_ERR_SHORT_TRANSFER:
        return "The serial write stopped part way through a frame; the board drops a frame that "
               "fails its checksum, so delivery cannot be established.";
    case MIMUSB_ERR_NO_DEVICE:
        return "The serial port is gone.";
    default:
        return "The serial write failed.";
    }
}

mimusb_result consume_completion(mimusb_node* node, const char* field) noexcept {
    const mimusb_result result =
        node->completion_result.exchange(MIMUSB_OK, std::memory_order_acq_rel);
    if (result == MIMUSB_OK) {
        node->completion_native_status.store(0, std::memory_order_relaxed);
        node->completion_report_length.store(0U, std::memory_order_relaxed);
        node->completion_report_id.store(0U, std::memory_order_relaxed);
        return MIMUSB_OK;
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
mimusb_result write_report(mimusb_node* node, const std::uint8_t* report, std::size_t length,
                           const char* field, std::uint32_t send_timeout_ms) noexcept {
    mimusb_device* device = node->device;
    // The same path as a generic Node's write, so the wait policy counts the report.
    mimusb::detail::EndpointRuntime* ep =
        mimusb::detail::write_runtime(node, node->config.ep_in.address);
    mimusb_result result = ep == nullptr
                               ? MIMUSB_ERR_INTERNAL
                               : mimusb::detail::endpoint_write(device, public_node_id(node), ep,
                                                                node->config.ep_in.address, report,
                                                                length, send_timeout_ms);
    std::int32_t native = 0;
    switch (result) {
    case MIMUSB_OK:
        break;
    case MIMUSB_ERR_SHORT_TRANSFER:
        native = 0; // some bytes reached the serial driver
        break;
    case MIMUSB_ERR_BUSY: // the endpoint was flushed while waiting: nothing was sent
        result = MIMUSB_ERR_TIMEOUT;
        native = -1;
        break;
    default: // nothing reached the serial driver; the pending state is kept for a resend
        native = -1;
        break;
    }
    if (result == MIMUSB_ERR_NO_DEVICE)
        device->latched.store(MIMUSB_ERR_NO_DEVICE, std::memory_order_release);
    mimusb::detail::transfer_complete(node, result, native);
    return consume_completion(node, field);
}

mimusb_result flush_node(mimusb_node* node, const std::uint32_t timeout_ms,
                         const bool drop_battery) {
    if (timeout_ms == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "timeout_ms", "A nonzero timeout is required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const mimusb_result completion = consume_completion(node, "node.completion");
        if (completion != MIMUSB_OK)
            return completion;
        if (drop_battery)
            node->battery_dirty = false;
        if (!node->dirty && !node->battery_dirty)
            return MIMUSB_OK;
        // A write may wait for what is left of the deadline, and at least a millisecond.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const mimusb_result submit = mimusb_node_submit(
            node, left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 1U);
        if (submit != MIMUSB_OK)
            return submit;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }
    if (!node->dirty && !node->battery_dirty)
        return MIMUSB_OK;
    set_error(MIMUSB_ERR_TIMEOUT, "node.submit_blocking",
              "The report sequence did not finish before the deadline.", 0, 0, public_node_id(node),
              report_id(node), 0U, static_cast<std::uint32_t>(node->spec->layout.wire_bytes));
    return MIMUSB_ERR_TIMEOUT;
}

} // namespace

namespace mimusb::detail {

// Sends a node's pending reports until it is clean, for the close paths.
mimusb_result send_neutral(mimusb_node* node, std::uint32_t timeout_ms) {
    if (node->generic)
        return MIMUSB_OK; // no report state: what the target sees of it ends with the commit
    mimusb_result result = MIMUSB_OK;
    if (node->transfer_inflight.load(std::memory_order_acquire) || touch_frame_in_progress(node)) {
        result = flush_node(node, timeout_ms, true);
        if (result != MIMUSB_OK)
            return result;
    }
    const mimusb_result terminal = consume_completion(node, "node.completion");
    if (terminal != MIMUSB_OK)
        return terminal;
    node->battery_dirty = false;
    if (node->device->link.failed())
        return MIMUSB_OK; // the target keeps whatever it last saw; nothing can be sent
    make_neutral(node);
    if (node->dirty)
        return flush_node(node, timeout_ms, true);
    return MIMUSB_OK;
}

} // namespace mimusb::detail

extern "C" {

mimusb_result MIMUSB_CALL mimusb_node_submit(mimusb_node* node,
                                             const std::uint32_t send_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node.submit",
                  "A live Node attached to a Device and Spec is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (send_timeout_ms == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "send_timeout_ms",
                  "This timeout must be set; it has no default.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (node->spec->kind == MIMUSB_PROFILE_RAW) {
        set_error(MIMUSB_ERR_UNSUPPORTED, "node.submit",
                  "Raw nodes require mimusb_raw_submit() with an explicit report buffer.");
        return MIMUSB_ERR_UNSUPPORTED;
    }
    if (const mimusb_result link = mimusb::detail::link_available(node, "node.submit");
        link != MIMUSB_OK) {
        return link;
    }
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMUSB_ERR_BUSY, "node.submit", "The node already has one report in flight.");
        return MIMUSB_ERR_BUSY;
    }
    const mimusb_result completion = consume_completion(node, "node.completion");
    if (completion != MIMUSB_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    // Input always goes first; the battery report is sent only by a submit
    // that has no input report to send.
    const bool battery_report = !node->dirty && node->battery_dirty;
    if (!node->dirty && !battery_report) {
        node->transfer_inflight.store(false, std::memory_order_release);
        mimusb::detail::clear_error();
        return MIMUSB_OK;
    }
    const mimusb::hid::ReportLayout& layout =
        battery_report ? node->spec->battery_layout : node->spec->layout;
    std::array<std::uint8_t, kMaximumReportBytes> buffer; // only wire_bytes of it is used
    if (layout.wire_bytes > buffer.size() ||
        layout.wire_bytes > node->device->maximum_report_bytes) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(MIMUSB_ERR_OVERFLOW, "node.submit",
                  "The report is longer than the board or the caller allows.");
        return MIMUSB_ERR_OVERFLOW;
    }
    auto* scan_touch =
        battery_report ? nullptr : std::get_if<mimusb::detail::TouchState>(&node->state);
    const auto* scan_config = std::get_if<mimusb::detail::TouchConfig>(&node->spec->config);
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
    const mimusb_result serialized =
        battery_report
            ? mimusb::detail::serialize_battery(node, buffer.data(), buffer.size(), &report_length)
            : mimusb::detail::serialize_node(node, buffer.data(), buffer.size(), &report_length);
    if (serialized != MIMUSB_OK) {
        rollback_scan_time();
        node->transfer_inflight.store(false, std::memory_order_release);
        return serialized;
    }
    if (node->device->config.transport.validate_reports == 1U) {
        const mimusb_result validated = mimusb::detail::validate_serialized_report(
            node->spec, layout, buffer.data(), report_length);
        if (validated != MIMUSB_OK) {
            rollback_scan_time();
            node->transfer_inflight.store(false, std::memory_order_release);
            return validated;
        }
    }
    node->submitted_battery = battery_report;
    node->submitted_non_neutral = !battery_report && has_non_neutral_state(node);
    node->submitted_report_length = report_length;
    node->submitted_report_id = battery_report ? layout.report_id : report_id(node);
    const mimusb_result result =
        write_report(node, buffer.data(), report_length, "node.submit", send_timeout_ms);
    if (result != MIMUSB_OK)
        rollback_scan_time();
    else
        mimusb::detail::clear_error();
    return result;
}
MIMUSB_C_RESULT_CATCH("node.submit")

mimusb_result MIMUSB_CALL mimusb_node_submit_blocking(mimusb_node* node,
                                                      const std::uint32_t timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node.submit_blocking",
                  "A live Node with an attached Device and Spec is required.");
        return MIMUSB_ERR_PARAM;
    }
    const mimusb_result result = flush_node(node, timeout_ms, false);
    return mimusb::detail::finish_result(result, "node.submit_blocking",
                                         "The blocking report sequence failed.");
}
MIMUSB_C_RESULT_CATCH("node.submit_blocking")

mimusb_result MIMUSB_CALL mimusb_raw_submit(mimusb_node* node, const std::uint8_t* report,
                                            const std::size_t length,
                                            const std::uint32_t send_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr ||
        node->spec->kind != MIMUSB_PROFILE_RAW || report == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "raw.submit", "A live raw node and a report are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (send_timeout_ms == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "send_timeout_ms",
                  "This timeout must be set; it has no default.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (const mimusb_result link = mimusb::detail::link_available(node, "raw.submit");
        link != MIMUSB_OK) {
        return link;
    }
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMUSB_ERR_BUSY, "raw.submit", "The raw node already has one report in flight.");
        return MIMUSB_ERR_BUSY;
    }
    const mimusb_result completion = consume_completion(node, "raw.completion");
    if (completion != MIMUSB_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    const auto* raw = std::get_if<mimusb::detail::RawConfig>(&node->spec->config);
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
            set_error(MIMUSB_ERR_PARAM, "raw.report",
                      "The report ID or exact wire length is not accepted by the raw spec.");
            return MIMUSB_ERR_PARAM;
        }
        const std::size_t accepted_index =
            static_cast<std::size_t>(accepted - raw->reports.begin());
        const std::uint8_t valid_mask = raw->trailing_valid_masks[accepted_index];
        if ((report[length - 1U] & static_cast<std::uint8_t>(~valid_mask)) != 0U) {
            node->transfer_inflight.store(false, std::memory_order_release);
            set_error(MIMUSB_ERR_PARAM, "raw.report.trailing_padding",
                      "Unused trailing Input-report bits must be zero.", 0, 0, public_node_id(node),
                      accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U,
                      static_cast<std::uint32_t>(length - 1U), 1U);
            return MIMUSB_ERR_PARAM;
        }
        submitted_id = accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U;
    } else {
        submitted_id = (node->has_report_ids && length > 0U) ? report[0] : 0U;
    }
    if (length > kMaximumReportBytes || length > node->device->maximum_report_bytes) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(MIMUSB_ERR_OVERFLOW, "raw.report",
                  "The report is longer than the board or the caller allows.");
        return MIMUSB_ERR_OVERFLOW;
    }
    node->dirty = true;
    node->submitted_non_neutral = false;
    node->submitted_report_length = length;
    node->submitted_report_id = submitted_id;
    const mimusb_result result = write_report(node, report, length, "raw.submit", send_timeout_ms);
    if (result == MIMUSB_OK)
        mimusb::detail::clear_error();
    return result;
}
MIMUSB_C_RESULT_CATCH("raw.submit")

} // extern "C"
