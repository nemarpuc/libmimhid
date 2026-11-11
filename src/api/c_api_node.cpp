// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Public C ABI: opening, configuring and closing Nodes.
 */

#include "api/support.hpp"
#include "usb/hid_layer.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <utility>

namespace tr = mimhid::transport;

using mimhid::detail::collect_report_ids;
using mimhid::detail::command_error;
using mimhid::detail::commit_device;
using mimhid::detail::CommitWaits;
using mimhid::detail::has_non_neutral_state;
using mimhid::detail::kCommandAttempts;
using mimhid::detail::make_neutral;
using mimhid::detail::public_node_id;
using mimhid::detail::read_commit_timeouts;
using mimhid::detail::report_id;
using mimhid::detail::require_timeout;
using mimhid::detail::reserved_is_zero;
using mimhid::detail::saturate_u32;
using mimhid::detail::send_neutral;
using mimhid::detail::set_error;
using mimhid::detail::touch_frame_in_progress;
using mimhid::detail::valid_usb_string;

namespace {

const std::vector<std::uint8_t> kNoDescriptor;

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

} // namespace

namespace {

// What only a HID Node has to satisfy; the endpoints themselves (addresses, packet sizes, ...) are
// checked by the composer, which sees every endpoint of the device.
mimhid_result validate_hid_node_config(const mimhid_device* device,
                                       const mimhid_hid_node_config& effective) noexcept {
    if (!reserved_is_zero(effective.reserved, "node_config.reserved"))
        return MIMHID_ERR_PARAM;
    if (effective.reserved16 != 0U) {
        set_error(MIMHID_ERR_PARAM, "node_config.reserved16", "The reserved field must be zero.");
        return MIMHID_ERR_PARAM;
    }
    if (effective.bcd_hid == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.bcd_hid",
                  "bcd_hid must be explicitly specified (e.g. 0x0111).");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (effective.interface_number >= device->board.max_interfaces) {
        set_error(MIMHID_ERR_OVERFLOW, "node_config.interface_number",
                  "The board has fewer HID interfaces.", 0, 0, 0, 0,
                  static_cast<std::uint32_t>(device->board.max_interfaces) - 1U,
                  effective.interface_number);
        return MIMHID_ERR_OVERFLOW;
    }
    if (effective.subclass > 1U || effective.protocol > 2U) {
        // HID 1.11 4.2 and 4.3: subclass 0 or 1; protocol 0 (none), 1 (keyboard) or 2 (mouse).
        set_error(MIMHID_ERR_PARAM, "node_config",
                  "subclass is 0 or 1 and protocol 0, 1 or 2 (HID 1.11 4.2, 4.3).");
        return MIMHID_ERR_PARAM;
    }
    if (effective.country_code > 35U || !valid_usb_string(effective.name)) {
        set_error(MIMHID_ERR_PARAM, "node_config",
                  "A string is not UTF-8 text of at most 126 UTF-16 code units without control "
                  "characters, or country_code > 35.");
        return MIMHID_ERR_PARAM;
    }
    // HID 1.11 4.4: a HID interface's endpoints are interrupt endpoints. The IN endpoint's queue
    // is the board's HID driver's, which drops the newest report when it is full; the other
    // policies need the generic driver (a later release).
    const bool has_out = effective.ep_out.address != 0U || effective.ep_out.max_packet_size != 0U ||
                         effective.ep_out.transfer_type != 0U || effective.ep_out.interval != 0U;
    for (const mimhid_usb_endpoint* ep :
         {&effective.ep_in, has_out ? &effective.ep_out : nullptr}) {
        if (ep == nullptr)
            continue;
        const char* field = ep == &effective.ep_in ? "node_config.ep_in.transfer_type"
                                                   : "node_config.ep_out.transfer_type";
        if (ep->transfer_type == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, field,
                      "transfer_type is required: a HID endpoint is MIMHID_TRANSFER_INTERRUPT.");
            return MIMHID_ERR_UNSET_FIELD;
        }
        if (ep->transfer_type != MIMHID_TRANSFER_INTERRUPT) {
            set_error(MIMHID_ERR_UNSUPPORTED, field,
                      "The endpoints of a HID Node are interrupt endpoints.");
            return MIMHID_ERR_UNSUPPORTED;
        }
    }
    if (effective.ep_in.policy.on_full == 0U || effective.ep_in.policy.depth == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "node_config.ep_in.policy",
                  "The IN endpoint needs a policy: on_full and depth are required.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (effective.ep_in.policy.on_full > MIMHID_FULL_WAIT) {
        set_error(MIMHID_ERR_PARAM, "node_config.ep_in.policy",
                  "on_full is MIMHID_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.");
        return MIMHID_ERR_PARAM;
    }
    if (effective.ep_in.policy.on_full != MIMHID_FULL_DROP_NEWEST) {
        set_error(MIMHID_ERR_UNSUPPORTED, "node_config.ep_in.policy.on_full",
                  "The IN endpoint of a HID Node takes MIMHID_FULL_DROP_NEWEST only in this "
                  "release.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (device->board.usb_high_speed &&
        (effective.ep_in.interval > 16U || effective.ep_out.interval > 16U)) {
        // USB 2.0 9.6.6: at high speed bInterval of an interrupt endpoint is an exponent, 1 to 16.
        set_error(MIMHID_ERR_PARAM, "node_config.ep_in.interval",
                  "A high-speed board takes a bInterval of 1 to 16 (USB 2.0 9.6.6).");
        return MIMHID_ERR_PARAM;
    }
    // The IN endpoint is checked for the board's limits and USB's rules like any other.
    if (effective.ep_in.address == 0U || (effective.ep_in.address & 0x80U) == 0U) {
        set_error(MIMHID_ERR_PARAM, "node_config.ep_in.address",
                  "ep_in.address is 0x81 to 0x8F: bit 7 set, endpoint number 1 to 15.");
        return MIMHID_ERR_PARAM;
    }
    if (has_out && (effective.ep_out.address & 0x80U) != 0U) {
        set_error(MIMHID_ERR_PARAM, "node_config.ep_out.address",
                  "ep_out.address is 0x01 to 0x0F: bit 7 clear, endpoint number 1 to 15.");
        return MIMHID_ERR_PARAM;
    }
    // The endpoints and the interface alone, as the composer will see them with the others.
    mimhid::usb::HidNodeInput one;
    one.config = effective;
    one.report_descriptor = &kNoDescriptor;
    std::vector<mimhid::usb::ComposerGroup> groups;
    mimhid::usb::ComposerError error;
    if (mimhid::usb::compose_hid_groups({one}, &groups, &error) != MIMHID_OK) {
        set_error(error.result, error.field, error.reason, 0, 0, 0, 0, error.offset, error.length);
        return error.result;
    }
    return mimhid::detail::precheck_groups(device, std::move(groups));
}

} // namespace

namespace mimhid::detail {

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

} // namespace mimhid::detail

extern "C" {

// ---- nodes -----------------------------------------------------------------------------

mimhid_result MIMHID_CALL mimhid_node_open(mimhid_device* device, const mimhid_node_config* config,
                                           const mimhid_commit_timeouts* timeouts,
                                           mimhid_node** out_node) try {
    mimhid::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMHID_ERR_PARAM, "out_node", "The output pointer is null.");
        return MIMHID_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr) {
        set_error(MIMHID_ERR_PARAM, "node.open", "A live Device is required.");
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
    if (!mimhid::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimhid_node_config)),
                                      "node_config")) {
        return MIMHID_ERR_PARAM;
    }
    if (!reserved_is_zero(config->reserved, "node_config.reserved"))
        return MIMHID_ERR_PARAM;
    if (config->interfaces == nullptr || config->interface_count == 0U ||
        config->interface_count > device->board.max_interfaces_any ||
        (config->association == nullptr) != (config->association_length == 0U)) {
        set_error(MIMHID_ERR_PARAM, "node_config.interfaces",
                  "A Node has one or more interfaces (at most the board's max_interfaces_any); "
                  "association and association_length go together.");
        return MIMHID_ERR_PARAM;
    }
    auto node = std::unique_ptr<mimhid_node>(new (std::nothrow) mimhid_node());
    if (node == nullptr) {
        set_error(MIMHID_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMHID_ERR_INTERNAL;
    }
    node->generic = true;
    node->device = device;
    if (config->association != nullptr)
        node->association.assign(config->association,
                                 config->association + config->association_length);
    for (std::size_t i = 0U; i < config->interface_count; ++i) {
        const mimhid_usb_interface& in = config->interfaces[i];
        // The class is the caller's: the library reads it only for these two refusals. Class 0 is
        // "defined at the device level" (USB 2.0 9.6.5 / the class codes of the USB-IF), never an
        // interface the caller forgot; HID is the board's HID driver's.
        if (in.class_code == 0U) {
            set_error(MIMHID_ERR_UNSET_FIELD, "node_config.interfaces.class_code",
                      "class_code is required.", 0, 0, 0, 0, in.number, 0);
            return MIMHID_ERR_UNSET_FIELD;
        }
        if (in.class_code == 0x03U) {
            set_error(MIMHID_ERR_UNSUPPORTED, "node_config.interfaces.class_code",
                      "HID (0x03) goes through mimhid_node_open_profile in this release.", 0, 0, 0,
                      0, in.number, 0);
            return MIMHID_ERR_UNSUPPORTED;
        }
        if ((in.endpoints == nullptr && in.endpoint_count != 0U) ||
            (in.class_descriptors == nullptr && in.class_descriptors_length != 0U) ||
            in.endpoint_count > device->board.max_endpoint_number * 2U) {
            set_error(MIMHID_ERR_PARAM, "node_config.interfaces",
                      "endpoints and class_descriptors must not be null with a nonzero count, and "
                      "an interface has no more endpoints than the board has.",
                      0, 0, 0, 0, in.number, 0);
            return MIMHID_ERR_PARAM;
        }
        if (!valid_usb_string(in.name)) {
            set_error(MIMHID_ERR_PARAM, "node_config.interfaces.name",
                      "The name must be UTF-8 without control characters, at most 126 UTF-16 code "
                      "units.");
            return MIMHID_ERR_PARAM;
        }
        mimhid::detail::GenericInterface copy;
        copy.number = in.number;
        copy.class_code = in.class_code;
        copy.subclass = in.subclass;
        copy.protocol = in.protocol;
        copy.name = in.name != nullptr ? in.name : "";
        if (in.class_descriptors_length != 0U)
            copy.class_descriptors.assign(in.class_descriptors,
                                          in.class_descriptors + in.class_descriptors_length);
        if (in.endpoint_count != 0U)
            copy.endpoints.assign(in.endpoints, in.endpoints + in.endpoint_count);
        for (const mimhid_usb_endpoint& ep : copy.endpoints) {
            if ((ep.address & 0x80U) != 0U && ep.policy.on_full != 0U && ep.policy.depth != 0U) {
                mimhid::detail::EndpointRuntime runtime;
                runtime.address = ep.address;
                runtime.max_packet_size = ep.max_packet_size;
                runtime.policy = ep.policy;
                node->in_endpoints.push_back(runtime);
            }
        }
        node->interfaces.push_back(std::move(copy));
    }
    // The interface checks of the composer on this Node alone; the ones across Nodes (numbers
    // without a gap, a repeated address) are made at the commit, before anything is sent.
    {
        std::vector<mimhid::usb::ComposerGroup> groups(1);
        groups[0].association = node->association;
        for (const mimhid::detail::GenericInterface& itf : node->interfaces) {
            mimhid::usb::ComposerInterface made;
            made.number = itf.number;
            made.class_code = itf.class_code;
            made.subclass = itf.subclass;
            made.protocol = itf.protocol;
            mimhid::detail::utf16_from_utf8(itf.name.c_str(), &made.name);
            made.class_descriptors = itf.class_descriptors;
            made.endpoints = itf.endpoints;
            groups[0].interfaces.push_back(std::move(made));
        }
        if (const mimhid_result r = mimhid::detail::precheck_groups(device, std::move(groups));
            r != MIMHID_OK)
            return r;
    }
    std::uint16_t node_id = 0U;
    if (!mimhid::detail::free_node_id(device, &node_id)) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open", "The Device holds no more Nodes.");
        return MIMHID_ERR_OVERFLOW;
    }
    node->node_id = node_id;
    node->iface = node->interfaces.front().number;

    mimhid_node* published = node.get();
    mimhid::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMHID_OK;
    }
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        mimhid::detail::unpublish_node(device, published);
        if (device->configured)
            commit_device(device, 0U, waits);
        return committed;
    }
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "generic node opened");
    *out_node = node.release();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.open")

mimhid_result MIMHID_CALL mimhid_node_open_profile(mimhid_device* device, mimhid_spec* spec,
                                                   const mimhid_hid_node_config* config,
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
    if (!mimhid::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimhid_hid_node_config)),
                                      "node_config")) {
        return MIMHID_ERR_PARAM;
    }
    mimhid_hid_node_config effective = *config;
    if (const mimhid_result v = validate_hid_node_config(device, effective); v != MIMHID_OK)
        return v;

    if (spec->descriptor.empty() || spec->descriptor.size() > device->board.max_descriptor_bytes) {
        set_error(MIMHID_ERR_OVERFLOW, "spec.descriptor.board",
                  "The descriptor exceeds the board's descriptor memory.");
        return MIMHID_ERR_OVERFLOW;
    }
    struct Limit {
        const char* field;
        const char* reason;
        std::uint64_t allowed;
        std::uint64_t needed;
    };
    const mimhid_linux_hid_policy& linux_hid = device->config.transport.linux_hid;
    const mimhid::hid::DescriptorRequirements& needs = spec->requirements;
    const std::array<Limit, 6> limits{{
        {"spec.descriptor.linux_hid", "The descriptor exceeds linux_hid.descriptor_bytes.",
         linux_hid.descriptor_bytes, spec->descriptor.size()},
        {"spec.requirements.maximum_fields_per_report",
         "A report has more fields than linux_hid.fields_per_report.", linux_hid.fields_per_report,
         needs.maximum_fields_per_report},
        {"spec.requirements.maximum_global_stack_depth",
         "The descriptor pushes deeper than linux_hid.global_stack_depth.",
         linux_hid.global_stack_depth, needs.maximum_global_stack_depth},
        {"spec.requirements.maximum_report_size_bits",
         "A Report Size is larger than linux_hid.report_size_bits.", linux_hid.report_size_bits,
         needs.maximum_report_size_bits},
        {"spec.requirements.maximum_usages",
         "The descriptor has more Usages than linux_hid.usages.", linux_hid.usages,
         needs.maximum_usages},
        {"spec.requirements.maximum_report_data_bits",
         "A report is larger than linux_hid.report_data_bits.", linux_hid.report_data_bits,
         needs.maximum_report_data_bits},
    }};
    for (const Limit& limit : limits) {
        if (limit.needed > limit.allowed) {
            set_error(MIMHID_ERR_OVERFLOW, limit.field, limit.reason, 0, 0, 0, 0,
                      saturate_u32(limit.allowed), saturate_u32(limit.needed));
            return MIMHID_ERR_OVERFLOW;
        }
    }
    for (const auto& report : spec->report_capabilities) {
        if (report.wire_length > device->maximum_report_bytes) {
            set_error(MIMHID_ERR_OVERFLOW, "spec.report",
                      "A report exceeds the board's or the caller's report size limit.");
            return MIMHID_ERR_OVERFLOW;
        }
    }

    std::uint16_t node_id = 0U;
    if (!mimhid::detail::free_node_id(device, &node_id)) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open", "The Device holds no more Nodes.");
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
    node->interface_string = effective.name != nullptr ? effective.name : "";
    node->config.name = nullptr;
    node->node_id = node_id;
    node->iface = effective.interface_number;
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
    mimhid::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMHID_OK;
    }
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        // The board's active configuration is unchanged or unknown; drop this node again.
        mimhid::detail::unpublish_node(device, published);
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
MIMHID_C_RESULT_CATCH("node.open_profile")

mimhid_result MIMHID_CALL mimhid_node_open_raw(mimhid_device* device, const uint8_t* descriptor,
                                               size_t descriptor_length,
                                               const mimhid_hid_node_config* config,
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
    if (!mimhid::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimhid_hid_node_config)),
                                      "node_config")) {
        return MIMHID_ERR_PARAM;
    }
    mimhid_hid_node_config effective = *config;
    if (const mimhid_result v = validate_hid_node_config(device, effective); v != MIMHID_OK)
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

    std::uint16_t node_id = 0U;
    if (!mimhid::detail::free_node_id(device, &node_id)) {
        set_error(MIMHID_ERR_OVERFLOW, "node.open_raw", "The Device holds no more Nodes.");
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
    node->interface_string = effective.name != nullptr ? effective.name : "";
    node->config.name = nullptr;
    node->node_id = node_id;
    node->iface = effective.interface_number;
    collect_report_ids(node->spec->descriptor, &node->report_ids, &node->has_report_ids);
    if (const mimhid_result initialized = initialize_node_state(node.get());
        initialized != MIMHID_OK) {
        mimhid::detail::release_spec(node->spec);
        node->spec = nullptr;
        return initialized;
    }

    mimhid_node* published = node.get();
    mimhid::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMHID_OK;
    }
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        mimhid::detail::unpublish_node(device, published);
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

mimhid_result MIMHID_CALL mimhid_node_set_feature_response(
    mimhid_node* node, const uint8_t report_id,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const uint8_t* data, const size_t length, const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        (data == nullptr && length != 0U) || length > node->device->board.max_feature_bytes) {
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
    payload.push_back(node->iface); // the interface number; the answer belongs to the interface
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
    // From here the Node goes whatever fails; the first failure is what the caller hears about.
    const mimhid_result neutral = send_neutral(node, drain_timeout_ms);
    const mimhid_error_detail neutral_detail = *mimhid_last_error();
    mimhid::detail::unpublish_node(device, node);
    device->dirty = true;
    mimhid_result committed = MIMHID_OK;
    if (!device->batch_open && !node->staged_only)
        committed = commit_device(device, 0U, waits);
    node->closed = true;
    mimhid::detail::release_spec(node->spec);
    node->spec = nullptr;
    delete node;
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "node closed");
    if (neutral != MIMHID_OK) {
        mimhid::detail::restore_error(neutral_detail);
        return neutral;
    }
    if (committed != MIMHID_OK)
        return committed;
    mimhid::detail::clear_error();
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.close")

} // extern "C"
