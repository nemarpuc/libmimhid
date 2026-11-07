// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Device-side internals shared by the C ABI entry points: option validation, interface
 * assignment, the commit sequence and the events the board reports.
 */

#include "api/support.hpp"
#include "transport/link.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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

bool same_endpoint(const mimhid_usb_endpoint& a, const mimhid_usb_endpoint& b) noexcept {
    return a.address == b.address && a.poll_interval_ms == b.poll_interval_ms &&
           a.max_packet_size == b.max_packet_size;
}

bool same_interface_settings(const mimhid_node& a, const mimhid_node& b) {
    const mimhid_usb_interface& x = a.config.iface;
    const mimhid_usb_interface& y = b.config.iface;
    return x.class_code == y.class_code && x.subclass == y.subclass && x.protocol == y.protocol &&
           x.bcd_hid == y.bcd_hid && x.country_code == y.country_code &&
           a.interface_string == b.interface_string &&
           same_endpoint(a.config.ep_in, b.config.ep_in) &&
           same_endpoint(a.config.ep_out, b.config.ep_out);
}

// Gives every live node a USB interface based on its explicit iface.number.
// Unique iface.number values are mapped densely to 0..N-1 to maintain USB 2.0 compliance.
mimhid_result assign_interfaces(mimhid_device* device, const std::vector<mimhid_node*>& nodes) {
    const std::size_t limit = device->board.max_interfaces;
    std::set<std::uint32_t> unique_ifaces;
    for (const mimhid_node* node : nodes) {
        unique_ifaces.insert(node->config.iface.number);
    }
    if (unique_ifaces.size() > limit) {
        set_error(MIMHID_ERR_OVERFLOW, "node_config.iface.number",
                  "Number of distinct interfaces exceeds the board's maximum interfaces.");
        return MIMHID_ERR_OVERFLOW;
    }
    std::map<std::uint32_t, std::uint8_t> dense_map;
    std::uint8_t dense_idx = 0;
    for (std::uint32_t iface_num : unique_ifaces) {
        dense_map[iface_num] = dense_idx++;
    }
    for (mimhid_node* node : nodes) {
        node->iface = dense_map[node->config.iface.number];
    }

    // USB HID 1.11 Sec 5.6: If multiple nodes share an interface, all must have distinct Report
    // IDs.
    for (std::uint32_t iface_num : unique_ifaces) {
        std::vector<mimhid_node*> iface_nodes;
        for (mimhid_node* node : nodes) {
            if (node->config.iface.number == iface_num)
                iface_nodes.push_back(node);
        }
        if (iface_nodes.size() > 1U) {
            std::bitset<256> combined_ids;
            for (const mimhid_node* node : iface_nodes) {
                if (!node->has_report_ids || node->report_ids.none()) {
                    set_error(MIMHID_ERR_PARAM, "node.report_id",
                              "Multiple nodes sharing an interface must all declare Report IDs.");
                    return MIMHID_ERR_PARAM;
                }
                if ((combined_ids & node->report_ids).any()) {
                    set_error(MIMHID_ERR_PARAM, "node.report_id",
                              "Duplicate Report ID assigned on the same interface.");
                    return MIMHID_ERR_PARAM;
                }
                combined_ids |= node->report_ids;
            }
        }
    }

    // The board describes a shared interface from its first Node, so the others must agree with
    // it; and two interfaces with one endpoint address would make the configuration invalid.
    std::map<std::uint32_t, const mimhid_node*> first_of;
    for (const mimhid_node* node : nodes) {
        const auto [it, inserted] = first_of.emplace(node->config.iface.number, node);
        if (!inserted && !same_interface_settings(*it->second, *node)) {
            set_error(MIMHID_ERR_PARAM, "node_config.iface",
                      "Nodes sharing an interface must have the same interface and endpoint "
                      "settings.");
            return MIMHID_ERR_PARAM;
        }
    }
    std::set<std::uint8_t> endpoints;
    for (const auto& [number, node] : first_of) {
        if (!endpoints.insert(node->config.ep_in.address).second ||
            (node->config.ep_out.address != 0U &&
             !endpoints.insert(node->config.ep_out.address).second)) {
            set_error(MIMHID_ERR_PARAM, "node_config.ep_in.address",
                      "Two interfaces use the same endpoint address.", 0, 0, 0, 0, 0, number);
            return MIMHID_ERR_PARAM;
        }
    }
    return MIMHID_OK;
}

void put_u16(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

// A string on the wire: the number of UTF-16 code units, then the units, little endian. The text
// was checked by valid_usb_string, so the conversion cannot fail.
void put_string(std::vector<std::uint8_t>* out, const std::string& text) {
    std::u16string units;
    mimhid::detail::utf16_from_utf8(text.c_str(), &units);
    out->push_back(static_cast<std::uint8_t>(units.size()));
    for (const char16_t unit : units)
        put_u16(out, unit);
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
    if (config == nullptr || !mimhid::detail::valid_struct(
                                 config, config->struct_size,
                                 static_cast<std::uint32_t>(sizeof(*config)), "device_config")) {
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
    if (timeouts == nullptr ||
        !mimhid::detail::valid_struct(timeouts, timeouts->struct_size,
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

// Sends the complete configuration of every live node and commits it. The board drops its USB
// device and re-enumerates it with exactly these nodes.
mimhid_result commit_device(mimhid_device* device, uint32_t reconnect_delay_ms,
                            const CommitWaits& waits) {
    const ReconfigGuard guard(device);
    const std::uint32_t control = waits.control;
    const std::vector<mimhid_node*> nodes = live_nodes(device);

    if (nodes.empty()) {
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
        device->dirty = false;
        return MIMHID_OK;
    }

    std::size_t total_descriptor = 0U;
    for (const mimhid_node* node : nodes)
        total_descriptor += node->spec->descriptor.size();
    if (total_descriptor > device->board.max_descriptor_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open",
                  "The report descriptors together exceed the board's descriptor memory.", 0, 0, 0,
                  0, device->board.max_descriptor_bytes,
                  static_cast<std::uint32_t>(total_descriptor));
        return MIMHID_ERR_OVERFLOW;
    }

    if (const mimhid_result assigned = assign_interfaces(device, nodes); assigned != MIMHID_OK)
        return assigned;

    const mimhid_usb_device& dev = device->config.usb_device;
    const mimhid_usb_config& cfg = device->config.usb_config;
    std::vector<std::uint8_t> payload;
    put_u16(&payload, dev.vendor_id);
    put_u16(&payload, dev.product_id);
    put_u16(&payload, dev.bcd_device);
    payload.push_back(dev.device_class);
    payload.push_back(dev.device_subclass);
    payload.push_back(dev.device_protocol);
    put_u16(&payload, dev.language_id);
    put_u16(&payload, dev.bcd_usb);
    payload.push_back(dev.ep0_max_packet_size);
    payload.push_back(cfg.configuration_value);
    payload.push_back(cfg.bm_attributes);
    payload.push_back(cfg.bmax_power);
    std::uint8_t flags = 0U;
    if (dev.serial_from_chip_id == 1U)
        flags |= MIMHID_DEVFLAG_SERIAL_FROM_ID;
    payload.push_back(flags);
    put_string(&payload, device->manufacturer);
    put_string(&payload, device->product);
    put_string(&payload, device->serial);
    put_string(&payload, device->configuration);
    mimhid_result r =
        command_error(device->link.command(MIMHID_T_DEVICE_CONFIG, payload.data(), payload.size(),
                                           control, kCommandAttempts),
                      "device.config", "The board did not accept the device configuration.",
                      MIMHID_T_DEVICE_CONFIG);
    if (r != MIMHID_OK)
        return r;

    for (mimhid_node* node : nodes) {
        const std::vector<std::uint8_t>& desc = node->spec->descriptor;
        payload.clear();
        payload.push_back(node->node_id);
        payload.push_back(node->iface);
        put_u16(&payload, static_cast<std::uint32_t>(desc.size()));
        payload.push_back(node->config.iface.class_code);
        payload.push_back(node->config.iface.subclass);
        payload.push_back(node->config.iface.protocol);
        put_u16(&payload, node->config.iface.bcd_hid);
        payload.push_back(static_cast<std::uint8_t>(node->config.iface.country_code));
        payload.push_back(node->config.ep_in.address);
        put_u16(&payload, node->config.ep_in.max_packet_size);
        payload.push_back(static_cast<std::uint8_t>(node->config.ep_in.poll_interval_ms));
        payload.push_back(node->config.ep_out.address);
        put_u16(&payload, node->config.ep_out.max_packet_size);
        payload.push_back(static_cast<std::uint8_t>(node->config.ep_out.poll_interval_ms));
        payload.push_back(0U); // idle rate
        payload.push_back(0U); // flags
        put_string(&payload, node->interface_string);
        r = command_error(device->link.command(MIMHID_T_NODE_BEGIN, payload.data(), payload.size(),
                                               control, kCommandAttempts),
                          "node.config", "The board refused the node's interface settings.",
                          MIMHID_T_NODE_BEGIN, public_node_id(node));
        if (r != MIMHID_OK)
            return r;
        for (std::size_t offset = 0U; offset < desc.size(); offset += MIMHID_NODE_DATA_CHUNK) {
            const std::size_t chunk =
                std::min<std::size_t>(MIMHID_NODE_DATA_CHUNK, desc.size() - offset);
            payload.clear();
            payload.push_back(node->node_id);
            put_u16(&payload, static_cast<std::uint32_t>(offset));
            payload.insert(payload.end(), desc.begin() + static_cast<std::ptrdiff_t>(offset),
                           desc.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            r = command_error(device->link.command(MIMHID_T_NODE_DATA, payload.data(),
                                                   payload.size(), control, kCommandAttempts),
                              "node.descriptor", "The board did not accept the report descriptor.",
                              MIMHID_T_NODE_DATA, public_node_id(node));
            if (r != MIMHID_OK)
                return r;
        }
        for (const mimhid_node::FeatureEntry& feature : node->features) {
            payload.clear();
            payload.push_back(node->node_id);
            payload.push_back(feature.report_id);
            payload.insert(payload.end(), feature.data.begin(), feature.data.end());
            r = command_error(device->link.command(MIMHID_T_FEATURE, payload.data(), payload.size(),
                                                   control, kCommandAttempts),
                              "node.feature", "The board did not accept a Feature report answer.",
                              MIMHID_T_FEATURE, public_node_id(node));
            if (r != MIMHID_OK)
                return r;
        }
        if (node->queue_depth != 0U) {
            const std::uint8_t depth[2] = {node->node_id, node->queue_depth};
            r = command_error(device->link.command(MIMHID_T_QUEUE_DEPTH, depth, sizeof(depth),
                                                   control, kCommandAttempts),
                              "node.queue_depth", "The board did not accept the queue depth.",
                              MIMHID_T_QUEUE_DEPTH, public_node_id(node));
            if (r != MIMHID_OK)
                return r;
        }
    }

    device->link.clear_ready();
    if (device->configured) {
        r = command_error(device->link.disconnect(control), "device.commit",
                          "The board did not disconnect in time.", MIMHID_T_COMMIT);
        if (r != MIMHID_OK)
            return r;
        if (reconnect_delay_ms > 0U)
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms));
    }
    r = command_error(device->link.connect(waits.reconnect), "device.commit",
                      "The board did not connect or enumerate in time.", MIMHID_T_COMMIT);
    if (r != MIMHID_OK)
        return r;
    device->configured = true;
    device->dirty = false;
    return MIMHID_OK;
}

void deliver_event(mimhid_device* device, const tr::BoardEvent& board) noexcept {
    const std::uint8_t* d = board.data.data();

    if (device->config.transport.event_sink == nullptr)
        return;
    mimhid_event event{};
    event.struct_size = sizeof(event);
    std::uint8_t iface = 0xFFU;
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
    case MIMHID_EV_SET_REPORT:
        if (board.length < 3U)
            return;
        iface = d[0];
        // HID report types: 2 Output, 3 Feature.
        event.kind = d[1] == 3U ? MIMHID_EVENT_FEATURE_REPORT : MIMHID_EVENT_OUTPUT_REPORT;
        event.report_id = d[2];
        event.data = d + 3;
        event.length = board.length - 3U;
        break;
    case MIMHID_EV_OUTPUT_REPORT:
        if (board.length < 3U)
            return;
        iface = d[0];
        event.kind = MIMHID_EVENT_OUTPUT_REPORT;
        event.report_id = 0;
        event.data = d + 3;
        event.length =
            std::min<std::size_t>(static_cast<std::size_t>(d[1] | (d[2] << 8)), board.length - 3U);
        break;
    case MIMHID_EV_SET_PROTOCOL:
    case MIMHID_EV_SET_IDLE:
        if (board.length < 2U)
            return;
        iface = d[0];
        event.kind = board.kind == MIMHID_EV_SET_PROTOCOL ? MIMHID_EVENT_SET_PROTOCOL
                                                          : MIMHID_EVENT_SET_IDLE;
        event.value = d[1];
        break;
    case MIMHID_EV_CONTROL_SETUP:
        if (board.length < 10U)
            return;
        event.kind = MIMHID_EVENT_CONTROL_SETUP;
        event.control_token = d[0];
        event.control_flags = d[1];
        event.data = d + 2;
        event.length = 8U;
        break;
    case MIMHID_EV_CONTROL_DATA:
        if (board.length < 1U)
            return;
        event.kind = MIMHID_EVENT_CONTROL_DATA;
        event.control_token = d[0];
        event.data = d + 1;
        event.length = board.length - 1U;
        break;
    case MIMHID_EV_REPORT_LOST:
        if (board.length < 3U)
            return;
        iface = d[0];
        event.kind = MIMHID_EVENT_REPORT_LOST;
        event.value = static_cast<std::uint32_t>(d[1] | (d[2] << 8));
        break;
    default:
        return;
    }
    if (iface != 0xFFU) {
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
    info.max_nodes = board.max_nodes;
    info.max_interfaces = board.max_interfaces;
    info.max_report_bytes =
        static_cast<std::uint8_t>(std::min<std::uint16_t>(board.max_report_bytes, 255U));
    info.max_descriptor_bytes = board.max_descriptor_bytes;
    info.has_unique_id = board.has_unique_id ? 1U : 0U;
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
