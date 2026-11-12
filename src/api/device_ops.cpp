// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Device-side internals shared by the C ABI entry points: option validation, interface
 * assignment, the commit sequence and the events the board reports.
 */

#include "api/support.hpp"
#include "transport/link.hpp"
#include "usb/descriptor_composer.hpp"
#include "usb/hid_layer.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace tr = mimhid::transport;

using mimhid::detail::collect_report_ids;
using mimhid::detail::command_error;
using mimhid::detail::commit_device;
using mimhid::detail::CommitWaits;
using mimhid::detail::deliver_event;
using mimhid::detail::describe_board;
using mimhid::detail::kCommandAttempts;
using mimhid::detail::live_nodes;
using mimhid::detail::prepare_device_config;
using mimhid::detail::public_node_id;
using mimhid::detail::read_commit_timeouts;
using mimhid::detail::report_id;
using mimhid::detail::require_timeout;
using mimhid::detail::reserved_is_zero;
using mimhid::detail::saturate_u32;
using mimhid::detail::set_error;
using mimhid::detail::valid_usb_string;
using mimhid::detail::validate_usb_identity;
using mimhid::detail::validate_usb_identity_on_board;

namespace {

bool required_product_policies_nonzero(const mimhid_transport_options& options) noexcept {
    const mimhid_linux_hid_policy& linux_hid = options.linux_hid;
    return linux_hid.descriptor_bytes != 0U && linux_hid.fields_per_report != 0U &&
           linux_hid.global_stack_depth != 0U && linux_hid.report_size_bits != 0U &&
           linux_hid.usages != 0U && linux_hid.report_data_bits != 0U;
}

void put_u16(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

struct ReconfigGuard {
    explicit ReconfigGuard(mimhid_device* d) : device(d) {
        device->reconfiguring.store(true, std::memory_order_release);
    }
    ~ReconfigGuard() { device->reconfiguring.store(false, std::memory_order_release); }
    mimhid_device* device;
};

// Copies as much of text as fits with its terminating NUL; destination is zero-initialized.
template <std::size_t N> void copy_text(char (&destination)[N], const std::string& text) noexcept {
    std::memcpy(destination, text.data(), std::min(text.size(), N - 1U));
}

} // namespace

namespace mimhid::detail {

bool utf16_from_utf8(const char* text, std::u16string* units) {
    units->clear();
    if (text == nullptr)
        return true;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p != 0U) {
        std::uint32_t code = 0U;
        unsigned extra = 0U;
        std::uint32_t least = 0U;
        if (*p < 0x80U) {
            code = *p;
        } else if (*p >= 0xC2U && *p <= 0xDFU) {
            code = *p & 0x1FU;
            extra = 1U;
            least = 0x80U;
        } else if (*p >= 0xE0U && *p <= 0xEFU) {
            code = *p & 0x0FU;
            extra = 2U;
            least = 0x800U;
        } else if (*p >= 0xF0U && *p <= 0xF4U) {
            code = *p & 0x07U;
            extra = 3U;
            least = 0x10000U;
        } else {
            return false;
        }
        ++p;
        for (unsigned i = 0U; i < extra; ++i, ++p) {
            if ((*p & 0xC0U) != 0x80U)
                return false;
            code = (code << 6U) | (*p & 0x3FU);
        }
        // No overlong forms, no surrogates, nothing above U+10FFFF, no control characters.
        if (code < least || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU) ||
            code < 0x20U || (code >= 0x7FU && code <= 0x9FU))
            return false;
        if (code >= 0x10000U) {
            code -= 0x10000U;
            units->push_back(static_cast<char16_t>(0xD800U + (code >> 10U)));
            units->push_back(static_cast<char16_t>(0xDC00U + (code & 0x3FFU)));
        } else {
            units->push_back(static_cast<char16_t>(code));
        }
        if (units->size() > MIMHID_STRING_MAX)
            return false;
    }
    return true;
}

bool valid_usb_string(const char* text) noexcept {
    try {
        std::u16string units;
        return utf16_from_utf8(text, &units);
    } catch (...) {
        return false;
    }
}

std::uint32_t saturate_u32(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

// Room kept for later releases: a nonzero value is refused, so that an old binary and a new
// header never read it differently.
bool reserved_is_zero(const std::uint32_t reserved, const char* field) noexcept {
    if (reserved != 0U) {
        set_error(MIMHID_ERR_PARAM, field, "The reserved field must be zero.");
        return false;
    }
    return true;
}

mimhid_result validate_usb_identity(const mimhid_usb_device& usb_device,
                                    const mimhid_usb_config& usb_config) noexcept {
    const std::pair<std::uint32_t, const char*> required_usb_device[] = {
        {usb_device.vendor_id, "usb_device.vendor_id"},
        {usb_device.language_id, "usb_device.language_id"},
        {usb_device.bcd_usb, "usb_device.bcd_usb"},
        {usb_device.ep0_max_packet_size, "usb_device.ep0_max_packet_size"},
    };
    for (const auto& [value, field] : required_usb_device) {
        if (value == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, field, "This value is required.");
            return MIMHID_ERR_UNSET_FIELD;
        }
    }
    if (usb_device.ep0_max_packet_size != 8U && usb_device.ep0_max_packet_size != 16U &&
        usb_device.ep0_max_packet_size != 32U && usb_device.ep0_max_packet_size != 64U) {
        set_error(MIMHID_ERR_PARAM, "usb_device.ep0_max_packet_size",
                  "ep0_max_packet_size must be 8, 16, 32, or 64.");
        return MIMHID_ERR_PARAM;
    }
    if (usb_device.device_class == 0U && usb_device.device_subclass != 0U) {
        // USB 2.0 9.6.1: "If the bDeviceClass field is reset to zero, this field must also be
        // reset to zero."
        set_error(MIMHID_ERR_PARAM, "usb_device.device_subclass",
                  "device_subclass must be zero when device_class is zero (USB 2.0 9.6.1).");
        return MIMHID_ERR_PARAM;
    }
    if (!mimhid::detail::valid_boolean(usb_device.serial_from_chip_id)) {
        set_error(MIMHID_ERR_PARAM, "usb_device.serial_from_chip_id",
                  "serial_from_chip_id must be zero or one.");
        return MIMHID_ERR_PARAM;
    }
    for (const char* text : {usb_device.manufacturer, usb_device.product, usb_device.serial}) {
        if (!valid_usb_string(text)) {
            set_error(MIMHID_ERR_PARAM, "usb_device.strings",
                      "A string must be UTF-8 without control characters, at most 126 UTF-16 code "
                      "units.");
            return MIMHID_ERR_PARAM;
        }
    }

    if (usb_config.configuration_value == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_config.configuration_value",
                  "configuration_value must be non-zero (e.g. 1).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (usb_config.configuration_value != 1U) {
        // USB 2.0 9.6.3 allows any value 1 to 255, but the board has one configuration and its
        // USB stack maps SET_CONFIGURATION(n) to the n-th one.
        set_error(MIMHID_ERR_UNSUPPORTED, "usb_config.configuration_value",
                  "The board has one configuration, and it is selected by the value 1.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if ((usb_config.bm_attributes & 0x80U) == 0U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bm_attributes",
                  "bm_attributes bit 7 must be set (0x80).");
        return MIMHID_ERR_PARAM;
    }
    if ((usb_config.bm_attributes & 0x1FU) != 0U) {
        // USB 2.0 9.6.3: D4..0 are reserved and reset to zero.
        set_error(MIMHID_ERR_PARAM, "usb_config.bm_attributes",
                  "bm_attributes bits 4..0 are reserved and must be zero (USB 2.0 9.6.3).");
        return MIMHID_ERR_PARAM;
    }
    if (usb_config.bmax_power > 250U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bmax_power",
                  "bmax_power must be at most 250 (500 mA).");
        return MIMHID_ERR_PARAM;
    }
    if (!valid_usb_string(usb_config.string)) {
        set_error(
            MIMHID_ERR_PARAM, "usb_config.string",
            "A string must be UTF-8 without control characters, at most 126 UTF-16 code units.");
        return MIMHID_ERR_PARAM;
    }

    return MIMHID_OK;
}

// The rules of the Device descriptor that depend on the speed the board runs at.
mimhid_result validate_usb_identity_on_board(const tr::BoardInfo& board,
                                             const mimhid_usb_device& usb_device) noexcept {
    if (board.usb_high_speed && usb_device.ep0_max_packet_size != 64U) {
        // USB 2.0 9.6.1: at high speed bMaxPacketSize0 is 64 and nothing else.
        set_error(MIMHID_ERR_PARAM, "usb_device.ep0_max_packet_size",
                  "A high-speed board has a 64-byte control endpoint (USB 2.0 9.6.1).");
        return MIMHID_ERR_PARAM;
    }
    return MIMHID_OK;
}

mimhid_result prepare_device_config(const mimhid_device_config* config,
                                    mimhid_device_config* effective) noexcept {
    if (!mimhid::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*config)),
                                      "device_config")) {
        return MIMHID_ERR_PARAM;
    }
    if (!reserved_is_zero(config->reserved, "device_config.reserved"))
        return MIMHID_ERR_PARAM;
    *effective = *config;

    // Transport required fields
    const std::pair<std::uint32_t, const char*> required_transport[] = {
        {effective->transport.baud_rate, "transport.baud_rate"},
        {effective->transport.handshake_timeout_ms, "transport.handshake_timeout_ms"},
        {effective->transport.maximum_report_bytes, "transport.maximum_report_bytes"},
        {static_cast<std::uint32_t>(effective->transport.log_level), "transport.log_level"},
    };
    for (const auto& [value, field] : required_transport) {
        if (value == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, field, "This value is required.");
            return MIMHID_ERR_UNSET_FIELD;
        }
    }
    if (!required_product_policies_nonzero(effective->transport)) {
        set_error(MIMHID_ERR_UNSET_FIELD, "transport.linux_hid",
                  "Every linux_hid limit is required.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (!mimhid::detail::valid_boolean(effective->transport.validate_reports) ||
        !mimhid::detail::valid_boolean(effective->transport.keep_attached) ||
        effective->transport.log_level < MIMHID_LOG_DISABLED ||
        effective->transport.log_level > MIMHID_LOG_TRACE ||
        (effective->transport.log_level != MIMHID_LOG_DISABLED &&
         effective->transport.log_sink == nullptr)) {
        set_error(MIMHID_ERR_PARAM, "transport",
                  "Every flag must be exactly zero or one, log_level a defined value, and a "
                  "log_level other than DISABLED needs a log_sink.");
        return MIMHID_ERR_PARAM;
    }

    return validate_usb_identity(effective->usb_device, effective->usb_config);
}

// Collects the Report IDs a descriptor declares (short items only; validated descriptors).
void collect_report_ids(const std::vector<std::uint8_t>& descriptor, std::bitset<256>* ids,
                        bool* has_ids) {
    constexpr std::uint8_t kTypeGlobal = 1U;
    constexpr std::uint8_t kGlobalReportId = 8U;
    std::size_t pos = 0U;
    while (pos < descriptor.size()) {
        const std::uint8_t prefix = descriptor[pos];
        if (prefix == 0xFEU) { // long item: length in the next byte
            pos += pos + 1U < descriptor.size() ? 3U + descriptor[pos + 1U] : descriptor.size();
            continue;
        }
        const std::uint8_t size_code = prefix & 0x03U;
        const std::size_t size = size_code == 3U ? 4U : size_code;
        const std::uint8_t type = (prefix >> 2U) & 0x03U;
        const std::uint8_t tag = (prefix >> 4U) & 0x0FU;
        if (type == kTypeGlobal && tag == kGlobalReportId && size >= 1U &&
            pos + 1U < descriptor.size()) {
            ids->set(descriptor[pos + 1U]);
            *has_ids = true;
        }
        pos += 1U + size;
    }
}

mimhid_result command_error(const tr::CommandResult& result, const char* field, const char* reason,
                            std::uint8_t frame_type, std::uint16_t node_id) noexcept {
    mimhid_result code = MIMHID_ERR_INTERNAL;
    switch (result.status) {
    case tr::CommandStatus::ack:
        return MIMHID_OK;
    case tr::CommandStatus::nak:
        switch (result.nak_code) {
        case MIMHID_NAK_PARAM:
            code = MIMHID_ERR_PARAM;
            break;
        case MIMHID_NAK_OVERFLOW:
            code = MIMHID_ERR_OVERFLOW;
            break;
        case MIMHID_NAK_STATE:
            code = MIMHID_ERR_BUSY;
            break;
        case MIMHID_NAK_DESCRIPTOR:
            code = MIMHID_ERR_DESCRIPTOR_REJECTED;
            break;
        case MIMHID_NAK_UNSUPPORTED:
            code = MIMHID_ERR_UNSUPPORTED;
            break;
        case MIMHID_NAK_BUSY:
            code = MIMHID_ERR_BUSY;
            break;
        default:
            code = MIMHID_ERR_INTERNAL;
            break;
        }
        set_error(code, field, reason, result.nak_code, frame_type, node_id);
        return code;
    case tr::CommandStatus::timeout:
        code = MIMHID_ERR_TIMEOUT;
        break;
    case tr::CommandStatus::closed:
        code = MIMHID_ERR_NO_DEVICE;
        break;
    case tr::CommandStatus::io_error:
        code = MIMHID_ERR_IO;
        break;
    case tr::CommandStatus::short_write:
        code = MIMHID_ERR_SHORT_TRANSFER;
        break;
    }
    set_error(code, field, reason, result.os_error, frame_type, node_id);
    return code;
}

std::vector<mimhid_node*> live_nodes(mimhid_device* device) {
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    std::vector<mimhid_node*> nodes;
    for (mimhid_node* node : device->slots) {
        if (node != nullptr && !node->closed)
            nodes.push_back(node);
    }
    return nodes;
}

bool free_node_id(mimhid_device* device, std::uint16_t* node_id) {
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    for (std::size_t i = 0U; i < device->slots.size(); ++i) {
        if (device->slots[i] == nullptr) {
            *node_id = static_cast<std::uint16_t>(i);
            return true;
        }
    }
    if (device->slots.size() >= kMaximumNodes)
        return false;
    *node_id = static_cast<std::uint16_t>(device->slots.size());
    return true;
}

void publish_node(mimhid_device* device, mimhid_node* node) {
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    if (device->slots.size() <= node->node_id)
        device->slots.resize(static_cast<std::size_t>(node->node_id) + 1U, nullptr);
    device->slots[node->node_id] = node;
}

void unpublish_node(mimhid_device* device, const mimhid_node* node) {
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    if (node->node_id < device->slots.size() && device->slots[node->node_id] == node)
        device->slots[node->node_id] = nullptr;
}

// A wait the caller passed to a call has no default: zero is refused.
mimhid_result require_timeout(const std::uint32_t timeout_ms, const char* field) noexcept {
    if (timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, field, "This timeout must be set; it has no default.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    return MIMHID_OK;
}

mimhid_result read_commit_timeouts(const mimhid_commit_timeouts* timeouts,
                                   CommitWaits* waits) noexcept {
    if (!mimhid::detail::valid_struct(timeouts, timeouts == nullptr ? 0U : timeouts->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*timeouts)), "timeouts")) {
        return MIMHID_ERR_PARAM;
    }
    if (!reserved_is_zero(timeouts->reserved, "timeouts.reserved"))
        return MIMHID_ERR_PARAM;
    if (const mimhid_result r =
            require_timeout(timeouts->control_timeout_ms, "timeouts.control_timeout_ms");
        r != MIMHID_OK)
        return r;
    if (const mimhid_result r =
            require_timeout(timeouts->reconnect_timeout_ms, "timeouts.reconnect_timeout_ms");
        r != MIMHID_OK)
        return r;
    waits->control = timeouts->control_timeout_ms;
    waits->reconnect = timeouts->reconnect_timeout_ms;
    return MIMHID_OK;
}

void reset_endpoint_counts(mimhid_device* device) {
    {
        // nodes_mutex is held for the whole walk, and taken before endpoint_mutex: the reader
        // thread calls this, and a Node is deleted only after unpublish_node took nodes_mutex, so
        // no Node walked here can go away under it.
        std::lock_guard<std::mutex> nodes(device->nodes_mutex);
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        for (mimhid_node* node : device->slots) {
            if (node == nullptr || node->closed)
                continue;
            for (EndpointRuntime& ep : node->in_endpoints) {
                ep.outstanding = 0U;
                ++ep.epoch;
            }
        }
        for (EndpointRuntime& ep : device->bytes.in_endpoints) {
            ep.outstanding = 0U;
            ++ep.epoch;
        }
    }
    device->endpoint_cv.notify_all();
}

// What the board takes, as the composer wants it.
usb::ComposerLimits composer_limits(const mimhid_device* device) {
    const tr::BoardInfo& board = device->board;
    usb::ComposerLimits lim;
    lim.max_interfaces = board.max_interfaces_any;
    lim.max_endpoint_number = board.max_endpoint_number;
    lim.max_endpoint_bytes = board.max_endpoint_bytes;
    lim.max_config_bytes = board.max_config_descriptor_bytes;
    lim.max_bos_bytes = board.max_bos_bytes;
    lim.max_strings = board.max_strings;
    lim.endpoint_queue_slots = board.endpoint_queue_slots;
    lim.max_transfer_bytes = board.max_transfer_bytes;
    lim.transfer_types = board.transfer_types;
    lim.high_speed = board.usb_high_speed;
    lim.has_unique_id = board.has_unique_id;
    return lim;
}

void fill_device_strings(const mimhid_device* device, usb::ComposerInput* input) {
    input->device = device->config.usb_device;
    input->config = device->config.usb_config;
    input->limits = composer_limits(device);
    utf16_from_utf8(device->manufacturer.c_str(), &input->manufacturer);
    utf16_from_utf8(device->product.c_str(), &input->product);
    utf16_from_utf8(device->serial.c_str(), &input->serial);
    utf16_from_utf8(device->configuration.c_str(), &input->configuration);
    for (const auto& entry : device->user_strings)
        utf16_from_utf8(entry.second.c_str(), &input->user_strings[entry.first]);
}

// The interfaces of the generic Nodes and, joined per interface, those of the HID Nodes.
mimhid_result groups_of_nodes(const std::vector<mimhid_node*>& nodes,
                              std::vector<usb::ComposerGroup>* groups,
                              std::vector<usb::HidNodeInput>* hid_inputs,
                              usb::ComposerError* error) {
    std::vector<usb::HidNodeInput>& hid = *hid_inputs;
    hid.clear();
    for (const mimhid_node* node : nodes) {
        if (node->generic) {
            usb::ComposerGroup group;
            group.node_id = public_node_id(node);
            group.association = node->association;
            for (const GenericInterface& itf : node->interfaces) {
                usb::ComposerInterface made;
                made.number = itf.number;
                made.class_code = itf.class_code;
                made.subclass = itf.subclass;
                made.protocol = itf.protocol;
                utf16_from_utf8(itf.name.c_str(), &made.name);
                made.class_descriptors = itf.class_descriptors;
                made.endpoints = itf.endpoints;
                group.interfaces.push_back(std::move(made));
            }
            groups->push_back(std::move(group));
        } else {
            usb::HidNodeInput in;
            in.config = node->config;
            utf16_from_utf8(node->interface_string.c_str(), &in.name);
            in.report_descriptor = &node->spec->descriptor;
            in.has_report_ids = node->has_report_ids;
            in.report_ids = node->report_ids;
            in.node_id = public_node_id(node);
            for (const mimhid_node::FeatureEntry& feature : node->features)
                in.features.push_back(usb::HidNodeInput::Feature{feature.report_id, feature.data});
            hid.push_back(std::move(in));
        }
    }
    std::vector<usb::ComposerGroup> hid_groups;
    if (const mimhid_result r = usb::compose_hid_groups(hid, &hid_groups, error); r != MIMHID_OK)
        return r;
    for (usb::ComposerGroup& g : hid_groups)
        groups->push_back(std::move(g));
    return MIMHID_OK;
}

mimhid_result precheck_groups(const mimhid_device* device, std::vector<usb::ComposerGroup> groups) {
    usb::ComposerInput input;
    fill_device_strings(device, &input);
    input.groups = std::move(groups);
    input.partial = true;
    usb::Composed composed;
    usb::ComposerError error;
    if (const mimhid_result r = usb::compose_descriptors(input, &composed, &error);
        r != MIMHID_OK) {
        set_error(r, error.field, error.reason, 0, 0, error.node_id, 0, error.offset, error.length);
        return r;
    }
    return MIMHID_OK;
}

// ---- control rules --------------------------------------------------------------------------

namespace {

void put_u16_le(std::vector<std::uint8_t>* out, const std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

// A command that finds the board answering from the rule it touches is sent again a few times:
// the board says BUSY until the transfer it is answering is over, which takes milliseconds.
tr::CommandResult command_retry_busy(mimhid_device* device, const std::uint8_t type,
                                     const std::vector<std::uint8_t>& payload,
                                     const std::uint32_t control) {
    constexpr int kBusyTries = 50;
    tr::CommandResult result;
    for (int tries = 0; tries < kBusyTries; ++tries) {
        result =
            device->link.command(type, payload.data(), payload.size(), control, kCommandAttempts);
        if (result.status != tr::CommandStatus::nak || result.nak_code != MIMHID_NAK_BUSY)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return result;
}

// The RULE frame of one rule (docs/PROTOCOL.md "Control rules").
std::vector<std::uint8_t> rule_payload(const std::size_t index, const usb::BoardRule& entry) {
    const mimhid_control_rule& r = entry.rule;
    std::vector<std::uint8_t> p{static_cast<std::uint8_t>(index), r.request_type_mask,
                                r.request_type_value, r.request_mask, r.request_value};
    put_u16_le(&p, r.value_mask);
    put_u16_le(&p, r.value_value);
    put_u16_le(&p, r.index_mask);
    put_u16_le(&p, r.index_value);
    p.push_back(static_cast<std::uint8_t>(r.action));
    switch (r.action) {
    case MIMHID_CONTROL_ANSWER:
        // The capacity the rule was given and the bytes it holds now (at most the capacity).
        put_u16_le(&p, entry.capacity);
        put_u16_le(&p, static_cast<std::uint32_t>(entry.data.size()));
        break;
    case MIMHID_CONTROL_ANSWER_LAST_SENT:
        p.push_back(r.endpoint_address);
        p.push_back(r.first_byte_filter);
        p.push_back(r.first_byte);
        break;
    case MIMHID_CONTROL_ASK:
        put_u16_le(&p, r.timeout_ms);
        // MIMHID_CONTROL_ACTION_STALL and _DROP are the wire's MIMHID_REPLY_STALL and _DROP plus 1.
        p.push_back(static_cast<std::uint8_t>(r.when_late - 1));
        break;
    default:
        break;
    }
    return p;
}

// Stages the whole table on the board: for each rule a RULE frame and, for an ANSWER, its bytes as
// a descriptor of the kind ANSWER. The board holds it as the staged table until a COMMIT or
// RULES_APPLY makes it the active one.
mimhid_result stage_rule_table(mimhid_device* device, const std::vector<usb::BoardRule>& table,
                               const std::uint32_t control) {
    const auto send = [&](const std::uint8_t type, const std::vector<std::uint8_t>& payload,
                          const char* reason) {
        return command_error(
            device->link.command(type, payload.data(), payload.size(), control, kCommandAttempts),
            "device.control_rules", reason, type);
    };
    for (std::size_t i = 0U; i < table.size(); ++i) {
        mimhid_result r = send(MIMHID_T_RULE, rule_payload(i, table[i]),
                               "The board did not accept a control rule.");
        const std::vector<std::uint8_t>& bytes = table[i].data;
        if (r != MIMHID_OK)
            return r;
        if (table[i].rule.action != MIMHID_CONTROL_ANSWER || bytes.empty())
            continue;
        std::vector<std::uint8_t> begin{MIMHID_DESC_ANSWER, static_cast<std::uint8_t>(i)};
        put_u16_le(&begin, 0U);
        put_u16_le(&begin, static_cast<std::uint32_t>(bytes.size()));
        begin.push_back(0U);
        r = send(MIMHID_T_DESC_BEGIN, begin, "The board did not accept the bytes of an answer.");
        for (std::size_t offset = 0U; r == MIMHID_OK && offset < bytes.size();
             offset += MIMHID_DESC_DATA_CHUNK) {
            const std::size_t chunk =
                std::min<std::size_t>(MIMHID_DESC_DATA_CHUNK, bytes.size() - offset);
            std::vector<std::uint8_t> data{MIMHID_DESC_ANSWER, static_cast<std::uint8_t>(i)};
            put_u16_le(&data, 0U);
            put_u16_le(&data, static_cast<std::uint32_t>(offset));
            data.insert(data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            r = send(MIMHID_T_DESC_DATA, data, "The board did not accept the bytes of an answer.");
        }
        if (r != MIMHID_OK)
            return r;
    }
    return MIMHID_OK;
}

// What the board holds in the table, checked before anything is sent.
mimhid_result check_rule_table(const mimhid_device* device,
                               const std::vector<usb::BoardRule>& table) {
    const tr::BoardInfo& board = device->board;
    std::size_t bytes = 0U;
    for (const usb::BoardRule& entry : table)
        bytes += entry.rule.action == MIMHID_CONTROL_ANSWER ? entry.capacity : 0U;
    if (table.size() > board.max_control_rules) {
        set_error(MIMHID_ERR_OVERFLOW, "device.control_rules",
                  "The control rules of the Nodes and of the Device exceed what the board holds "
                  "(mimhid_board_limits.max_control_rules).",
                  0, 0, 0, 0, board.max_control_rules, static_cast<std::uint32_t>(table.size()));
        return MIMHID_ERR_OVERFLOW;
    }
    if (bytes > board.max_answer_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "device.control_rules",
                  "The answers of the control rules exceed the board's answer memory "
                  "(mimhid_board_limits.max_answer_bytes).",
                  0, 0, 0, 0, board.max_answer_bytes, static_cast<std::uint32_t>(bytes));
        return MIMHID_ERR_OVERFLOW;
    }
    return MIMHID_OK;
}

// The table the board is to hold: the rules of the Nodes opened with mimhid_node_open in node
// order, those the library writes for the profile Nodes, then the Device's own. What the target
// has set (idle rate, protocol) is kept from the table the board holds now.
mimhid_result build_rule_table(mimhid_device* device, const std::vector<mimhid_node*>& nodes,
                               const std::vector<usb::HidNodeInput>& hid,
                               std::vector<usb::BoardRule>* table) {
    table->clear();
    // An ANSWER rule keeps the capacity it was given however its bytes change, so the board
    // reserves the same room each time the table is sent and a shortened answer can grow again.
    for (mimhid_node* node : nodes) {
        if (node->generic)
            table->insert(table->end(), node->rules.begin(), node->rules.end());
    }
    std::vector<usb::BoardRule> library = usb::compose_hid_rules(hid);
    {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        for (usb::BoardRule& rule : library) {
            if (rule.role != usb::BoardRule::Role::hid_get_idle &&
                rule.role != usb::BoardRule::Role::hid_get_protocol)
                continue;
            for (const usb::BoardRule& held : device->table) {
                if (held.role == rule.role && held.iface == rule.iface && !held.data.empty())
                    rule.data = held.data;
            }
        }
    }
    table->insert(table->end(), std::make_move_iterator(library.begin()),
                  std::make_move_iterator(library.end()));
    table->insert(table->end(), device->device_rules.begin(), device->device_rules.end());
    return check_rule_table(device, *table);
}

// The request matches the rule: each field with its mask equals its value.
bool rule_matches(const mimhid_control_rule& r, const std::uint8_t* setup) {
    const std::uint16_t value = static_cast<std::uint16_t>(setup[2] | (setup[3] << 8));
    const std::uint16_t index = static_cast<std::uint16_t>(setup[4] | (setup[5] << 8));
    return (setup[0] & r.request_type_mask) == r.request_type_value &&
           (setup[1] & r.request_mask) == r.request_value &&
           (value & r.value_mask) == r.value_value && (index & r.index_mask) == r.index_value;
}

// The reader thread cannot wait for the board, so the change of the bytes of an answer is done by
// this thread, which sends what the table holds for the rule when it gets to it.
void updater_loop(mimhid_device* device) {
    for (;;) {
        mimhid_device::Update update;
        {
            std::unique_lock<std::mutex> lock(device->update_mutex);
            device->update_cv.wait(
                lock, [&] { return device->updater_stop || !device->updates.empty(); });
            if (device->updater_stop)
                return;
            update = device->updates.front();
            device->updates.pop_front();
        }
        if (device->link.failed() || device->reconfiguring.load(std::memory_order_acquire))
            continue;
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            if (update.index >= device->table.size())
                continue;
            const usb::BoardRule& entry = device->table[update.index];
            if (entry.role != usb::BoardRule::Role::hid_get_idle &&
                entry.role != usb::BoardRule::Role::hid_get_protocol)
                continue; // the table changed since: the commit sent the values it had
            payload.push_back(static_cast<std::uint8_t>(update.index));
            payload.insert(payload.end(), entry.data.begin(), entry.data.end());
        }
        const std::uint32_t timeout = device->rule_timeout_ms.load(std::memory_order_acquire);
        (void)command_retry_busy(device, MIMHID_T_RULE_DATA, payload,
                                 timeout == 0U ? 1000U : timeout);
    }
}

} // namespace

void start_rule_updater(mimhid_device* device) {
    std::lock_guard<std::mutex> lock(device->update_mutex);
    if (!device->updater.joinable()) {
        device->updater_stop = false;
        device->updater = std::thread(updater_loop, device);
    }
}

void stop_rule_updater(mimhid_device* device) noexcept {
    {
        std::lock_guard<std::mutex> lock(device->update_mutex);
        device->updater_stop = true;
    }
    device->update_cv.notify_all();
    if (device->updater.joinable())
        device->updater.join();
}

// Sets the one byte a GET_IDLE or GET_PROTOCOL rule of `iface` answers with, and has it sent.
// The reader thread calls this.
static void set_hid_answer(mimhid_device* device, const usb::BoardRule::Role role,
                           // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                           const std::uint8_t iface, const std::uint8_t value) {
    std::uint16_t found = 0xFFFFU;
    {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        for (std::size_t i = 0U; i < device->table.size(); ++i) {
            usb::BoardRule& entry = device->table[i];
            if (entry.role == role && entry.iface == iface && entry.data.size() == 1U) {
                entry.data[0] = value;
                found = static_cast<std::uint16_t>(i);
            }
        }
    }
    if (found == 0xFFFFU)
        return;
    {
        std::lock_guard<std::mutex> lock(device->update_mutex);
        device->updates.push_back(mimhid_device::Update{found, value});
    }
    device->update_cv.notify_one();
}

// HID 1.11 7.2.4 and 7.2.6: the idle rate and the protocol start again from their initial values
// each time the target configures the device.
static void reset_hid_answers(mimhid_device* device) {
    std::vector<std::pair<usb::BoardRule::Role, std::uint8_t>> changed;
    {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        for (const usb::BoardRule& entry : device->table) {
            const std::uint8_t initial =
                entry.role == usb::BoardRule::Role::hid_get_protocol ? 1U : 0U;
            if ((entry.role == usb::BoardRule::Role::hid_get_idle ||
                 entry.role == usb::BoardRule::Role::hid_get_protocol) &&
                entry.data.size() == 1U && entry.data[0] != initial)
                changed.emplace_back(entry.role, entry.iface);
        }
    }
    for (const auto& [role, iface] : changed)
        set_hid_answer(device, role, iface,
                       role == usb::BoardRule::Role::hid_get_protocol ? 1U : 0U);
}

mimhid_result copy_control_rules(const mimhid_device* device, const mimhid_control_rule* rules,
                                 const std::size_t count, const char* field,
                                 std::vector<usb::BoardRule>* out) {
    out->clear();
    if (count != 0U && rules == nullptr) {
        set_error(MIMHID_ERR_PARAM, field, "rules is null with a nonzero rule_count.");
        return MIMHID_ERR_PARAM;
    }
    std::size_t bytes = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        const mimhid_control_rule& r = rules[i];
        const auto fail = [&](const mimhid_result result, const char* reason) {
            set_error(result, field, reason, 0, 0, 0, 0, static_cast<std::uint32_t>(i), 0);
            return result;
        };
        if (r.action == 0)
            return fail(MIMHID_ERR_UNSET_FIELD, "action is required.");
        if (r.action < MIMHID_CONTROL_ANSWER || r.action > MIMHID_CONTROL_STALL)
            return fail(MIMHID_ERR_PARAM, "An unknown rule action.");
        if ((r.request_type_value & ~r.request_type_mask) != 0 ||
            (r.request_value & ~r.request_mask) != 0 || (r.value_value & ~r.value_mask) != 0 ||
            (r.index_value & ~r.index_mask) != 0)
            return fail(MIMHID_ERR_PARAM, "A value outside its mask never matches.");
        if (r.reserved8 != 0U)
            return fail(MIMHID_ERR_PARAM, "The reserved field must be zero.");
        const bool answer = r.action == MIMHID_CONTROL_ANSWER;
        const bool last = r.action == MIMHID_CONTROL_ANSWER_LAST_SENT;
        const bool ask = r.action == MIMHID_CONTROL_ASK;
        if (!answer && (r.data != nullptr || r.length != 0U))
            return fail(MIMHID_ERR_PARAM, "data and length belong to an ANSWER rule only.");
        if (!last && (r.endpoint_address != 0U || r.first_byte_filter != 0U || r.first_byte != 0U))
            return fail(MIMHID_ERR_PARAM,
                        "endpoint_address and the first byte belong to ANSWER_LAST_SENT only.");
        if (!ask && (r.timeout_ms != 0U || r.when_late != 0))
            return fail(MIMHID_ERR_PARAM, "timeout_ms and when_late belong to an ASK rule only.");
        if (answer || last) {
            // USB 2.0 9.3.1: bit 7 of bmRequestType is the direction; the answer is data stage.
            if ((r.request_type_mask & 0x80U) == 0U || (r.request_type_value & 0x80U) == 0U)
                return fail(MIMHID_ERR_PARAM,
                            "An ANSWER or ANSWER_LAST_SENT rule matches device-to-host requests "
                            "only: bit 7 of request_type_mask and _value.");
        }
        if (answer) {
            if (r.data == nullptr && r.length != 0U)
                return fail(MIMHID_ERR_PARAM, "data is null with a nonzero length.");
            if (r.length > 0xFFFFU)
                return fail(MIMHID_ERR_OVERFLOW, "An answer is at most 65535 bytes.");
            bytes += r.length;
            if (bytes > device->board.max_answer_bytes)
                return fail(MIMHID_ERR_OVERFLOW, "The answers exceed the board's answer memory "
                                                 "(mimhid_board_limits.max_answer_bytes).");
        }
        if (last) {
            // USB 2.0 9.6.6: an IN endpoint address has bit 7 set, bits 6..4 clear, a number.
            if ((r.endpoint_address & 0x80U) == 0U || (r.endpoint_address & 0x70U) != 0U ||
                (r.endpoint_address & 0x0FU) == 0U || r.first_byte_filter > 1U)
                return fail(MIMHID_ERR_PARAM,
                            "endpoint_address is an IN endpoint address (0x81 to 0x8F) and "
                            "first_byte_filter 0 or 1.");
        }
        if (ask) {
            if (r.timeout_ms == 0U || r.when_late == 0)
                return fail(MIMHID_ERR_UNSET_FIELD, "An ASK rule needs timeout_ms and when_late.");
            if (r.timeout_ms > MIMHID_CONTROL_MAX_TIMEOUT_MS ||
                (r.when_late != MIMHID_CONTROL_ACTION_STALL &&
                 r.when_late != MIMHID_CONTROL_ACTION_DROP))
                return fail(MIMHID_ERR_PARAM,
                            "timeout_ms is 1 to MIMHID_CONTROL_MAX_TIMEOUT_MS and when_late STALL "
                            "or DROP.");
        }
        usb::BoardRule entry;
        entry.rule = r;
        entry.rule.data = nullptr;
        if (answer && r.length != 0U)
            entry.data.assign(r.data, r.data + r.length);
        entry.capacity = static_cast<std::uint16_t>(answer ? r.length : 0U);
        out->push_back(std::move(entry));
    }
    return MIMHID_OK;
}

mimhid_result rebuild_rules(mimhid_device* device, const std::uint32_t control_timeout_ms) {
    const std::vector<mimhid_node*> nodes = live_nodes(device);
    std::vector<usb::ComposerGroup> groups;
    std::vector<usb::HidNodeInput> hid;
    usb::ComposerError error;
    if (const mimhid_result made = groups_of_nodes(nodes, &groups, &hid, &error);
        made != MIMHID_OK) {
        set_error(made, error.field, error.reason, 0, 0, error.node_id, 0, error.offset,
                  error.length);
        return made;
    }
    std::vector<usb::BoardRule> table;
    if (const mimhid_result built = build_rule_table(device, nodes, hid, &table);
        built != MIMHID_OK)
        return built;
    if (const mimhid_result staged = stage_rule_table(device, table, control_timeout_ms);
        staged != MIMHID_OK)
        return staged;
    const std::vector<std::uint8_t> apply{device->report_all ? std::uint8_t{1} : std::uint8_t{0},
                                          static_cast<std::uint8_t>(table.size())};
    if (const mimhid_result applied = command_error(
            command_retry_busy(device, MIMHID_T_RULES_APPLY, apply, control_timeout_ms),
            "device.control_rules", "The board did not apply the control rules.",
            MIMHID_T_RULES_APPLY);
        applied != MIMHID_OK)
        return applied;
    {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        device->table = std::move(table);
    }
    device->control_routed.store(device->report_all || !device->device_rules.empty(),
                                 std::memory_order_release);
    return MIMHID_OK;
}

mimhid_result send_rule_data(mimhid_device* device, const std::size_t table_index,
                             // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                             const std::uint8_t* data, const std::size_t length,
                             const std::uint32_t control_timeout_ms) {
    std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(table_index)};
    if (length != 0U)
        payload.insert(payload.end(), data, data + length);
    return command_error(
        command_retry_busy(device, MIMHID_T_RULE_DATA, payload, control_timeout_ms),
        "device.rule_data", "The board did not accept the bytes of the answer.",
        MIMHID_T_RULE_DATA);
}

// Changes the bytes of an ANSWER rule of the Device (node null) or of a Node's own rules: kept for
// the next table, and sent to the board when it holds the table now.
mimhid_result change_rule_data(mimhid_device* device, mimhid_node* node,
                               const std::size_t rule_index, const std::uint8_t* data,
                               // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                               const std::size_t length, const std::uint32_t control_timeout_ms) {
    std::vector<usb::BoardRule>& own = node != nullptr ? node->rules : device->device_rules;
    if (rule_index >= own.size() || own[rule_index].rule.action != MIMHID_CONTROL_ANSWER) {
        set_error(MIMHID_ERR_PARAM, "rule_index", "There is no ANSWER rule with this number.");
        return MIMHID_ERR_PARAM;
    }
    if (data == nullptr && length != 0U) {
        set_error(MIMHID_ERR_PARAM, "data", "data is null with a nonzero length.");
        return MIMHID_ERR_PARAM;
    }
    if (length > own[rule_index].capacity) {
        set_error(MIMHID_ERR_PARAM, "length",
                  "length is more than the rule's capacity, the length of its data when the rule "
                  "was set.");
        return MIMHID_ERR_PARAM;
    }
    std::vector<std::uint8_t> bytes;
    if (length != 0U)
        bytes.assign(data, data + length);
    const bool live =
        device->configured && !device->batch_open && (node == nullptr || !node->staged_only);
    // Where the rule is in the table the board holds: the Nodes' rules come first in node order.
    std::size_t index = rule_index;
    if (node == nullptr) {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        index = device->table.size() - std::min(device->table.size(), device->device_rules.size()) +
                rule_index;
    } else {
        for (const mimhid_node* other : live_nodes(device)) {
            if (other == node)
                break;
            if (other->generic)
                index += other->rules.size();
        }
    }
    if (!live) {
        own[rule_index].data = std::move(bytes);
        if (device->configured)
            device->dirty = true; // a batch is open: the commit sends the bytes
        return MIMHID_OK;
    }
    const mimhid_result r =
        send_rule_data(device, index, bytes.data(), bytes.size(), control_timeout_ms);
    if (r != MIMHID_OK)
        return r;
    own[rule_index].data = bytes;
    std::lock_guard<std::mutex> lock(device->rules_mutex);
    if (index < device->table.size())
        device->table[index].data = std::move(bytes);
    return MIMHID_OK;
}

// Sends the complete configuration of every live node and commits it. The board drops its USB
// device and re-enumerates it with exactly these nodes.
mimhid_result commit_device(mimhid_device* device, uint32_t reconnect_delay_ms,
                            const CommitWaits& waits) {
    const ReconfigGuard guard(device);
    const std::uint32_t control = waits.control;
    const std::vector<mimhid_node*> nodes = live_nodes(device);

    if (nodes.empty() && !device->descriptors_set) {
        if (device->configured) {
            const std::uint8_t detach[3] = {
                1U, static_cast<std::uint8_t>(reconnect_delay_ms & 0xFFU),
                static_cast<std::uint8_t>((reconnect_delay_ms >> 8U) & 0xFFU)};
            device->link.clear_ready();
            const mimhid_result r =
                command_error(device->link.command(MIMHID_T_COMMIT, detach, sizeof(detach), control,
                                                   kCommandAttempts),
                              "device.commit", "The board did not acknowledge going off the bus.",
                              MIMHID_T_COMMIT);
            if (r != MIMHID_OK)
                return r;
            device->configured = false;
        }
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            device->table.clear(); // the board dropped its rules with the configuration
        }
        device->dirty = false;
        return MIMHID_OK;
    }

    // The host composes every descriptor; the board takes them as bytes. A Device given as bytes
    // (mimhid_device_set_descriptors) has them already, and its strings are the raw ones.
    usb::ComposerError composer_error;
    std::vector<usb::HidNodeInput> hid_inputs;
    usb::Composed composed;
    const std::vector<std::uint8_t>* bos = nullptr;
    if (device->descriptors_set) {
        composed.device = device->bytes.device;
        composed.configuration = device->bytes.configuration;
        for (const auto& entry : device->raw_strings) {
            std::vector<std::uint8_t> descriptor{
                static_cast<std::uint8_t>(2U + 2U * entry.second.size()), 3U};
            for (const char16_t unit : entry.second)
                put_u16(&descriptor, unit);
            composed.strings.push_back(usb::ComposedString{entry.first.first, entry.first.second,
                                                           false, std::move(descriptor)});
        }
        if (!device->bytes.bos.empty())
            bos = &device->bytes.bos;
    } else {
        usb::ComposerInput input;
        fill_device_strings(device, &input);
        if (const mimhid_result made =
                groups_of_nodes(nodes, &input.groups, &hid_inputs, &composer_error);
            made != MIMHID_OK) {
            set_error(made, composer_error.field, composer_error.reason, 0, 0,
                      composer_error.node_id, 0, composer_error.offset, composer_error.length);
            return made;
        }
        if (const mimhid_result made = usb::compose_descriptors(input, &composed, &composer_error);
            made != MIMHID_OK) {
            set_error(made, composer_error.field, composer_error.reason, 0, 0,
                      composer_error.node_id, 0, composer_error.offset, composer_error.length);
            return made;
        }
        if (!device->composed_bos.empty()) {
            // A host asks for a BOS descriptor only from bcdUSB 0x0201 on (USB 3.2 9.6.2).
            if (device->config.usb_device.bcd_usb < 0x0201U) {
                set_error(MIMHID_ERR_PARAM, "device.bos",
                          "A BOS descriptor needs usb_device.bcd_usb of 0x0201 or more: a host "
                          "asks for one only then (USB 3.2 9.6.2); the value is in offset.",
                          0, 0, 0, 0, device->config.usb_device.bcd_usb, 0);
                return MIMHID_ERR_PARAM;
            }
            bos = &device->composed_bos;
        }
    }

    const auto send = [&](std::uint8_t type, const std::vector<std::uint8_t>& payload,
                          const char* field, const char* reason, std::uint16_t node_id) {
        return command_error(
            device->link.command(type, payload.data(), payload.size(), control, kCommandAttempts),
            field, reason, type, node_id);
    };
    // One descriptor: DESC_BEGIN with its length, then its bytes in chunks.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const auto send_blob = [&](std::uint8_t kind, std::uint8_t index, std::uint16_t language,
                               const std::vector<std::uint8_t>& bytes, std::uint8_t flags,
                               const char* field) {
        std::vector<std::uint8_t> begin{kind, index};
        put_u16(&begin, language);
        put_u16(&begin, static_cast<std::uint32_t>(bytes.size()));
        begin.push_back(flags);
        mimhid_result result =
            send(MIMHID_T_DESC_BEGIN, begin, field, "The board did not accept a descriptor.", 0U);
        for (std::size_t offset = 0U; result == MIMHID_OK && offset < bytes.size();
             offset += MIMHID_DESC_DATA_CHUNK) {
            const std::size_t chunk =
                std::min<std::size_t>(MIMHID_DESC_DATA_CHUNK, bytes.size() - offset);
            std::vector<std::uint8_t> data{kind, index};
            put_u16(&data, language);
            put_u16(&data, static_cast<std::uint32_t>(offset));
            data.insert(data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            result = send(MIMHID_T_DESC_DATA, data, field,
                          "The board did not accept the bytes of a descriptor.", 0U);
        }
        return result;
    };

    mimhid_result r = send(MIMHID_T_DESC_RESET, {}, "device.config",
                           "The board did not start a new configuration.", 0U);
    if (r == MIMHID_OK)
        r = send_blob(MIMHID_DESC_DEVICE, 0U, 0U, composed.device, 0U, "device.config");
    if (r == MIMHID_OK)
        r = send_blob(MIMHID_DESC_CONFIGURATION, 0U, 0U, composed.configuration, 0U,
                      "device.config");
    if (r == MIMHID_OK && bos != nullptr)
        r = send_blob(MIMHID_DESC_BOS, 0U, 0U, *bos, 0U, "device.bos");
    for (const usb::ComposedString& text : composed.strings) {
        if (r != MIMHID_OK)
            break;
        r = send_blob(MIMHID_DESC_STRING, text.index, text.language, text.descriptor,
                      text.from_chip_id ? MIMHID_DESC_F_SERIAL_FROM_ID : 0U, "device.strings");
    }
    if (r != MIMHID_OK)
        return r;

    // The control rules, with the bytes of their answers; the COMMIT makes them the active ones.
    std::vector<usb::BoardRule> table;
    if (r = build_rule_table(device, nodes, hid_inputs, &table); r != MIMHID_OK)
        return r;
    r = stage_rule_table(device, table, control);
    if (r != MIMHID_OK)
        return r;

    // A Device given as bytes: the policy of each of its IN endpoints.
    for (const EndpointRuntime& ep : device->bytes.in_endpoints) {
        if (!device->descriptors_set)
            break;
        r = send(MIMHID_T_EP_CONFIG, {ep.address, ep.policy.depth, ep.policy.on_full},
                 "device.endpoint.policy", "The board did not accept the policy of an endpoint.",
                 0U);
        if (r != MIMHID_OK)
            return r;
    }

    // The depth of the queue of each interface's IN endpoint (Nodes sharing an interface share it:
    // the least depth).
    // The policy of every IN endpoint: from the Node, or for profile Nodes from the interface,
    // which its Nodes share the settings of.
    for (const mimhid_node* node : nodes) {
        if (node->generic) {
            for (const EndpointRuntime& ep : node->in_endpoints) {
                r = send(MIMHID_T_EP_CONFIG, {ep.address, ep.policy.depth, ep.policy.on_full},
                         "node.endpoint.policy",
                         "The board did not accept the policy of an endpoint.",
                         public_node_id(node));
                if (r != MIMHID_OK)
                    return r;
            }
        } else {
            // Nodes on one interface share its endpoint: one command, the least depth.
            bool first_of_interface = true;
            for (const mimhid_node* other : nodes) {
                if (other == node)
                    break;
                if (!other->generic && other->iface == node->iface)
                    first_of_interface = false;
            }
            if (!first_of_interface)
                continue;
            r = send(MIMHID_T_EP_CONFIG,
                     {node->config.ep_in.address, hid_interface_depth(device, node->iface),
                      node->config.ep_in.policy.on_full},
                     "node.endpoint.policy", "The board did not accept the queue depth.",
                     public_node_id(node));
            if (r != MIMHID_OK)
                return r;
        }
    }

    device->rule_timeout_ms.store(control, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(device->rules_mutex);
        device->table = std::move(table);
    }
    start_rule_updater(device);
    device->link.clear_ready();
    // The board leaves the bus before every configuration, the first one of this Device too: a
    // program that ended without closing its Device (or closed it with keep_attached) leaves the
    // board on the target's bus, and a configuration sent to a board that is attached changes its
    // descriptors under a device the target has already enumerated. The target would never see
    // the new device, and the board would never report it mounted.
    r = command_error(device->link.disconnect(control), "device.commit",
                      "The board did not disconnect in time.", MIMHID_T_COMMIT);
    if (r != MIMHID_OK)
        return r;
    if (reconnect_delay_ms > 0U)
        std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
    r = command_error(device->link.connect(waits.reconnect), "device.commit",
                      "The board did not connect or enumerate in time.", MIMHID_T_COMMIT);
    if (r != MIMHID_OK)
        return r;
    device->configured = true;
    device->dirty = false;
    reset_endpoint_counts(device);
    if (device->report_all) {
        // A COMMIT applies the rules without the report-everything flag.
        std::size_t count = 0U;
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            count = device->table.size();
        }
        const std::vector<std::uint8_t> apply{1U, static_cast<std::uint8_t>(count)};
        r = command_error(command_retry_busy(device, MIMHID_T_RULES_APPLY, apply, control),
                          "device.control_rules", "The board did not apply the control rules.",
                          MIMHID_T_RULES_APPLY);
        if (r != MIMHID_OK)
            return r;
    }
    device->control_routed.store(device->report_all || !device->device_rules.empty(),
                                 std::memory_order_release);
    return MIMHID_OK;
}

// The public id of the live Node that owns an endpoint address, and the interface it is on; 0 when
// none does. A HID Node owns the endpoints of its config.
std::uint16_t node_of_endpoint(mimhid_device* device, const std::uint32_t address,
                               std::uint32_t* interface_number) {
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    for (const mimhid_node* node : device->slots) {
        if (node == nullptr || node->closed)
            continue;
        if (node->generic) {
            for (const GenericInterface& itf : node->interfaces) {
                for (const mimhid_usb_endpoint& ep : itf.endpoints) {
                    if (ep.address == address) {
                        *interface_number = itf.number;
                        return public_node_id(node);
                    }
                }
            }
        } else if (node->config.ep_in.address == address ||
                   (node->config.ep_out.address != 0U && node->config.ep_out.address == address)) {
            *interface_number = node->iface;
            return public_node_id(node);
        }
    }
    // A Device given as bytes has no Node: the id is 0, and the interface is the one the
    // configuration puts the endpoint on.
    if (device->descriptors_set) {
        const auto found = device->bytes.interface_of.find(static_cast<std::uint8_t>(address));
        if (found != device->bytes.interface_of.end())
            *interface_number = found->second;
    }
    return 0U;
}

void deliver_event(mimhid_device* device, const tr::BoardEvent& board) noexcept {
    const std::uint8_t* d = board.data.data();

    // The wait policy's count is kept whether or not anyone listens for events.
    if (board.kind == MIMHID_EV_EP_DONE) {
        if (board.length < 3U)
            return;
        const std::uint32_t done = static_cast<std::uint32_t>(d[1] | (d[2] << 8));
        {
            // As reset_endpoint_counts: nodes_mutex for the whole walk, then endpoint_mutex.
            std::lock_guard<std::mutex> nodes(device->nodes_mutex);
            std::lock_guard<std::mutex> lock(device->endpoint_mutex);
            for (mimhid_node* node : device->slots) {
                if (node == nullptr || node->closed)
                    continue;
                for (EndpointRuntime& ep : node->in_endpoints) {
                    if (ep.address == d[0])
                        ep.outstanding = ep.outstanding > done ? ep.outstanding - done : 0U;
                }
            }
            for (EndpointRuntime& ep : device->bytes.in_endpoints) {
                if (ep.address == d[0])
                    ep.outstanding = ep.outstanding > done ? ep.outstanding - done : 0U;
            }
        }
        device->endpoint_cv.notify_all();
        return;
    }
    // The target left: nothing the board held for it will be reported done (a flush of its own).
    if (board.kind == MIMHID_EV_UNMOUNTED) {
        reset_endpoint_counts(device);
        device->pending.clear();
    }
    if (board.kind == MIMHID_EV_MOUNTED) {
        device->pending.clear();
        reset_hid_answers(device);
    }
    mimhid_event event{};
    event.struct_size = sizeof(event);
    std::uint8_t iface = 0xFFU;
    // A report the target sent to a profile Node, as the HID driver of the board used to report
    // it: kind OUTPUT_REPORT or FEATURE_REPORT, the Report ID and the bytes after it.
    const auto hid_report = [&](const std::uint8_t type, const std::uint8_t id,
                                const std::uint8_t* data, std::size_t length) {
        if (type == 2U && id == 0U) {
            event.kind = MIMHID_EVENT_OUTPUT_REPORT; // the whole packet: the ID, if any, is in it
        } else {
            // TinyUSB strips the Report ID from the data stage when it matches wValue
            // (src/class/hid/hid_device.c, hidd_control_xfer_cb, SET_REPORT).
            if (id != 0U && length > 1U && data[0] == id) {
                ++data;
                --length;
            }
            event.kind = type == 3U ? MIMHID_EVENT_FEATURE_REPORT : MIMHID_EVENT_OUTPUT_REPORT;
            event.report_id = id;
        }
        event.data = data;
        event.length = length;
    };
    switch (board.kind) {
    case MIMHID_EV_MOUNTED:
        event.kind = MIMHID_EVENT_MOUNTED;
        break;
    case MIMHID_EV_UNMOUNTED:
        event.kind = MIMHID_EVENT_UNMOUNTED;
        break;
    case MIMHID_EV_SUSPEND:
        event.kind = MIMHID_EVENT_SUSPEND;
        event.value = board.length >= 1U ? d[0] : 0U;
        break;
    case MIMHID_EV_RESUME:
        event.kind = MIMHID_EVENT_RESUME;
        break;
    case MIMHID_EV_CONTROL_SETUP: {
        if (board.length < 10U)
            return;
        const std::uint8_t* setup = d + 2;
        if ((d[1] & MIMHID_CONTROL_F_ACCEPTED) != 0U) {
            // An ACCEPT rule took it. When it is one the library wrote for a profile interface,
            // the program hears of it as the HID event it stands for, not as a control transfer.
            usb::BoardRule::Role role = usb::BoardRule::Role::user;
            {
                std::lock_guard<std::mutex> lock(device->rules_mutex);
                for (const usb::BoardRule& entry : device->table) {
                    if (rule_matches(entry.rule, setup)) {
                        role = entry.role;
                        break;
                    }
                }
            }
            const std::uint16_t value = static_cast<std::uint16_t>(setup[2] | (setup[3] << 8));
            const std::uint16_t length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
            const std::uint8_t itf = static_cast<std::uint8_t>(setup[4]);
            if (role == usb::BoardRule::Role::hid_set_idle) {
                // HID 1.11 7.2.4: the idle rate is the high byte of wValue.
                const std::uint8_t rate = static_cast<std::uint8_t>(value >> 8U);
                set_hid_answer(device, usb::BoardRule::Role::hid_get_idle, itf, rate);
                event.kind = MIMHID_EVENT_SET_IDLE;
                event.value = rate;
                iface = itf;
                break;
            }
            if (role == usb::BoardRule::Role::hid_set_protocol) {
                // HID 1.11 7.2.6: the protocol is wValue (0 boot, 1 report).
                const std::uint8_t protocol = static_cast<std::uint8_t>(value & 0xFFU);
                set_hid_answer(device, usb::BoardRule::Role::hid_get_protocol, itf, protocol);
                event.kind = MIMHID_EVENT_SET_PROTOCOL;
                event.value = protocol;
                iface = itf;
                break;
            }
            if (role == usb::BoardRule::Role::hid_set_report) {
                if (length != 0U) {
                    // The data stage follows as CONTROL_DATA with this token.
                    if (device->pending.size() >= 8U)
                        device->pending.erase(device->pending.begin());
                    device->pending.push_back(mimhid_device::Pending{d[0], 0U, itf, value});
                    return;
                }
                hid_report(static_cast<std::uint8_t>(value >> 8U),
                           static_cast<std::uint8_t>(value & 0xFFU), d, 0U);
                iface = itf;
                break;
            }
        }
        if (device->config.transport.event_sink == nullptr)
            return;
        event.kind = MIMHID_EVENT_CONTROL_SETUP;
        event.control_token = d[0];
        event.control_flags = d[1];
        event.data = d + 2;
        event.length = 8U;
        break;
    }
    case MIMHID_EV_CONTROL_DATA:
        if (board.length < 1U)
            return;
        for (auto it = device->pending.begin(); it != device->pending.end(); ++it) {
            if (it->token != d[0])
                continue;
            const mimhid_device::Pending found = *it;
            device->pending.erase(it);
            hid_report(static_cast<std::uint8_t>(found.value >> 8U),
                       static_cast<std::uint8_t>(found.value & 0xFFU), d + 1, board.length - 1U);
            iface = found.iface;
            break;
        }
        if (iface != 0xFFU)
            break;
        if (device->config.transport.event_sink == nullptr)
            return;
        event.kind = MIMHID_EVENT_CONTROL_DATA;
        event.control_token = d[0];
        event.data = d + 1;
        event.length = board.length - 1U;
        break;
    case MIMHID_EV_TRANSFER_LOST: {
        if (board.length < 3U)
            return;
        event.kind = MIMHID_EVENT_TRANSFER_LOST;
        event.value = static_cast<std::uint32_t>(d[1] | (d[2] << 8));
        event.endpoint_address = d[0];
        event.node_id = node_of_endpoint(device, d[0], &event.interface_number);
        break;
    }
    case MIMHID_EV_EP_HALT:
        if (board.length < 2U)
            return;
        event.kind = MIMHID_EVENT_ENDPOINT_HALT;
        event.value = d[1];
        event.endpoint_address = d[0];
        event.node_id = node_of_endpoint(device, d[0], &event.interface_number);
        break;
    case MIMHID_EV_SET_INTERFACE: {
        if (board.length < 2U)
            return;
        // The board switched the interface's isochronous endpoints and dropped what waited for
        // the ones that stopped: the writers that wait on those endpoints start counting again.
        {
            std::lock_guard<std::mutex> nodes(device->nodes_mutex);
            std::lock_guard<std::mutex> lock(device->endpoint_mutex);
            for (EndpointRuntime& ep : device->bytes.in_endpoints) {
                const auto found = device->bytes.interface_of.find(ep.address);
                if (found != device->bytes.interface_of.end() && found->second == d[0]) {
                    ep.outstanding = 0U;
                    ++ep.epoch;
                }
            }
        }
        device->endpoint_cv.notify_all();
        event.kind = MIMHID_EVENT_INTERFACE_ALTERNATE;
        event.interface_number = d[0];
        event.value = d[1];
        break;
    }
    default:
        if (board.kind > MIMHID_T_EP_OUT_BASE && board.kind <= MIMHID_T_EP_OUT_LAST) {
            // A packet on an OUT endpoint, as it came (a zero-length one has length 0). On the OUT
            // endpoint of a profile Node it is an Output report, as the HID driver reported it.
            const std::uint32_t ep_address = board.kind & 0x0FU;
            std::uint8_t profile_iface = 0xFFU;
            {
                std::lock_guard<std::mutex> lock(device->nodes_mutex);
                for (const mimhid_node* node : device->slots) {
                    if (node != nullptr && !node->closed && !node->generic &&
                        node->config.ep_out.address != 0U &&
                        node->config.ep_out.address == ep_address)
                        profile_iface = node->iface;
                }
            }
            if (profile_iface != 0xFFU) {
                event.kind = MIMHID_EVENT_OUTPUT_REPORT;
                event.data = d;
                event.length = board.length;
                iface = profile_iface;
                break;
            }
            event.kind = MIMHID_EVENT_ENDPOINT_DATA;
            event.endpoint_address = board.kind & 0x0FU;
            event.data = d;
            event.length = board.length;
            event.node_id =
                node_of_endpoint(device, event.endpoint_address, &event.interface_number);
            break;
        }
        return;
    }
    if (device->config.transport.event_sink == nullptr)
        return;
    if (iface != 0xFFU) {
        event.interface_number = iface;
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        const mimhid_node* match = nullptr;
        if (event.kind == MIMHID_EVENT_OUTPUT_REPORT && event.length > 0U &&
            event.report_id == 0U) {
            const std::uint8_t candidate_id = event.data[0];
            for (const mimhid_node* node : device->slots) {
                if (node == nullptr || node->closed || node->iface != iface)
                    continue;
                if (node->has_report_ids && node->report_ids.test(candidate_id)) {
                    match = node;
                    event.report_id = candidate_id;
                    event.data += 1;
                    event.length -= 1U;
                    break;
                }
            }
        }
        if (match == nullptr) {
            for (const mimhid_node* node : device->slots) {
                if (node == nullptr || node->closed || node->iface != iface)
                    continue;
                if (event.report_id != 0U && node->has_report_ids &&
                    node->report_ids.test(event.report_id)) {
                    match = node;
                    break;
                }
                if (match == nullptr)
                    match = node;
            }
        }
        event.node_id = public_node_id(match);
    }
    try {
        device->config.transport.event_sink(device->config.transport.event_user, &event);
    } catch (...) {
        // A sink is foreign callback code and must not unwind through the reader thread.
    }
}

mimhid_device_info describe_board(const std::string& port, const std::string& description,
                                  const tr::BoardInfo& board, std::uint32_t baud) noexcept {
    mimhid_device_info info{};
    copy_text(info.port_name, port);
    copy_text(info.description, description);
    info.version_major = board.version[0];
    info.version_minor = board.version[1];
    info.version_patch = board.version[2];
    info.is_ready = board.ready ? 1U : 0U;
    info.firmware_compatible = board.compatible ? 1U : 0U;
    info.max_interfaces = board.max_interfaces_any;
    info.max_report_bytes =
        static_cast<std::uint8_t>(std::min<std::uint16_t>(board.max_report_bytes, 255U));
    info.max_descriptor_bytes = board.max_answer_bytes;
    info.has_unique_id = board.has_unique_id ? 1U : 0U;
    info.max_endpoint_number = board.max_endpoint_number;
    info.baud_rate = baud;
    info.max_baud = board.max_baud;
    info.uart_flags = board.uart_flags;
    info.chip = board.chip;
    info.usb_high_speed = board.usb_high_speed ? 1U : 0U;
    info.unique_id_length = board.unique_id_length;
    std::memcpy(info.unique_id, board.unique_id.data(), board.unique_id_length);
    return info;
}

} // namespace mimhid::detail
