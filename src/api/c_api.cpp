// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Stable C ABI orchestration. Serial details stay below the transport boundary; immutable
 * specs are shared while all mutable report state stays per node. The structure follows
 * libaoahid: Specs are opened as Nodes on a Device, and Nodes submit reports.
 */

#include "api/internal.hpp"
#include "transport/link.hpp"
#include "transport/serial_port.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace tr = mimhid::transport;

namespace {
using mimhid::detail::set_error;

// How often a configuration frame is sent before the board is given up on; each try waits for the
// timeout the caller passed.
constexpr int kCommandAttempts = 3;

// The public header repeats the wire protocol's tables.
static_assert(MIMHID_PROTOCOL_VERSION == MIMHID_PROTO_VERSION);
static_assert(tr::kBaudRates == std::array<std::uint32_t, MIMHID_BAUD_COUNT>(MIMHID_BAUD_RATES));
static_assert(MIMHID_CHIP_RP2040 == 1 && MIMHID_CHIP_RP2350 == 2 && MIMHID_CHIP_ESP32S3 == 3 &&
              MIMHID_CHIP_NRF52840 == 4 && MIMHID_CHIP_STM32F4 == 5);
static_assert(sizeof(mimhid_device_info::unique_id) == MIMHID_UNIQUE_ID_MAX);
static_assert(MIMHID_CONTROL_MAX_RULES == MIMHID_CONTROL_RULES &&
              MIMHID_CONTROL_MAX_TIMEOUT_MS == MIMHID_CONTROL_TIMEOUT_MAX);
static_assert(MIMHID_EVENT_CONTROL_SETUP == MIMHID_EV_CONTROL_SETUP &&
              MIMHID_EVENT_CONTROL_DATA == MIMHID_EV_CONTROL_DATA);
static_assert(MIMHID_CONTROL_FLAG_HELD == MIMHID_CONTROL_F_HELD &&
              MIMHID_CONTROL_FLAG_TOO_LONG == MIMHID_CONTROL_F_TOO_LONG &&
              MIMHID_CONTROL_FLAG_FIXED == MIMHID_CONTROL_F_FIXED &&
              MIMHID_CONTROL_FLAG_LOST == MIMHID_CONTROL_F_LOST);
static_assert(MIMHID_CONTROL_ACTION_LEAVE == MIMHID_CONTROL_LEAVE + 1 &&
              MIMHID_CONTROL_ACTION_ANSWER == MIMHID_CONTROL_ANSWER + 1 &&
              MIMHID_CONTROL_ACTION_STALL == MIMHID_CONTROL_STALL + 1 &&
              MIMHID_CONTROL_ACTION_DROP == MIMHID_CONTROL_DROP + 1);
// The largest report this library sends: a high-speed interrupt endpoint packet.
constexpr std::size_t kMaximumReportBytes = MIMHID_HOST_MAX_REPORT_BYTES;

std::uint16_t report_id(const mimhid_node* node) noexcept {
    return node != nullptr && node->spec != nullptr && node->spec->layout.has_report_id
               ? node->spec->layout.report_id
               : 0U;
}

std::uint16_t public_node_id(const mimhid_node* node) noexcept {
    return node == nullptr ? std::uint16_t{0} : static_cast<std::uint16_t>(node->node_id + 1U);
}

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

// ---- options -------------------------------------------------------------------------

bool valid_usb_string(const char* text) noexcept {
    if (text == nullptr)
        return true;
    std::size_t length = 0U;
    for (; text[length] != '\0'; ++length) {
        const auto c = static_cast<unsigned char>(text[length]);
        if (c < 0x20U || c > 0x7EU)
            return false;
    }
    return length <= MIMHID_STRING_MAX;
}

bool required_product_policies_nonzero(const mimhid_transport_options& options) noexcept {
    return options.linux_descriptor_policy_bytes != 0U &&
           options.linux_hid_fields_per_report_policy != 0U &&
           options.linux_hid_global_stack_depth_policy != 0U &&
           options.linux_hid_usages_policy != 0U &&
           options.linux_hid_report_data_policy_bits != 0U &&
           options.linux_hid_report_size_policy_bits != 0U;
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
            set_error(MIMHID_ERR_UNSET_FIELD, field, "This value must be set; it has no default.");
            return MIMHID_ERR_UNSET_FIELD;
        }
    }
    if (tr::baud_index_of(effective->transport.baud_rate) < 0 ||
        !tr::SerialPort::baud_supported(effective->transport.baud_rate)) {
        set_error(MIMHID_ERR_UNSUPPORTED, "transport.baud_rate",
                  "The rate is not one of MIMHID_BAUD_RATES that this system can set.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (!required_product_policies_nonzero(effective->transport)) {
        set_error(MIMHID_ERR_UNSET_FIELD, "transport.linux_policy",
                  "Every required product policy must be nonzero.");
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

    // USB Device required fields
    const std::pair<std::uint32_t, const char*> required_usb_device[] = {
        {effective->usb_device.vendor_id, "usb_device.vendor_id"},
        {effective->usb_device.language_id, "usb_device.language_id"},
        {effective->usb_device.bcd_usb, "usb_device.bcd_usb"},
        {effective->usb_device.ep0_max_packet_size, "usb_device.ep0_max_packet_size"},
    };
    for (const auto& [value, field] : required_usb_device) {
        if (value == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, field, "This value must be set; it has no default.");
            return MIMHID_ERR_UNSET_FIELD;
        }
    }
    if (effective->usb_device.ep0_max_packet_size != 8U &&
        effective->usb_device.ep0_max_packet_size != 16U &&
        effective->usb_device.ep0_max_packet_size != 32U &&
        effective->usb_device.ep0_max_packet_size != 64U) {
        set_error(MIMHID_ERR_PARAM, "usb_device.ep0_max_packet_size",
                  "ep0_max_packet_size must be 8, 16, 32, or 64.");
        return MIMHID_ERR_PARAM;
    }
    if (!mimhid::detail::valid_boolean(effective->usb_device.serial_from_chip_id)) {
        set_error(MIMHID_ERR_PARAM, "usb_device.serial_from_chip_id",
                  "serial_from_chip_id must be zero or one.");
        return MIMHID_ERR_PARAM;
    }
    for (const char* text : {effective->usb_device.manufacturer, effective->usb_device.product,
                             effective->usb_device.serial}) {
        if (!valid_usb_string(text)) {
            set_error(MIMHID_ERR_PARAM, "usb_device.strings",
                      "A string must be printable ASCII, at most 126 characters.");
            return MIMHID_ERR_PARAM;
        }
    }

    // USB Config required fields
    if (effective->usb_config.configuration_value == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_config.configuration_value",
                  "configuration_value must be non-zero (e.g. 1).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if ((effective->usb_config.bm_attributes & 0x80U) == 0U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bm_attributes",
                  "bm_attributes bit 7 must be set (0x80).");
        return MIMHID_ERR_PARAM;
    }
    if (effective->usb_config.bmax_power > 250U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bmax_power",
                  "bmax_power must be at most 250 (500 mA).");
        return MIMHID_ERR_PARAM;
    }
    if (!valid_usb_string(effective->usb_config.string)) {
        set_error(MIMHID_ERR_PARAM, "usb_config.string",
                  "A string must be printable ASCII, at most 126 characters.");
        return MIMHID_ERR_PARAM;
    }

    return MIMHID_OK;
}

// ---- node state helpers --------------------------------------------------------------

mimhid_result initialize_node_state(mimhid_node* node) {
    switch (node->spec->kind) {
    case MIMHID_PROFILE_KEYBOARD: {
        const auto* spec = std::get_if<mimhid::detail::KeyboardConfig>(&node->spec->config);
        mimhid::detail::KeyboardState value{};
        const std::size_t key_count =
            static_cast<std::size_t>(spec->options.usage_maximum - spec->options.usage_minimum) +
            1U;
        value.key_transitions.resize(key_count);
        value.pressed_bitmap.resize(key_count);
        node->state = std::move(value);
        break;
    }
    case MIMHID_PROFILE_MOUSE: {
        const auto* spec = std::get_if<mimhid::detail::MouseConfig>(&node->spec->config);
        mimhid::detail::MouseState value{};
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMHID_PROFILE_TOGGLE: {
        const auto* spec = std::get_if<mimhid::detail::ConsumerConfig>(&node->spec->config);
        mimhid::detail::ConsumerState value{};
        value.pressed.resize(spec->allowed_usages.size());
        value.transitions.resize(spec->allowed_usages.size());
        node->state = std::move(value);
        break;
    }
    case MIMHID_PROFILE_GAMEPAD: {
        const auto* spec = std::get_if<mimhid::detail::GamepadConfig>(&node->spec->config);
        mimhid::detail::GamepadState value{};
        value.axes.reserve(spec->axes.size());
        for (const auto& axis : spec->axes)
            value.axes.push_back(axis.axis.neutral_value);
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMHID_PROFILE_TOUCHSCREEN:
    case MIMHID_PROFILE_TOUCHPAD: {
        const auto* spec = std::get_if<mimhid::detail::TouchConfig>(&node->spec->config);
        mimhid::detail::TouchState value{};
        value.buttons.resize(spec->fields.button_count);
        value.button_transitions.resize(spec->fields.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMHID_PROFILE_PEN:
    case MIMHID_PROFILE_DIGITIZER:
        node->state = mimhid::detail::PenState{};
        break;
    case MIMHID_PROFILE_RAW:
        node->state = mimhid::detail::RawState{};
        node->dirty = false;
        break;
    default:
        set_error(MIMHID_ERR_INTERNAL, "spec.kind",
                  "A validated ProfileSpec contained an unknown profile kind.");
        return MIMHID_ERR_INTERNAL;
    }
    return MIMHID_OK;
}

bool has_non_neutral_state(const mimhid_node* node) noexcept {
    if (node == nullptr || node->spec == nullptr) {
        return false;
    }
    if (const auto* keyboard = std::get_if<mimhid::detail::KeyboardState>(&node->state)) {
        return keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    }
    if (const auto* mouse = std::get_if<mimhid::detail::MouseState>(&node->state)) {
        return std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* consumer = std::get_if<mimhid::detail::ConsumerState>(&node->state)) {
        return std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* gamepad = std::get_if<mimhid::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<mimhid::detail::GamepadConfig>(&node->spec->config);
        if (spec == nullptr)
            return false;
        for (std::size_t index = 0U; index < gamepad->axes.size(); ++index) {
            if (gamepad->axes[index] != spec->axes[index].axis.neutral_value)
                return true;
        }
        return gamepad->hat_has_direction ||
               std::any_of(gamepad->buttons.begin(), gamepad->buttons.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* touch = std::get_if<mimhid::detail::TouchState>(&node->state)) {
        const bool active_contact =
            std::any_of(touch->contacts.begin(), touch->contacts.end(), [](const auto& contact) {
                return contact.phase == mimhid::detail::ContactPhase::down;
            });
        return active_contact || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* pen = std::get_if<mimhid::detail::PenState>(&node->state)) {
        return pen->tool_switch_departure || pen->desired.in_range != 0U ||
               pen->desired.tip != 0U || pen->desired.barrel_buttons != 0U;
    }
    return false;
}

// Each helper is deliberately noinline and touches exactly one NodeState
// alternative. This is a one-time close-path operation, never the per-report
// send path, so the call overhead is immaterial; keeping each alternative's
// vector access in its own non-inlined function also keeps GCC 14's
// -Wstringop-overflow analysis from unifying value ranges across unrelated
// sibling if/else-if arms of one large function, which produced a false
// positive at -O3 on musl. MSVC does not
// recognize the gnu:: attribute namespace and, under /WX, turns its own
// warning about that into a hard error, so this is spelled per compiler
// rather than as a portable [[gnu::noinline]] attribute.
#if defined(_MSC_VER) && !defined(__clang__)
#define MIMHID_NOINLINE __declspec(noinline)
#else
#define MIMHID_NOINLINE __attribute__((noinline))
#endif

MIMHID_NOINLINE bool neutralize_keyboard(mimhid::detail::KeyboardState* keyboard) noexcept {
    const bool had_non_neutral_state =
        keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    std::fill(keyboard->pressed_bitmap.begin(), keyboard->pressed_bitmap.end(), std::uint8_t{0});
    keyboard->pressed_bitmap_count = 0U;
    keyboard->modifiers = 0U;
    return had_non_neutral_state;
}

MIMHID_NOINLINE bool neutralize_mouse(mimhid::detail::MouseState* mouse) noexcept {
    const bool had_non_neutral_state =
        std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                    [](const std::uint8_t value) { return value != 0U; });
    std::fill(mouse->buttons.begin(), mouse->buttons.end(), std::uint8_t{0});
    mouse->dx = mouse->dy = mouse->wheel = mouse->pan = 0;
    return had_non_neutral_state;
}

MIMHID_NOINLINE bool neutralize_gamepad(mimhid::detail::GamepadState* gamepad,
                                        const mimhid::detail::GamepadConfig* spec) noexcept {
    bool had_non_neutral_state = false;
    for (std::size_t index = 0U; index < gamepad->axes.size(); ++index) {
        had_non_neutral_state =
            had_non_neutral_state || gamepad->axes[index] != spec->axes[index].axis.neutral_value;
        gamepad->axes[index] = spec->axes[index].axis.neutral_value;
    }
    had_non_neutral_state = had_non_neutral_state || gamepad->hat_has_direction ||
                            std::any_of(gamepad->buttons.begin(), gamepad->buttons.end(),
                                        [](const std::uint8_t value) { return value != 0U; });
    std::fill(gamepad->buttons.begin(), gamepad->buttons.end(), std::uint8_t{0});
    gamepad->hat_has_direction = false;
    gamepad->hat = 0;
    return had_non_neutral_state;
}

MIMHID_NOINLINE bool neutralize_touch(mimhid::detail::TouchState* touch) noexcept {
    bool had_non_neutral_state = false;
    for (auto& contact : touch->contacts) {
        had_non_neutral_state =
            had_non_neutral_state || contact.phase != mimhid::detail::ContactPhase::none;
        if (contact.phase == mimhid::detail::ContactPhase::down) {
            contact.phase = mimhid::detail::ContactPhase::up;
        }
    }
    had_non_neutral_state =
        had_non_neutral_state || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    std::fill(touch->buttons.begin(), touch->buttons.end(), std::uint8_t{0});
    touch->packet_cursor = 0U;
    return had_non_neutral_state;
}

MIMHID_NOINLINE bool neutralize_pen(mimhid::detail::PenState* pen) noexcept {
    const bool had_non_neutral_state = pen->desired.in_range != 0U || pen->desired.tip != 0U ||
                                       pen->desired.barrel_buttons != 0U ||
                                       pen->emitted.in_range != 0U || pen->emitted.tip != 0U ||
                                       pen->emitted.barrel_buttons != 0U;
    pen->desired.in_range = 0U;
    pen->desired.tip = 0U;
    pen->desired.barrel_buttons = 0U;
    pen->desired.pressure = 0;
    pen->tool_switch_departure = false;
    return had_non_neutral_state;
}
#undef MIMHID_NOINLINE

void make_neutral(mimhid_node* node) noexcept {
    bool had_non_neutral_state = false;
    if (auto* keyboard = std::get_if<mimhid::detail::KeyboardState>(&node->state)) {
        had_non_neutral_state = neutralize_keyboard(keyboard);
    } else if (auto* mouse = std::get_if<mimhid::detail::MouseState>(&node->state)) {
        had_non_neutral_state = neutralize_mouse(mouse);
    } else if (auto* consumer = std::get_if<mimhid::detail::ConsumerState>(&node->state)) {
        had_non_neutral_state = std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                                            [](const std::uint8_t value) { return value != 0U; });
        std::fill(consumer->pressed.begin(), consumer->pressed.end(), std::uint8_t{0});
    } else if (auto* gamepad = std::get_if<mimhid::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<mimhid::detail::GamepadConfig>(&node->spec->config);
        had_non_neutral_state = neutralize_gamepad(gamepad, spec);
    } else if (auto* touch = std::get_if<mimhid::detail::TouchState>(&node->state)) {
        had_non_neutral_state = neutralize_touch(touch);
    } else if (auto* pen = std::get_if<mimhid::detail::PenState>(&node->state)) {
        had_non_neutral_state = neutralize_pen(pen);
    }
    node->dirty = (had_non_neutral_state || node->emitted_non_neutral) &&
                  node->spec->kind != MIMHID_PROFILE_RAW;
}

bool touch_frame_in_progress(const mimhid_node* node) noexcept {
    const auto* touch =
        node == nullptr ? nullptr : std::get_if<mimhid::detail::TouchState>(&node->state);
    return touch != nullptr && touch->packet_cursor != 0U;
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
                            std::uint8_t frame_type, std::uint16_t node_id = 0U) noexcept {
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

// ---- interface assignment and commit ---------------------------------------------------

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

    return MIMHID_OK;
}

void put_u16(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put_string(std::vector<std::uint8_t>* out, const std::string& text) {
    out->push_back(static_cast<std::uint8_t>(text.size()));
    out->insert(out->end(), text.begin(), text.end());
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

struct ReconfigGuard {
    explicit ReconfigGuard(mimhid_device* d) : device(d) {
        device->reconfiguring.store(true, std::memory_order_release);
    }
    ~ReconfigGuard() { device->reconfiguring.store(false, std::memory_order_release); }
    mimhid_device* device;
};

// A wait the caller passed to a call has no default: zero is refused.
mimhid_result require_timeout(const std::uint32_t timeout_ms, const char* field) noexcept {
    if (timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, field, "This timeout must be set; it has no default.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    return MIMHID_OK;
}

// The waits of a call that configures the board and re-enumerates its USB device.
struct CommitWaits {
    std::uint32_t control{};
    std::uint32_t reconnect{};
};

mimhid_result read_commit_timeouts(const mimhid_commit_timeouts* timeouts,
                                   CommitWaits* waits) noexcept {
    if (timeouts == nullptr ||
        !mimhid::detail::valid_struct(timeouts, timeouts->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*timeouts)), "timeouts")) {
        return MIMHID_ERR_PARAM;
    }
    if (!reserved_is_zero(timeouts->reserved, "timeouts.reserved"))
        return MIMHID_ERR_PARAM;
    if (const mimhid_result r = require_timeout(timeouts->control_timeout_ms,
                                                "timeouts.control_timeout_ms");
        r != MIMHID_OK)
        return r;
    if (const mimhid_result r = require_timeout(timeouts->reconnect_timeout_ms,
                                                "timeouts.reconnect_timeout_ms");
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

// ---- events from the board -------------------------------------------------------------

#ifndef MIMHID_EV_OUTPUT_REPORT
#define MIMHID_EV_OUTPUT_REPORT 8
#endif

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
        event.length = std::min<std::size_t>(static_cast<std::size_t>(d[1] | (d[2] << 8)),
                                             board.length - 3U);
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

mimhid_result flush_node(mimhid_node* node, std::uint32_t timeout_ms, bool drop_battery);

// Sends a node's pending reports until it is clean, for the close paths.
mimhid_result send_neutral(mimhid_node* node, std::uint32_t timeout_ms) {
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

mimhid_device_info describe_board(const std::string& port, const std::string& description,
                                  const tr::BoardInfo& board, std::uint32_t baud) noexcept {
    mimhid_device_info info{};
    std::strncpy(info.port_name, port.c_str(), sizeof(info.port_name) - 1U);
    std::strncpy(info.description, description.c_str(), sizeof(info.description) - 1U);
    info.version_major = board.version[0];
    info.version_minor = board.version[1];
    info.version_patch = board.version[2];
    info.is_ready = board.ready ? 1U : 0U;
    info.protocol_version = board.protocol;
    info.max_nodes = board.max_nodes;
    info.max_interfaces = board.max_interfaces;
    info.max_report_bytes = static_cast<std::uint8_t>(std::min<std::uint16_t>(board.max_report_bytes, 255U));
    info.max_descriptor_bytes = board.max_descriptor_bytes;
    info.has_unique_id = board.has_unique_id ? 1U : 0U;
    info.baud_rate = baud;
    info.baud_mask = board.baud_mask;
    info.chip = board.chip;
    info.usb_high_speed = board.usb_high_speed ? 1U : 0U;
    info.unique_id_length = board.unique_id_length;
    std::memcpy(info.unique_id, board.unique_id.data(), board.unique_id_length);
    return info;
}

} // namespace

#define MIMHID_C_RESULT_CATCH(api_name)                                                            \
    catch (const std::bad_alloc&) {                                                                \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
        return MIMHID_ERR_INTERNAL;                                                                \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
        return MIMHID_ERR_INTERNAL;                                                                \
    }

#define MIMHID_C_VALUE_CATCH(api_name, fallback)                                                   \
    catch (const std::bad_alloc&) {                                                                \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
        return fallback;                                                                           \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
        return fallback;                                                                           \
    }

#define MIMHID_C_VOID_CATCH(api_name)                                                              \
    catch (const std::bad_alloc&) {                                                                \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(MIMHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
    }

extern "C" {

// ---- discovery -------------------------------------------------------------------------

mimhid_result MIMHID_CALL mimhid_discover(const std::uint32_t timeout_ms,
                                          mimhid_discovery** out_discovery) try {
    mimhid::detail::clear_error();
    if (out_discovery == nullptr) {
        set_error(MIMHID_ERR_PARAM, "out_discovery", "The output pointer is null.");
        return MIMHID_ERR_PARAM;
    }
    *out_discovery = nullptr;
    if (timeout_ms == 0U) {
        set_error(MIMHID_ERR_PARAM, "timeout_ms", "A nonzero timeout is required.");
        return MIMHID_ERR_PARAM;
    }
    auto discovery = std::make_unique<mimhid_discovery>();
    std::vector<tr::PortCandidate> ports = tr::list_candidate_ports();
    std::sort(ports.begin(), ports.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    // Each port is searched on its own thread; a board's rate is measured and the board left
    // on the boot rate.
    std::vector<std::optional<mimhid_device_info>> found(ports.size());
    std::vector<std::thread> searches;
    searches.reserve(ports.size());
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].path.size() >= sizeof(mimhid_device_info::port_name))
            continue;
        searches.emplace_back([&, i] {
            tr::SerialPort port;
            if (!port.open(ports[i].path.c_str(), MIMHID_BOOT_BAUD).ok)
                return;
            tr::Handshaker handshake(port, timeout_ms);
            tr::BoardInfo board;
            if (!handshake.find(&board))
                return;
            // A board on another protocol is listed with its version, to be flashed again.
            std::uint32_t fastest = handshake.rate();
            if (board.protocol == MIMHID_PROTO_VERSION) {
                fastest = handshake.fastest(&board);
                if (fastest == 0U || !handshake.to_boot(&board))
                    return;
            }
            found[i] = describe_board(ports[i].path, ports[i].description, board, fastest);
        });
    }
    for (std::thread& search : searches)
        search.join();
    for (const auto& entry : found) {
        if (entry)
            discovery->entries.push_back(*entry);
    }
    *out_discovery = discovery.release();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("discover")

std::size_t MIMHID_CALL mimhid_discovery_count(const mimhid_discovery* discovery) try {
    mimhid::detail::clear_error();
    if (discovery == nullptr) {
        set_error(MIMHID_ERR_PARAM, "discovery", "The discovery handle is null.");
        return 0U;
    }
    return discovery->entries.size();
}
MIMHID_C_VALUE_CATCH("discovery.count", 0U)

const mimhid_device_info* MIMHID_CALL mimhid_discovery_get(const mimhid_discovery* discovery,
                                                           const std::size_t index) try {
    mimhid::detail::clear_error();
    if (discovery == nullptr || index >= discovery->entries.size()) {
        set_error(MIMHID_ERR_PARAM, "discovery",
                  "A discovery handle and an index in range are required.");
        return nullptr;
    }
    return &discovery->entries[index];
}
MIMHID_C_VALUE_CATCH("discovery.get", nullptr)

void MIMHID_CALL mimhid_discovery_destroy(mimhid_discovery* discovery) try {
    mimhid::detail::clear_error();
    if (discovery == nullptr) {
        set_error(MIMHID_ERR_PARAM, "discovery", "The discovery handle is null.");
        return;
    }
    delete discovery;
}
MIMHID_C_VOID_CATCH("discovery.destroy")

// ---- device ----------------------------------------------------------------------------

mimhid_result MIMHID_CALL mimhid_device_open(const char* port_path,
                                             const mimhid_device_config* config,
                                             mimhid_device** out_device) try {
    mimhid::detail::clear_error();
    if (out_device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "out_device", "The output pointer is null.");
        return MIMHID_ERR_PARAM;
    }
    *out_device = nullptr;
    if (port_path == nullptr || port_path[0] == '\0') {
        set_error(MIMHID_ERR_PARAM, "port_path", "A non-empty port_path is required.");
        return MIMHID_ERR_PARAM;
    }
    mimhid_device_config effective{};
    if (const mimhid_result prepared = prepare_device_config(config, &effective);
        prepared != MIMHID_OK) {
        return prepared;
    }

    auto device = std::make_unique<mimhid_device>();
    device->config = effective;
    device->manufacturer =
        effective.usb_device.manufacturer != nullptr ? effective.usb_device.manufacturer : "";
    device->product = effective.usb_device.product != nullptr ? effective.usb_device.product : "";
    device->serial = effective.usb_device.serial != nullptr ? effective.usb_device.serial : "";
    device->configuration =
        effective.usb_config.string != nullptr ? effective.usb_config.string : "";
    // The caller's string pointers are not retained.
    device->config.usb_device.manufacturer = device->config.usb_device.product = nullptr;
    device->config.usb_device.serial = nullptr;
    device->config.usb_config.string = nullptr;

    const tr::OpenResult opened = device->link.open(port_path);
    if (!opened.ok) {
        if (opened.step == tr::OpenStep::open) {
            set_error(MIMHID_ERR_ACCESS, "port",
                      "Failed to open the serial port (it may be in use by another program).",
                      opened.os_error);
            return MIMHID_ERR_ACCESS;
        }
        set_error(MIMHID_ERR_IO, "port", "Failed to configure the serial port.", opened.os_error);
        return MIMHID_ERR_IO;
    }
    // The board's own limits and the line rate come from talking to it.
    tr::Handshaker handshake(device->link.port(), device->config.transport.handshake_timeout_ms);
    if (!handshake.find(&device->board)) {
        set_error(MIMHID_ERR_TIMEOUT, "port",
                  "No board answered on this port at any rate (check the firmware is libmimhid "
                  "1.0.1 or newer, the wiring, and the port).");
        return MIMHID_ERR_TIMEOUT;
    }
    if (device->board.protocol != MIMHID_PROTO_VERSION) {
        set_error(MIMHID_ERR_PROTOCOL, "port",
                  "The board's firmware speaks another protocol version: flash the firmware of "
                  "this libmimhid release.",
                  0, 0, 0, 0, MIMHID_PROTO_VERSION, device->board.protocol);
        return MIMHID_ERR_PROTOCOL;
    }
    switch (handshake.switch_to(effective.transport.baud_rate, &device->board)) {
    case tr::Handshake::ok:
        break;
    case tr::Handshake::rate_refused:
        set_error(MIMHID_ERR_UNSUPPORTED, "baud_rate",
                  "The board cannot run at this rate, or the serial port refused to be set to it.");
        return MIMHID_ERR_UNSUPPORTED;
    case tr::Handshake::rate_failed:
        set_error(MIMHID_ERR_UNSUPPORTED, "baud_rate",
                  "The link did not carry this rate: the serial adapter or the wiring does not "
                  "support it. mimhid_discover measures the fastest rate that works.");
        return MIMHID_ERR_UNSUPPORTED;
    case tr::Handshake::no_board:
    case tr::Handshake::io_error:
        set_error(MIMHID_ERR_IO, "port", "The board stopped answering while the rate was agreed.");
        return MIMHID_ERR_IO;
    }

    if (effective.transport.maximum_report_bytes > device->board.max_report_bytes ||
        effective.transport.maximum_report_bytes > kMaximumReportBytes) {
        set_error(MIMHID_ERR_OVERFLOW, "transport.maximum_report_bytes",
                  "The limit exceeds what the board accepts.");
        return MIMHID_ERR_OVERFLOW;
    }
    device->maximum_report_bytes = effective.transport.maximum_report_bytes;
    device->info = describe_board(port_path, "", device->board, effective.transport.baud_rate);

    mimhid_device* raw = device.get();
    device->link.start([raw](const tr::BoardEvent& event) { deliver_event(raw, event); });

    mimhid::detail::log_message(raw, MIMHID_LOG_INFO, "device opened");
    *out_device = device.release();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.open")

mimhid_result MIMHID_CALL
mimhid_device_set_usb_identity(mimhid_device* device,
                               const mimhid_usb_device* usb_device,
                               const mimhid_usb_config* usb_config,
                               const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (usb_device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "usb_device", "usb_device cannot be null.");
        return MIMHID_ERR_PARAM;
    }
    if (usb_config == nullptr) {
        set_error(MIMHID_ERR_PARAM, "usb_config", "usb_config cannot be null.");
        return MIMHID_ERR_PARAM;
    }
    if (usb_device->vendor_id == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_device.vendor_id", "vendor_id must be non-zero.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (usb_device->language_id == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_device.language_id", "language_id must be non-zero.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (usb_device->bcd_usb == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_device.bcd_usb", "bcd_usb must be non-zero.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (usb_device->ep0_max_packet_size != 8U && usb_device->ep0_max_packet_size != 16U &&
        usb_device->ep0_max_packet_size != 32U && usb_device->ep0_max_packet_size != 64U) {
        set_error(MIMHID_ERR_PARAM, "usb_device.ep0_max_packet_size",
                  "ep0_max_packet_size must be 8, 16, 32, or 64.");
        return MIMHID_ERR_PARAM;
    }
    if (!mimhid::detail::valid_boolean(usb_device->serial_from_chip_id)) {
        set_error(MIMHID_ERR_PARAM, "usb_device.serial_from_chip_id",
                  "serial_from_chip_id must be zero or one.");
        return MIMHID_ERR_PARAM;
    }
    for (const char* text : {usb_device->manufacturer, usb_device->product, usb_device->serial}) {
        if (!valid_usb_string(text)) {
            set_error(MIMHID_ERR_PARAM, "usb_device.strings",
                      "A string must be printable ASCII, at most 126 characters.");
            return MIMHID_ERR_PARAM;
        }
    }
    if (usb_config->configuration_value == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "usb_config.configuration_value",
                  "configuration_value must be non-zero (e.g. 1).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if ((usb_config->bm_attributes & 0x80U) == 0U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bm_attributes",
                  "bm_attributes bit 7 must be set (0x80).");
        return MIMHID_ERR_PARAM;
    }
    if (usb_config->bmax_power > 250U) {
        set_error(MIMHID_ERR_PARAM, "usb_config.bmax_power",
                  "bmax_power must be at most 250 (500 mA).");
        return MIMHID_ERR_PARAM;
    }
    if (!valid_usb_string(usb_config->string)) {
        set_error(MIMHID_ERR_PARAM, "usb_config.string",
                  "A string must be printable ASCII, at most 126 characters.");
        return MIMHID_ERR_PARAM;
    }

    device->config.usb_device = *usb_device;
    device->config.usb_config = *usb_config;
    device->manufacturer = usb_device->manufacturer ? usb_device->manufacturer : "";
    device->product = usb_device->product ? usb_device->product : "";
    device->serial = usb_device->serial ? usb_device->serial : "";
    device->configuration = usb_config->string ? usb_config->string : "";
    device->config.usb_device.manufacturer = device->config.usb_device.product = nullptr;
    device->config.usb_device.serial = nullptr;
    device->config.usb_config.string = nullptr;
    device->dirty = true;
    if (!device->batch_open)
        return commit_device(device, 0U, waits);
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.set_usb_identity")

mimhid_result MIMHID_CALL mimhid_device_commit(mimhid_device* device, uint32_t reconnect_delay_ms,
                                               const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    const mimhid_result res = commit_device(device, reconnect_delay_ms, waits);
    if (res == MIMHID_OK) {
        device->batch_open = false;
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        for (mimhid_node* node : device->slots) {
            if (node != nullptr)
                node->staged_only = false;
        }
    }
    return res;
}
MIMHID_C_RESULT_CATCH("device.commit")

mimhid_result MIMHID_CALL mimhid_device_connect(mimhid_device* device,
                                                const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (!device->configured) {
        return commit_device(device, 0U, waits);
    }
    return command_error(device->link.connect(waits.control), "device.connect",
                         "The board did not acknowledge connect.", MIMHID_T_COMMIT);
}
MIMHID_C_RESULT_CATCH("device.connect")

mimhid_result MIMHID_CALL mimhid_device_disconnect(mimhid_device* device,
                                                   const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    const mimhid_result r = command_error(
        device->link.disconnect(control_timeout_ms), "device.disconnect",
        "The board did not acknowledge disconnect.", MIMHID_T_COMMIT);
    if (r == MIMHID_OK) {
        device->configured = false;
    }
    return r;
}
MIMHID_C_RESULT_CATCH("device.disconnect")

mimhid_result MIMHID_CALL mimhid_device_close(mimhid_device* device,
                                              const uint32_t control_timeout_ms,
                                              const uint32_t drain_timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device handle is null.");
        return MIMHID_ERR_PARAM;
    }
    // A refused timeout consumes nothing.
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (const mimhid_result t = require_timeout(drain_timeout_ms, "drain_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (device->closing.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device is already closing.");
        return MIMHID_ERR_PARAM;
    }
    mimhid_result retained = MIMHID_OK;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(drain_timeout_ms);
    for (mimhid_node* node : live_nodes(device)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::uint32_t budget =
            left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 1U;
        const mimhid_result neutral = send_neutral(node, budget);
        if (neutral != MIMHID_OK && retained == MIMHID_OK)
            retained = neutral;
    }
    if (!device->link.failed() && device->control_routed.load(std::memory_order_acquire)) {
        // The next program finds the board answering everything itself.
        const std::uint8_t none[2] = {0U, 0U};
        (void)device->link.command(MIMHID_T_CONTROL_CONFIG, none, sizeof(none), control_timeout_ms,
                                   1);
    }
    if (!device->link.failed() && device->configured && device->config.transport.keep_attached == 0U) {
        const std::uint8_t detach[1] = {1U};
        const tr::CommandResult sent = device->link.command(
            MIMHID_T_COMMIT, detach, 1, control_timeout_ms, kCommandAttempts);
        if (sent.status != tr::CommandStatus::ack && retained == MIMHID_OK)
            retained = command_error(sent, "device.close", "The board did not go off the bus.",
                                     MIMHID_T_COMMIT);
    }
    // The next program to open the board, or a search, finds it at once on the boot rate.
    if (!device->link.failed() && device->info.baud_rate != MIMHID_BOOT_BAUD) {
        const std::uint8_t boot[1] = {0U};
        (void)device->link.command(MIMHID_T_SET_BAUD, boot, 1, control_timeout_ms, 1);
    }
    device->link.drain();
    device->link.close();
    for (mimhid_node*& node : device->slots) {
        if (node != nullptr) {
            if (node->spec != nullptr)
                mimhid::detail::release_spec(node->spec);
            delete node;
            node = nullptr;
        }
    }
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "device closed");
    delete device;
    if (retained == MIMHID_OK)
        mimhid::detail::clear_error();
    return retained;
}
MIMHID_C_RESULT_CATCH("device.close")

mimhid_result MIMHID_CALL mimhid_device_latched_error(mimhid_device* device) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMHID_ERR_NO_DEVICE;
    }
    const mimhid_result latched = device->latched.exchange(MIMHID_OK, std::memory_order_acq_rel);
    if (latched != MIMHID_OK)
        set_error(latched, "device", "A serial write failed earlier.");
    return latched;
}
MIMHID_C_RESULT_CATCH("device.latched_error")

int32_t MIMHID_CALL mimhid_device_is_ready(const mimhid_device* device) {
    return device != nullptr && device->link.ready() ? 1 : 0;
}

mimhid_result MIMHID_CALL mimhid_device_wait_ready(mimhid_device* device,
                                                   const uint32_t timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device handle is null.");
        return MIMHID_ERR_PARAM;
    }
    if (device->link.wait_ready(timeout_ms))
        return MIMHID_OK;
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMHID_ERR_NO_DEVICE;
    }
    set_error(MIMHID_ERR_TIMEOUT, "device.wait_ready",
              "The target did not mount the device in time.");
    return MIMHID_ERR_TIMEOUT;
}
MIMHID_C_RESULT_CATCH("device.wait_ready")

mimhid_result MIMHID_CALL mimhid_device_batch_begin(mimhid_device* device) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    if (device->batch_open) {
        set_error(MIMHID_ERR_BUSY, "device.batch", "A batch is already open.");
        return MIMHID_ERR_BUSY;
    }
    device->batch_open = true;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.batch_begin")

mimhid_result MIMHID_CALL mimhid_device_batch_end(mimhid_device* device,
                                                  const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (!device->batch_open) {
        set_error(MIMHID_ERR_BUSY, "device.batch", "No batch is open.");
        return MIMHID_ERR_BUSY;
    }
    device->batch_open = false;
    if (!device->dirty)
        return MIMHID_OK;
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        device->batch_open = true; // staged changes stay; the caller may retry or close nodes
        return committed;
    }
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        for (mimhid_node* node : device->slots) {
            if (node != nullptr)
                node->staged_only = false;
        }
    }
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.batch_end")

mimhid_result MIMHID_CALL mimhid_device_remote_wakeup(mimhid_device* device,
                                                      const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device handle is null.");
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    const tr::CommandResult result = device->link.command(
        MIMHID_T_REMOTE_WAKEUP, nullptr, 0, control_timeout_ms, kCommandAttempts);
    return command_error(result, "device.remote_wakeup", "The target has not suspended the device.",
                         MIMHID_T_REMOTE_WAKEUP);
}
MIMHID_C_RESULT_CATCH("device.remote_wakeup")

mimhid_result MIMHID_CALL mimhid_device_board_info(const mimhid_device* device,
                                                   mimhid_device_info* out_info) try {
    mimhid::detail::clear_error();
    if (device == nullptr || out_info == nullptr) {
        set_error(MIMHID_ERR_PARAM, "device", "A Device and an output pointer are required.");
        return MIMHID_ERR_PARAM;
    }
    *out_info = device->info;
    out_info->is_ready = device->link.ready() ? 1U : 0U;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.board_info")

mimhid_result MIMHID_CALL mimhid_device_limits(const mimhid_device* device,
                                               mimhid_board_limits* out_limits) try {
    mimhid::detail::clear_error();
    if (device == nullptr || out_limits == nullptr ||
        !mimhid::detail::valid_struct(out_limits, out_limits->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*out_limits)),
                                      "board_limits")) {
        set_error(MIMHID_ERR_PARAM, "device",
                  "A Device and an output with its struct_size set are required.");
        return MIMHID_ERR_PARAM;
    }
    if (!reserved_is_zero(out_limits->reserved, "board_limits.reserved"))
        return MIMHID_ERR_PARAM;
    out_limits->max_report_bytes = device->board.max_report_bytes;
    out_limits->max_feature_bytes = device->board.max_feature_bytes;
    out_limits->max_endpoint_bytes = device->board.max_endpoint_bytes;
    out_limits->max_frame_payload = device->board.max_frame_payload;
    out_limits->max_control_data_bytes = device->board.max_control_data_bytes;
    out_limits->max_control_rules = device->board.max_control_rules;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.limits")

// ---- control transfers of the target ---------------------------------------------------

mimhid_result MIMHID_CALL mimhid_device_set_control_routing(mimhid_device* device,
                                                            const uint32_t report_all,
                                                            const mimhid_control_rule* rules,
                                                            const size_t rule_count,
                                                            const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (!mimhid::detail::valid_boolean(report_all)) {
        set_error(MIMHID_ERR_PARAM, "report_all", "report_all must be 0 or 1.");
        return MIMHID_ERR_PARAM;
    }
    if (rule_count > MIMHID_CONTROL_MAX_RULES || (rule_count != 0U && rules == nullptr)) {
        set_error(MIMHID_ERR_PARAM, "rules", "The rules are missing or more than the API takes.");
        return MIMHID_ERR_PARAM;
    }
    if (rule_count > device->board.max_control_rules) {
        set_error(MIMHID_ERR_OVERFLOW, "rules", "The board holds fewer rules.", 0, MIMHID_T_CONTROL_CONFIG);
        return MIMHID_ERR_OVERFLOW;
    }
    std::uint8_t payload[2 + MIMHID_CONTROL_RULES * MIMHID_CONTROL_RULE_LEN];
    std::size_t n = 0;
    payload[n++] = static_cast<std::uint8_t>(report_all);
    payload[n++] = static_cast<std::uint8_t>(rule_count);
    for (std::size_t i = 0; i < rule_count; ++i) {
        const mimhid_control_rule& r = rules[i];
        if (r.timeout_ms == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, "rule.timeout_ms", "A nonzero timeout is required.");
            return MIMHID_ERR_UNSET_FIELD;
        }
        if (r.when_late == 0) {
            set_error(MIMHID_ERR_UNSET_FIELD, "rule.when_late",
                      "The action after the time ran out is required.");
            return MIMHID_ERR_UNSET_FIELD;
        }
        if (r.timeout_ms > MIMHID_CONTROL_MAX_TIMEOUT_MS || r.when_late < MIMHID_CONTROL_ACTION_LEAVE ||
            r.when_late > MIMHID_CONTROL_ACTION_DROP ||
            (r.request_type_value & ~r.request_type_mask) != 0 ||
            (r.request_value & ~r.request_mask) != 0 || (r.value_value & ~r.value_mask) != 0 ||
            (r.index_value & ~r.index_mask) != 0) {
            set_error(MIMHID_ERR_PARAM, "rule",
                      "A time outside 1 to MIMHID_CONTROL_MAX_TIMEOUT_MS, an unknown action, or a "
                      "value with bits outside its mask (it could never match).");
            return MIMHID_ERR_PARAM;
        }
        payload[n++] = r.request_type_mask;
        payload[n++] = r.request_type_value;
        payload[n++] = r.request_mask;
        payload[n++] = r.request_value;
        payload[n++] = static_cast<std::uint8_t>(r.value_mask & 0xFFU);
        payload[n++] = static_cast<std::uint8_t>(r.value_mask >> 8);
        payload[n++] = static_cast<std::uint8_t>(r.value_value & 0xFFU);
        payload[n++] = static_cast<std::uint8_t>(r.value_value >> 8);
        payload[n++] = static_cast<std::uint8_t>(r.index_mask & 0xFFU);
        payload[n++] = static_cast<std::uint8_t>(r.index_mask >> 8);
        payload[n++] = static_cast<std::uint8_t>(r.index_value & 0xFFU);
        payload[n++] = static_cast<std::uint8_t>(r.index_value >> 8);
        payload[n++] = static_cast<std::uint8_t>(r.timeout_ms & 0xFFU);
        payload[n++] = static_cast<std::uint8_t>(r.timeout_ms >> 8);
        payload[n++] = static_cast<std::uint8_t>(r.when_late - 1);
    }
    const mimhid_result result = command_error(
        device->link.command(MIMHID_T_CONTROL_CONFIG, payload, n, control_timeout_ms,
                             kCommandAttempts),
        "device.set_control_routing", "The board did not accept the control routing.",
        MIMHID_T_CONTROL_CONFIG);
    if (result == MIMHID_OK)
        device->control_routed.store(report_all != 0U || rule_count != 0U, std::memory_order_release);
    return result;
}
MIMHID_C_RESULT_CATCH("device.set_control_routing")

mimhid_result MIMHID_CALL mimhid_device_control_reply(mimhid_device* device, const uint32_t token,
                                                      const mimhid_control_action action,
                                                      const uint8_t* data, const size_t length,
                                                      const uint32_t send_timeout_ms) try {
    mimhid::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(send_timeout_ms, "send_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (token == 0U || token > 255U || action < MIMHID_CONTROL_ACTION_LEAVE ||
        action > MIMHID_CONTROL_ACTION_DROP ||
        (length != 0U && (data == nullptr || action != MIMHID_CONTROL_ACTION_ANSWER))) {
        set_error(MIMHID_ERR_PARAM, "reply",
                  "A token of 1 to 255, a known action, and data only with ANSWER are required.");
        return MIMHID_ERR_PARAM;
    }
    if (length > device->board.max_control_data_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "reply", "The data is longer than the board takes.", 0,
                  MIMHID_T_CONTROL_REPLY, 0, 0, 0, static_cast<std::uint32_t>(length));
        return MIMHID_ERR_OVERFLOW;
    }
    std::int32_t os_error = 0;
    const tr::IoStatus status = device->link.send_control_reply(
        static_cast<std::uint8_t>(token), static_cast<std::uint8_t>(action - 1), data, length,
        send_timeout_ms, &os_error);
    switch (status) {
    case tr::IoStatus::ok:
        return MIMHID_OK;
    case tr::IoStatus::timeout:
        set_error(MIMHID_ERR_TIMEOUT, "reply", "The serial write made no progress in time.", os_error,
                  MIMHID_T_CONTROL_REPLY);
        return MIMHID_ERR_TIMEOUT;
    case tr::IoStatus::short_write:
        set_error(MIMHID_ERR_SHORT_TRANSFER, "reply", "The serial write was cut short.", os_error,
                  MIMHID_T_CONTROL_REPLY);
        return MIMHID_ERR_SHORT_TRANSFER;
    case tr::IoStatus::closed:
        device->latched.store(MIMHID_ERR_NO_DEVICE, std::memory_order_release);
        set_error(MIMHID_ERR_NO_DEVICE, "reply", "The serial link failed or was unplugged.", os_error,
                  MIMHID_T_CONTROL_REPLY);
        return MIMHID_ERR_NO_DEVICE;
    case tr::IoStatus::error:
        break;
    }
    set_error(MIMHID_ERR_IO, "reply", "The serial write failed.", os_error, MIMHID_T_CONTROL_REPLY);
    return MIMHID_ERR_IO;
}
MIMHID_C_RESULT_CATCH("device.control_reply")

// ---- nodes -----------------------------------------------------------------------------

mimhid_result validate_node_config(const mimhid_device* device,
                                   const mimhid_node_config& effective) noexcept {
    if (!reserved_is_zero(effective.reserved, "node_config.reserved"))
        return MIMHID_ERR_PARAM;
    if (effective.ep_in.poll_interval_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.ep_in.poll_interval_ms",
                  "poll_interval_ms must be non-zero (1-255 ms).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (effective.ep_in.max_packet_size == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.ep_in.max_packet_size",
                  "ep_in.max_packet_size must be non-zero (1 to board max_endpoint_bytes).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (effective.iface.bcd_hid == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.iface.bcd_hid",
                  "bcd_hid must be explicitly specified (e.g. 0x0111).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (effective.iface.class_code == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.iface.class_code",
                  "class_code must be explicitly specified (e.g. 0x03 for HID).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if ((effective.ep_in.address & 0x80U) == 0U) {
        set_error(MIMHID_ERR_PARAM, "node_config.ep_in.address",
                  "ep_in.address must have bit 7 set for IN endpoint (e.g. 0x81).");
        return MIMHID_ERR_PARAM;
    }
    if (effective.iface.country_code > 35U || !valid_usb_string(effective.iface.name)) {
        set_error(MIMHID_ERR_PARAM, "node_config.iface",
                  "A string is not printable ASCII of at most 126 characters, or country_code > 35.");
        return MIMHID_ERR_PARAM;
    }
    if (effective.ep_in.max_packet_size > device->board.max_endpoint_bytes ||
        effective.ep_out.max_packet_size > device->board.max_endpoint_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "node_config.ep_in.max_packet_size",
                  "An endpoint packet is larger than the board's endpoints hold.", 0, 0, 0, 0,
                  device->board.max_endpoint_bytes,
                  std::max<std::uint32_t>(effective.ep_in.max_packet_size, effective.ep_out.max_packet_size));
        return MIMHID_ERR_OVERFLOW;
    }
    if (effective.iface.number >= device->board.max_interfaces) {
        set_error(MIMHID_ERR_OVERFLOW, "node_config.iface.number",
                  "The board has fewer HID interfaces.", 0, 0, 0, 0,
                  static_cast<std::uint32_t>(device->board.max_interfaces) - 1U,
                  effective.iface.number);
        return MIMHID_ERR_OVERFLOW;
    }
    if (effective.ep_out.max_packet_size > 0U || effective.ep_out.address != 0U) {
        if ((effective.ep_out.address & 0x80U) != 0U || effective.ep_out.address == 0U) {
            set_error(MIMHID_ERR_PARAM, "node_config.ep_out.address",
                      "ep_out.address must have bit 7 clear and nonzero (e.g. 0x01).");
            return MIMHID_ERR_PARAM;
        }
        if (effective.ep_out.poll_interval_ms == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, "node_config.ep_out.poll_interval_ms",
                      "poll_interval_ms must be non-zero (1-255 ms) when ep_out is enabled.");
            return MIMHID_ERR_UNSET_FIELD;
        }
        if (effective.ep_out.max_packet_size == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, "node_config.ep_out.max_packet_size",
                      "max_packet_size must be non-zero when ep_out is enabled.");
            return MIMHID_ERR_UNSET_FIELD;
        }
    }
    return MIMHID_OK;
}

mimhid_result MIMHID_CALL mimhid_node_open(mimhid_device* device, mimhid_spec* spec,
                                           const mimhid_node_config* config,
                                           const mimhid_commit_timeouts* timeouts,
                                           mimhid_node** out_node) try {
    mimhid::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMHID_ERR_PARAM, "out_node", "The output pointer is null.");
        return MIMHID_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr || spec == nullptr) {
        set_error(MIMHID_ERR_PARAM, "node.open", "A live Device and immutable Spec are required.");
        return MIMHID_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device handle was consumed by close.");
        return MIMHID_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMHID_ERR_NO_DEVICE;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (config == nullptr || !mimhid::detail::valid_struct(
                                 config, config->struct_size,
                                 static_cast<std::uint32_t>(sizeof(*config)), "node_config")) {
        return MIMHID_ERR_PARAM;
    }
    mimhid_node_config effective = *config;
    if (const mimhid_result v = validate_node_config(device, effective); v != MIMHID_OK)
        return v;

    const mimhid_transport_options& policy = device->config.transport;
    if (spec->descriptor.empty() || spec->descriptor.size() > device->board.max_descriptor_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.descriptor.board",
                  "The descriptor exceeds the board's descriptor memory.");
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->descriptor.size() > policy.linux_descriptor_policy_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.descriptor.linux_policy",
                  "The descriptor exceeds the caller's Linux descriptor byte policy.");
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->requirements.maximum_fields_per_report > policy.linux_hid_fields_per_report_policy) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.requirements.maximum_fields_per_report",
                  "The descriptor exceeds the caller-selected per-report field policy for the "
                  "exact target HID parser revision.",
                  0, 0, 0, 0, policy.linux_hid_fields_per_report_policy,
                  spec->requirements.maximum_fields_per_report);
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->requirements.maximum_global_stack_depth >
        policy.linux_hid_global_stack_depth_policy) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.requirements.maximum_global_stack_depth",
                  "The descriptor exceeds the caller-selected Global stack-depth policy for the "
                  "exact target HID parser revision.",
                  0, 0, 0, 0, policy.linux_hid_global_stack_depth_policy,
                  spec->requirements.maximum_global_stack_depth);
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->requirements.maximum_report_size_bits > policy.linux_hid_report_size_policy_bits) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.requirements.maximum_report_size_bits",
                  "The descriptor exceeds the caller-selected Report Size declaration policy for "
                  "the exact target HID parser revision.",
                  0, 0, 0, 0, policy.linux_hid_report_size_policy_bits,
                  spec->requirements.maximum_report_size_bits);
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->requirements.maximum_usages > policy.linux_hid_usages_policy) {
        set_error(
            MIMHID_ERR_OVERFLOW, "spec.requirements.maximum_usages",
            "The descriptor exceeds the caller-selected Usage policy for the exact target "
            "HID parser revision.",
            0, 0, 0, 0,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                policy.linux_hid_usages_policy, std::numeric_limits<std::uint32_t>::max())),
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                spec->requirements.maximum_usages, std::numeric_limits<std::uint32_t>::max())));
        return MIMHID_ERR_OVERFLOW;
    }
    if (spec->requirements.maximum_report_data_bits > policy.linux_hid_report_data_policy_bits) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.requirements.maximum_report_data_bits",
                  "The descriptor exceeds the caller-selected report data-bit policy for the "
                  "exact target HID parser revision.",
                  0, 0, 0, 0,
                  static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(policy.linux_hid_report_data_policy_bits,
                                              std::numeric_limits<std::uint32_t>::max())),
                  static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(spec->requirements.maximum_report_data_bits,
                                              std::numeric_limits<std::uint32_t>::max())));
        return MIMHID_ERR_OVERFLOW;
    }
    for (const auto& report : spec->report_capabilities) {
        if (report.wire_length > device->maximum_report_bytes) {
            set_error(MIMHID_ERR_OVERFLOW, "spec.report",
                      "A report exceeds the board's or the caller's report size limit.");
            return MIMHID_ERR_OVERFLOW;
        }
    }

    // Lowest free node id the board can hold.
    std::uint8_t node_id = 0xFFU;
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        const std::size_t limit = std::min<std::size_t>(device->board.max_nodes, MIMHID_MAX_NODES);
        for (std::size_t i = 0U; i < limit; ++i) {
            if (device->slots[i] == nullptr) {
                node_id = static_cast<std::uint8_t>(i);
                break;
            }
        }
    }
    if (node_id == 0xFFU) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open", "The board holds no more nodes.");
        return MIMHID_ERR_OVERFLOW;
    }

    auto node = std::unique_ptr<mimhid_node>(new (std::nothrow) mimhid_node());
    if (node == nullptr) {
        set_error(MIMHID_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMHID_ERR_INTERNAL;
    }
    node->device = device;
    node->spec = spec;
    node->config = effective;
    node->interface_string = effective.iface.name != nullptr ? effective.iface.name : "";
    node->config.iface.name = nullptr;
    node->node_id = node_id;
    collect_report_ids(spec->descriptor, &node->report_ids, &node->has_report_ids);
    if (const mimhid_result initialized = initialize_node_state(node.get());
        initialized != MIMHID_OK)
        return initialized;

    // A touch Spec that declares Contact Count Maximum answers the target's Feature read.
    if (const auto* touch = std::get_if<mimhid::detail::TouchConfig>(&spec->config);
        touch != nullptr && touch->fields.enable_contact_count_maximum_feature_declaration == 1U) {
        mimhid_node::FeatureEntry entry;
        entry.report_id =
            touch->fields.report_id.enabled == 1U ? touch->fields.report_id.value : 0U;
        const std::size_t bytes = (touch->fields.contact_count.bit_width + 7U) / 8U;
        for (std::size_t i = 0U; i < bytes; ++i)
            entry.data.push_back(
                static_cast<std::uint8_t>((touch->fields.maximum_contacts >> (8U * i)) & 0xFFU));
        node->features.push_back(std::move(entry));
    }

    mimhid::detail::retain_spec(spec);
    mimhid_node* published = node.get();
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        device->slots[node_id] = published;
    }
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMHID_OK;
    }
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        // The board's active configuration is unchanged or unknown; drop this node again.
        {
            std::lock_guard<std::mutex> lock(device->nodes_mutex);
            device->slots[node_id] = nullptr;
        }
        mimhid::detail::release_spec(spec);
        node->spec = nullptr;
        if (device->configured) {
            commit_device(device, 0U, waits);
        }
        return committed;
    }
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "node opened");
    *out_node = node.release();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.open")

mimhid_result MIMHID_CALL mimhid_node_open_raw(mimhid_device* device, const uint8_t* descriptor,
                                               size_t descriptor_length,
                                               const mimhid_node_config* config,
                                               const mimhid_commit_timeouts* timeouts,
                                               mimhid_node** out_node) try {
    mimhid::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMHID_ERR_PARAM, "out_node",
                  "A nonnull pointer to receive the Node is required.");
        return MIMHID_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr || descriptor == nullptr || descriptor_length == 0U) {
        set_error(MIMHID_ERR_PARAM, "node.open_raw", "A live Device and descriptor are required.");
        return MIMHID_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "device", "The Device handle was consumed by close.");
        return MIMHID_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMHID_ERR_NO_DEVICE;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (config == nullptr || !mimhid::detail::valid_struct(
                                 config, config->struct_size,
                                 static_cast<std::uint32_t>(sizeof(*config)), "node_config")) {
        return MIMHID_ERR_PARAM;
    }
    mimhid_node_config effective = *config;
    if (const mimhid_result v = validate_node_config(device, effective); v != MIMHID_OK)
        return v;

    if (descriptor_length > device->board.max_descriptor_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "descriptor.board",
                  "The descriptor exceeds the board's descriptor memory.");
        return MIMHID_ERR_OVERFLOW;
    }

    auto spec = std::unique_ptr<mimhid_spec>(new (std::nothrow) mimhid_spec());
    if (spec == nullptr) {
        set_error(MIMHID_ERR_INTERNAL, "spec", "Memory allocation for the Spec failed.");
        return MIMHID_ERR_INTERNAL;
    }
    spec->references.store(1U, std::memory_order_relaxed);
    spec->kind = MIMHID_PROFILE_RAW;
    spec->descriptor.assign(descriptor, descriptor + descriptor_length);

    std::uint8_t node_id = 0xFFU;
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        const std::size_t limit = std::min<std::size_t>(device->board.max_nodes, MIMHID_MAX_NODES);
        for (std::size_t i = 0U; i < limit; ++i) {
            if (device->slots[i] == nullptr) {
                node_id = static_cast<std::uint8_t>(i);
                break;
            }
        }
    }
    if (node_id == 0xFFU) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open_raw", "The board holds no more nodes.");
        return MIMHID_ERR_OVERFLOW;
    }

    auto node = std::unique_ptr<mimhid_node>(new (std::nothrow) mimhid_node());
    if (node == nullptr) {
        set_error(MIMHID_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMHID_ERR_INTERNAL;
    }
    node->device = device;
    node->spec = spec.release();
    node->config = effective;
    node->interface_string = effective.iface.name != nullptr ? effective.iface.name : "";
    node->config.iface.name = nullptr;
    node->node_id = node_id;
    collect_report_ids(node->spec->descriptor, &node->report_ids, &node->has_report_ids);
    if (const mimhid_result initialized = initialize_node_state(node.get());
        initialized != MIMHID_OK) {
        mimhid::detail::release_spec(node->spec);
        node->spec = nullptr;
        return initialized;
    }

    mimhid_node* published = node.get();
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        device->slots[node_id] = published;
    }
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMHID_OK;
    }
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        {
            std::lock_guard<std::mutex> lock(device->nodes_mutex);
            device->slots[node_id] = nullptr;
        }
        mimhid::detail::release_spec(node->spec);
        node->spec = nullptr;
        if (device->configured) {
            commit_device(device, 0U, waits);
        }
        return committed;
    }
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "raw node opened");
    *out_node = node.release();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.open_raw")

mimhid_result MIMHID_CALL mimhid_node_set_feature_response(mimhid_node* node,
                                                           const uint8_t report_id,
                                                           const uint8_t* data, const size_t length,
                                                           const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        (data == nullptr && length != 0U) ||
        length > node->device->board.max_feature_bytes) {
        set_error(MIMHID_ERR_PARAM, "node.feature",
                  "A live Node and a response of at most the board's max_feature_bytes are "
                  "required.");
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    mimhid_device* device = node->device;
    mimhid_node::FeatureEntry* entry = nullptr;
    for (auto& existing : node->features) {
        if (existing.report_id == report_id)
            entry = &existing;
    }
    if (entry == nullptr) {
        node->features.push_back(mimhid_node::FeatureEntry{report_id, {}});
        entry = &node->features.back();
    }
    entry->data.assign(data, data + length);
    if (!device->configured || device->batch_open || node->staged_only)
        return MIMHID_OK; // sent with the next commit
    std::vector<std::uint8_t> payload;
    payload.push_back(node->node_id);
    payload.push_back(report_id);
    payload.insert(payload.end(), entry->data.begin(), entry->data.end());
    return command_error(device->link.command(MIMHID_T_FEATURE, payload.data(), payload.size(),
                                              control_timeout_ms, kCommandAttempts),
                         "node.feature", "The board did not accept the Feature report answer.",
                         MIMHID_T_FEATURE, public_node_id(node));
}
MIMHID_C_RESULT_CATCH("node.set_feature_response")

uint16_t MIMHID_CALL mimhid_node_id(const mimhid_node* node) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed) {
        set_error(MIMHID_ERR_PARAM, "node", "A live Node is required.");
        return 0U;
    }
    return public_node_id(node);
}
MIMHID_C_VALUE_CATCH("node.id", 0U)

mimhid_result MIMHID_CALL mimhid_node_manifest(const mimhid_node* node,
                                               mimhid_capability_manifest* manifest) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->spec == nullptr) {
        set_error(MIMHID_ERR_PARAM, "node", "A live Node is required.");
        return MIMHID_ERR_PARAM;
    }
    return mimhid_spec_manifest(node->spec, manifest);
}
MIMHID_C_RESULT_CATCH("node.manifest")

} // extern "C"

namespace {

// Writes one serialized report and settles the node state machine with the outcome, exactly
// as the completion of an asynchronous transfer would. A negative native status means that no
// byte reached the serial driver, so the pending state is kept for a resend.
mimhid_result write_report(mimhid_node* node, const std::uint8_t* report, std::size_t length,
                           const char* field, std::uint32_t send_timeout_ms) noexcept {
    mimhid_device* device = node->device;
    std::int32_t os_error = 0;
    const tr::IoStatus status = device->link.send_report(
        node->node_id, report, length, send_timeout_ms, &os_error);
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
        const mimhid_result submit =
            mimhid_node_submit(node, left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 1U);
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
    if (const mimhid_result t = require_timeout(send_timeout_ms, "send_timeout_ms");
        t != MIMHID_OK)
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
    const tr::IoStatus status = device->link.send_report(
        node->node_id, report_bytes, report_length, send_timeout_ms, &os_error);
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

mimhid_result MIMHID_CALL mimhid_node_close(mimhid_node* node,
                                            const mimhid_commit_timeouts* timeouts,
                                            const uint32_t drain_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, "node.close",
                  "The Node handle is null, closed, detached, or owned by a closing Device.");
        return MIMHID_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (const mimhid_result t = require_timeout(drain_timeout_ms, "drain_timeout_ms");
        t != MIMHID_OK)
        return t;
    mimhid_device* device = node->device;
    if (const mimhid_result neutral = send_neutral(node, drain_timeout_ms);
        neutral != MIMHID_OK) {
        return neutral;
    }
    // Take the node out of the configuration the board gets, and restore it if that fails.
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        device->slots[node->node_id] = nullptr;
    }
    device->dirty = true;
    if (!device->batch_open && !node->staged_only) {
        const mimhid_result committed = commit_device(device, 0U, waits);
        if (committed != MIMHID_OK) {
            std::lock_guard<std::mutex> lock(device->nodes_mutex);
            device->slots[node->node_id] = node;
            return committed;
        }
    }
    node->closed = true;
    mimhid::detail::release_spec(node->spec);
    node->spec = nullptr;
    delete node;
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "node closed");
    mimhid::detail::clear_error();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.close")

} // extern "C"
