// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: opening, configuring and closing Nodes.
 */

#include "api/support.hpp"
#include "usb/hid_layer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace tr = mimusb::transport;

using mimusb::detail::collect_report_ids;
using mimusb::detail::command_error;
using mimusb::detail::commit_device;
using mimusb::detail::CommitWaits;
using mimusb::detail::has_non_neutral_state;
using mimusb::detail::kCommandAttempts;
using mimusb::detail::make_neutral;
using mimusb::detail::public_node_id;
using mimusb::detail::read_commit_timeouts;
using mimusb::detail::report_id;
using mimusb::detail::require_timeout;
using mimusb::detail::reserved_is_zero;
using mimusb::detail::saturate_u32;
using mimusb::detail::send_neutral;
using mimusb::detail::set_error;
using mimusb::detail::touch_frame_in_progress;
using mimusb::detail::valid_usb_string;

namespace {

const std::vector<std::uint8_t> kNoDescriptor;

mimusb_result initialize_node_state(mimusb_node* node) {
    switch (node->spec->kind) {
    case MIMUSB_PROFILE_KEYBOARD: {
        const auto* spec = std::get_if<mimusb::detail::KeyboardConfig>(&node->spec->config);
        mimusb::detail::KeyboardState value{};
        const std::size_t key_count =
            static_cast<std::size_t>(spec->options.usage_maximum - spec->options.usage_minimum) +
            1U;
        value.key_transitions.resize(key_count);
        value.pressed_bitmap.resize(key_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_MOUSE: {
        const auto* spec = std::get_if<mimusb::detail::MouseConfig>(&node->spec->config);
        mimusb::detail::MouseState value{};
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_TOGGLE: {
        const auto* spec = std::get_if<mimusb::detail::ConsumerConfig>(&node->spec->config);
        mimusb::detail::ConsumerState value{};
        value.pressed.resize(spec->allowed_usages.size());
        value.transitions.resize(spec->allowed_usages.size());
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_GAMEPAD: {
        const auto* spec = std::get_if<mimusb::detail::GamepadConfig>(&node->spec->config);
        mimusb::detail::GamepadState value{};
        value.axes.reserve(spec->axes.size());
        for (const auto& axis : spec->axes)
            value.axes.push_back(axis.axis.neutral_value);
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_TOUCHSCREEN:
    case MIMUSB_PROFILE_TOUCHPAD: {
        const auto* spec = std::get_if<mimusb::detail::TouchConfig>(&node->spec->config);
        mimusb::detail::TouchState value{};
        value.buttons.resize(spec->fields.button_count);
        value.button_transitions.resize(spec->fields.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_PEN:
    case MIMUSB_PROFILE_DIGITIZER:
        node->state = mimusb::detail::PenState{};
        break;
    case MIMUSB_PROFILE_RAW:
        node->state = mimusb::detail::RawState{};
        node->dirty = false;
        break;
    default:
        set_error(MIMUSB_ERR_INTERNAL, "spec.kind",
                  "A validated ProfileSpec contained an unknown profile kind.");
        return MIMUSB_ERR_INTERNAL;
    }
    return MIMUSB_OK;
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
#define MIMUSB_NOINLINE __declspec(noinline)
#else
#define MIMUSB_NOINLINE __attribute__((noinline))
#endif

MIMUSB_NOINLINE bool neutralize_keyboard(mimusb::detail::KeyboardState* keyboard) noexcept {
    const bool had_non_neutral_state =
        keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    std::fill(keyboard->pressed_bitmap.begin(), keyboard->pressed_bitmap.end(), std::uint8_t{0});
    keyboard->pressed_bitmap_count = 0U;
    keyboard->modifiers = 0U;
    return had_non_neutral_state;
}

MIMUSB_NOINLINE bool neutralize_mouse(mimusb::detail::MouseState* mouse) noexcept {
    const bool had_non_neutral_state =
        std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                    [](const std::uint8_t value) { return value != 0U; });
    std::fill(mouse->buttons.begin(), mouse->buttons.end(), std::uint8_t{0});
    mouse->dx = mouse->dy = mouse->wheel = mouse->pan = 0;
    return had_non_neutral_state;
}

MIMUSB_NOINLINE bool neutralize_gamepad(mimusb::detail::GamepadState* gamepad,
                                        const mimusb::detail::GamepadConfig* spec) noexcept {
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

MIMUSB_NOINLINE bool neutralize_touch(mimusb::detail::TouchState* touch) noexcept {
    bool had_non_neutral_state = false;
    for (auto& contact : touch->contacts) {
        had_non_neutral_state =
            had_non_neutral_state || contact.phase != mimusb::detail::ContactPhase::none;
        if (contact.phase == mimusb::detail::ContactPhase::down) {
            contact.phase = mimusb::detail::ContactPhase::up;
        }
    }
    had_non_neutral_state =
        had_non_neutral_state || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    std::fill(touch->buttons.begin(), touch->buttons.end(), std::uint8_t{0});
    touch->packet_cursor = 0U;
    return had_non_neutral_state;
}

MIMUSB_NOINLINE bool neutralize_pen(mimusb::detail::PenState* pen) noexcept {
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
mimusb_result validate_hid_node_config(const mimusb_device* device,
                                       const mimusb_hid_node_config& effective) noexcept {
    if (!reserved_is_zero(effective.reserved, "node_config.reserved"))
        return MIMUSB_ERR_PARAM;
    if (effective.reserved16 != 0U) {
        set_error(MIMUSB_ERR_PARAM, "node_config.reserved16", "The reserved field must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if (effective.bcd_hid == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.bcd_hid",
                  "bcd_hid must be explicitly specified (e.g. 0x0111).");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (effective.interface_number >= device->board.max_interfaces_any) {
        set_error(MIMUSB_ERR_OVERFLOW, "node_config.interface_number",
                  "The board has fewer interfaces.", 0, 0, 0, 0,
                  static_cast<std::uint32_t>(device->board.max_interfaces_any) - 1U,
                  effective.interface_number);
        return MIMUSB_ERR_OVERFLOW;
    }
    if (effective.subclass > 1U || effective.protocol > 2U) {
        // HID 1.11 4.2 and 4.3: subclass 0 or 1; protocol 0 (none), 1 (keyboard) or 2 (mouse).
        set_error(MIMUSB_ERR_PARAM, "node_config",
                  "subclass is 0 or 1 and protocol 0, 1 or 2 (HID 1.11 4.2, 4.3).");
        return MIMUSB_ERR_PARAM;
    }
    if (effective.country_code > 35U || !valid_usb_string(effective.name)) {
        set_error(MIMUSB_ERR_PARAM, "node_config",
                  "A string is not UTF-8 text of at most 126 UTF-16 code units without control "
                  "characters, or country_code > 35.");
        return MIMUSB_ERR_PARAM;
    }
    // HID 1.11 4.4: a HID interface's endpoints are interrupt endpoints. The IN endpoint is one of
    // the generic driver's, with any of the three policies.
    const bool has_out = effective.ep_out.address != 0U || effective.ep_out.max_packet_size != 0U ||
                         effective.ep_out.transfer_type != 0U || effective.ep_out.interval != 0U;
    for (const mimusb_usb_endpoint* ep :
         {&effective.ep_in, has_out ? &effective.ep_out : nullptr}) {
        if (ep == nullptr)
            continue;
        const char* field = ep == &effective.ep_in ? "node_config.ep_in.transfer_type"
                                                   : "node_config.ep_out.transfer_type";
        if (ep->transfer_type == 0U) {
            set_error(MIMUSB_ERR_UNSET_FIELD, field,
                      "transfer_type is required: a HID endpoint is MIMUSB_TRANSFER_INTERRUPT.");
            return MIMUSB_ERR_UNSET_FIELD;
        }
        if (ep->transfer_type != MIMUSB_TRANSFER_INTERRUPT) {
            set_error(MIMUSB_ERR_UNSUPPORTED, field,
                      "The endpoints of a HID Node are interrupt endpoints.");
            return MIMUSB_ERR_UNSUPPORTED;
        }
    }
    if (effective.ep_in.policy.on_full == 0U || effective.ep_in.policy.depth == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.ep_in.policy",
                  "The IN endpoint needs a policy: on_full and depth are required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (effective.ep_in.policy.on_full > MIMUSB_FULL_WAIT) {
        set_error(MIMUSB_ERR_PARAM, "node_config.ep_in.policy",
                  "on_full is MIMUSB_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->board.usb_high_speed &&
        (effective.ep_in.interval > 16U || effective.ep_out.interval > 16U)) {
        // USB 2.0 9.6.6: at high speed bInterval of an interrupt endpoint is an exponent, 1 to 16.
        set_error(MIMUSB_ERR_PARAM, "node_config.ep_in.interval",
                  "A high-speed board takes a bInterval of 1 to 16 (USB 2.0 9.6.6).");
        return MIMUSB_ERR_PARAM;
    }
    // The IN endpoint is checked for the board's limits and USB's rules like any other.
    if (effective.ep_in.address == 0U || (effective.ep_in.address & 0x80U) == 0U) {
        set_error(MIMUSB_ERR_PARAM, "node_config.ep_in.address",
                  "ep_in.address is 0x81 to 0x8F: bit 7 set, endpoint number 1 to 15.");
        return MIMUSB_ERR_PARAM;
    }
    if (has_out && (effective.ep_out.address & 0x80U) != 0U) {
        set_error(MIMUSB_ERR_PARAM, "node_config.ep_out.address",
                  "ep_out.address is 0x01 to 0x0F: bit 7 clear, endpoint number 1 to 15.");
        return MIMUSB_ERR_PARAM;
    }
    // The endpoints and the interface alone, as the composer will see them with the others.
    mimusb::usb::HidNodeInput one;
    one.config = effective;
    one.report_descriptor = &kNoDescriptor;
    std::vector<mimusb::usb::ComposerGroup> groups;
    mimusb::usb::ComposerError error;
    if (mimusb::usb::compose_hid_groups({one}, &groups, &error) != MIMUSB_OK) {
        set_error(error.result, error.field, error.reason, 0, 0, 0, 0, error.offset, error.length);
        return error.result;
    }
    return mimusb::detail::precheck_groups(device, std::move(groups));
}

} // namespace

namespace mimusb::detail {

bool has_non_neutral_state(const mimusb_node* node) noexcept {
    if (node == nullptr || node->spec == nullptr) {
        return false;
    }
    if (const auto* keyboard = std::get_if<mimusb::detail::KeyboardState>(&node->state)) {
        return keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    }
    if (const auto* mouse = std::get_if<mimusb::detail::MouseState>(&node->state)) {
        return std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* consumer = std::get_if<mimusb::detail::ConsumerState>(&node->state)) {
        return std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* gamepad = std::get_if<mimusb::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<mimusb::detail::GamepadConfig>(&node->spec->config);
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
    if (const auto* touch = std::get_if<mimusb::detail::TouchState>(&node->state)) {
        const bool active_contact =
            std::any_of(touch->contacts.begin(), touch->contacts.end(), [](const auto& contact) {
                return contact.phase == mimusb::detail::ContactPhase::down;
            });
        return active_contact || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* pen = std::get_if<mimusb::detail::PenState>(&node->state)) {
        return pen->tool_switch_departure || pen->desired.in_range != 0U ||
               pen->desired.tip != 0U || pen->desired.barrel_buttons != 0U;
    }
    return false;
}

#undef MIMUSB_NOINLINE

void make_neutral(mimusb_node* node) noexcept {
    bool had_non_neutral_state = false;
    if (auto* keyboard = std::get_if<mimusb::detail::KeyboardState>(&node->state)) {
        had_non_neutral_state = neutralize_keyboard(keyboard);
    } else if (auto* mouse = std::get_if<mimusb::detail::MouseState>(&node->state)) {
        had_non_neutral_state = neutralize_mouse(mouse);
    } else if (auto* consumer = std::get_if<mimusb::detail::ConsumerState>(&node->state)) {
        had_non_neutral_state = std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                                            [](const std::uint8_t value) { return value != 0U; });
        std::fill(consumer->pressed.begin(), consumer->pressed.end(), std::uint8_t{0});
    } else if (auto* gamepad = std::get_if<mimusb::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<mimusb::detail::GamepadConfig>(&node->spec->config);
        had_non_neutral_state = neutralize_gamepad(gamepad, spec);
    } else if (auto* touch = std::get_if<mimusb::detail::TouchState>(&node->state)) {
        had_non_neutral_state = neutralize_touch(touch);
    } else if (auto* pen = std::get_if<mimusb::detail::PenState>(&node->state)) {
        had_non_neutral_state = neutralize_pen(pen);
    }
    node->dirty = (had_non_neutral_state || node->emitted_non_neutral) &&
                  node->spec->kind != MIMUSB_PROFILE_RAW;
}

bool touch_frame_in_progress(const mimusb_node* node) noexcept {
    const auto* touch =
        node == nullptr ? nullptr : std::get_if<mimusb::detail::TouchState>(&node->state);
    return touch != nullptr && touch->packet_cursor != 0U;
}

} // namespace mimusb::detail

extern "C" {

// ---- nodes -----------------------------------------------------------------------------

mimusb_result MIMUSB_CALL mimusb_node_open(mimusb_device* device, const mimusb_node_config* config,
                                           const mimusb_commit_timeouts* timeouts,
                                           mimusb_node** out_node) try {
    mimusb::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_node", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node.open", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "The Device handle was consumed by close.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    if (device->descriptors_set) {
        set_error(MIMUSB_ERR_BUSY, "node.open",
                  "The Device is given as bytes (mimusb_device_set_descriptors); clear them "
                  "before opening a Node.");
        return MIMUSB_ERR_BUSY;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (!mimusb::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_node_config)),
                                      "node_config")) {
        return MIMUSB_ERR_PARAM;
    }
    if (!reserved_is_zero(config->reserved, "node_config.reserved"))
        return MIMUSB_ERR_PARAM;
    if (config->interfaces == nullptr || config->interface_count == 0U ||
        config->interface_count > device->board.max_interfaces_any ||
        (config->association == nullptr) != (config->association_length == 0U)) {
        set_error(MIMUSB_ERR_PARAM, "node_config.interfaces",
                  "A Node has one or more interfaces (at most the board's max_interfaces_any); "
                  "association and association_length go together.");
        return MIMUSB_ERR_PARAM;
    }
    auto node = std::unique_ptr<mimusb_node>(new (std::nothrow) mimusb_node());
    if (node == nullptr) {
        set_error(MIMUSB_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMUSB_ERR_INTERNAL;
    }
    node->generic = true;
    node->device = device;
    if (const mimusb_result r = mimusb::detail::copy_control_rules(
            device, config->rules, config->rule_count, "node_config.rules", &node->rules);
        r != MIMUSB_OK)
        return r;
    if (config->association != nullptr)
        node->association.assign(config->association,
                                 config->association + config->association_length);
    for (std::size_t i = 0U; i < config->interface_count; ++i) {
        const mimusb_usb_interface& in = config->interfaces[i];
        // The class is the caller's: the library reads it only for this refusal. Class 0 is
        // "defined at the device level" (USB 2.0 9.6.5 / the class codes of the USB-IF), never an
        // interface the caller forgot.
        if (in.class_code == 0U) {
            set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.class_code",
                      "class_code is required.", 0, 0, 0, 0, in.number, 0);
            return MIMUSB_ERR_UNSET_FIELD;
        }
        if (in.settings == nullptr || in.setting_count == 0U || in.setting_count > 255U) {
            set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings",
                      "An interface has 1 to 255 settings (bAlternateSetting 0, 1, 2, ... in "
                      "order); settings must not be null.",
                      0, 0, 0, 0, in.number, static_cast<std::uint32_t>(in.setting_count));
            return MIMUSB_ERR_PARAM;
        }
        if (!valid_usb_string(in.name)) {
            set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.name",
                      "The name must be UTF-8 without control characters, at most 126 UTF-16 code "
                      "units.");
            return MIMUSB_ERR_PARAM;
        }
        mimusb::detail::GenericInterface copy;
        copy.number = in.number;
        copy.class_code = in.class_code;
        copy.subclass = in.subclass;
        copy.protocol = in.protocol;
        copy.name = in.name != nullptr ? in.name : "";
        for (std::size_t s = 0U; s < in.setting_count; ++s) {
            const mimusb_usb_setting& setting = in.settings[s];
            if (setting.items == nullptr && setting.item_count != 0U) {
                set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.items",
                          "items must not be null with a nonzero item_count.", 0, 0, 0, 0,
                          in.number, static_cast<std::uint32_t>(s));
                return MIMUSB_ERR_PARAM;
            }
            mimusb::usb::ComposerSetting made_setting;
            for (std::size_t k = 0U; k < setting.item_count; ++k) {
                const mimusb_usb_item& item = setting.items[k];
                mimusb::usb::ComposerItem made_item;
                if (item.reserved != 0U) {
                    set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.items.reserved",
                              "reserved must be zero.", 0, 0, 0, 0, in.number,
                              static_cast<std::uint32_t>(k));
                    return MIMUSB_ERR_PARAM;
                }
                if (item.kind == MIMUSB_ITEM_ENDPOINT) {
                    if (item.descriptor != nullptr || item.descriptor_length != 0U) {
                        set_error(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.items.descriptor",
                                  "An endpoint item leaves descriptor and descriptor_length "
                                  "zero.",
                                  0, 0, 0, 0, in.number, static_cast<std::uint32_t>(k));
                        return MIMUSB_ERR_PARAM;
                    }
                    made_item.is_endpoint = true;
                    made_item.endpoint = item.endpoint;
                } else if (item.kind == MIMUSB_ITEM_DESCRIPTOR) {
                    mimusb_usb_endpoint zero{};
                    if (item.descriptor == nullptr ||
                        std::memcmp(&item.endpoint, &zero, sizeof(zero)) != 0) {
                        set_error(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.items.descriptor",
                                  "A descriptor item has a descriptor and leaves endpoint all "
                                  "zero.",
                                  0, 0, 0, 0, in.number, static_cast<std::uint32_t>(k));
                        return MIMUSB_ERR_PARAM;
                    }
                    made_item.descriptor.assign(item.descriptor,
                                                item.descriptor + item.descriptor_length);
                } else {
                    set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.settings.items.kind",
                              "kind is MIMUSB_ITEM_ENDPOINT or MIMUSB_ITEM_DESCRIPTOR.", 0, 0, 0, 0,
                              in.number, static_cast<std::uint32_t>(k));
                    return MIMUSB_ERR_UNSET_FIELD;
                }
                made_setting.items.push_back(std::move(made_item));
            }
            copy.settings.push_back(std::move(made_setting));
        }
        // One runtime per IN address, over all settings: an address that comes again in another
        // setting is the same endpoint (the composer checks they agree). The largest packet is the
        // bound of a write, and an isochronous endpoint takes one packet per write.
        copy.for_each_endpoint([&](const mimusb_usb_endpoint& ep) {
            if ((ep.address & 0x80U) == 0U || ep.policy.on_full == 0U || ep.policy.depth == 0U)
                return;
            for (mimusb::detail::EndpointRuntime& known : node->in_endpoints) {
                if (known.address == ep.address) {
                    known.max_packet_size = std::max(known.max_packet_size, ep.max_packet_size);
                    return;
                }
            }
            mimusb::detail::EndpointRuntime runtime;
            runtime.address = ep.address;
            runtime.max_packet_size = ep.max_packet_size;
            runtime.isochronous = ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS;
            runtime.policy = ep.policy;
            node->in_endpoints.push_back(runtime);
        });
        node->interfaces.push_back(std::move(copy));
    }
    // The interface checks of the composer on this Node alone; the ones across Nodes (numbers
    // without a gap, a repeated address) are made at the commit, before anything is sent.
    {
        std::vector<mimusb::usb::ComposerGroup> groups(1);
        groups[0].association = node->association;
        for (const mimusb::detail::GenericInterface& itf : node->interfaces) {
            mimusb::usb::ComposerInterface made;
            made.number = itf.number;
            made.class_code = itf.class_code;
            made.subclass = itf.subclass;
            made.protocol = itf.protocol;
            mimusb::detail::utf16_from_utf8(itf.name.c_str(), &made.name);
            made.settings = itf.settings;
            groups[0].interfaces.push_back(std::move(made));
        }
        if (const mimusb_result r = mimusb::detail::precheck_groups(device, std::move(groups));
            r != MIMUSB_OK)
            return r;
    }
    std::uint16_t node_id = 0U;
    if (!mimusb::detail::free_node_id(device, &node_id)) {
        set_error(MIMUSB_ERR_OVERFLOW, "node.open", "The Device holds no more Nodes.");
        return MIMUSB_ERR_OVERFLOW;
    }
    node->node_id = node_id;
    node->iface = node->interfaces.front().number;

    mimusb_node* published = node.get();
    mimusb::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMUSB_OK;
    }
    const mimusb_result committed = commit_device(device, 0U, waits);
    if (committed != MIMUSB_OK) {
        mimusb::detail::unpublish_node(device, published);
        if (device->configured)
            commit_device(device, 0U, waits);
        return committed;
    }
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "generic node opened");
    *out_node = node.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("node.open")

mimusb_result MIMUSB_CALL mimusb_node_open_profile(mimusb_device* device, mimusb_spec* spec,
                                                   const mimusb_hid_node_config* config,
                                                   const mimusb_commit_timeouts* timeouts,
                                                   mimusb_node** out_node) try {
    mimusb::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_node", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr || spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node.open", "A live Device and immutable Spec are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "The Device handle was consumed by close.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    if (device->descriptors_set) {
        set_error(MIMUSB_ERR_BUSY, "node.open_profile",
                  "The Device is given as bytes (mimusb_device_set_descriptors); clear them "
                  "before opening a Node.");
        return MIMUSB_ERR_BUSY;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (!mimusb::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_hid_node_config)),
                                      "node_config")) {
        return MIMUSB_ERR_PARAM;
    }
    mimusb_hid_node_config effective = *config;
    if (const mimusb_result v = validate_hid_node_config(device, effective); v != MIMUSB_OK)
        return v;

    if (spec->descriptor.empty() || spec->descriptor.size() > device->board.max_answer_bytes) {
        set_error(MIMUSB_ERR_OVERFLOW, "spec.descriptor.board",
                  "The descriptor exceeds the board's answer memory.");
        return MIMUSB_ERR_OVERFLOW;
    }
    struct Limit {
        const char* field;
        const char* reason;
        std::uint64_t allowed;
        std::uint64_t needed;
    };
    const mimusb_linux_hid_policy& linux_hid = device->config.transport.linux_hid;
    const mimusb::hid::DescriptorRequirements& needs = spec->requirements;
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
            set_error(MIMUSB_ERR_OVERFLOW, limit.field, limit.reason, 0, 0, 0, 0,
                      saturate_u32(limit.allowed), saturate_u32(limit.needed));
            return MIMUSB_ERR_OVERFLOW;
        }
    }
    for (const auto& report : spec->report_capabilities) {
        if (report.wire_length > device->maximum_report_bytes) {
            set_error(MIMUSB_ERR_OVERFLOW, "spec.report",
                      "A report exceeds the board's or the caller's report size limit.");
            return MIMUSB_ERR_OVERFLOW;
        }
    }

    std::uint16_t node_id = 0U;
    if (!mimusb::detail::free_node_id(device, &node_id)) {
        set_error(MIMUSB_ERR_OVERFLOW, "node.open", "The Device holds no more Nodes.");
        return MIMUSB_ERR_OVERFLOW;
    }

    auto node = std::unique_ptr<mimusb_node>(new (std::nothrow) mimusb_node());
    if (node == nullptr) {
        set_error(MIMUSB_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMUSB_ERR_INTERNAL;
    }
    node->device = device;
    node->spec = spec;
    node->config = effective;
    node->interface_string = effective.name != nullptr ? effective.name : "";
    node->config.name = nullptr;
    node->node_id = node_id;
    node->iface = effective.interface_number;
    {
        // The IN endpoint is one of the generic driver's: it has a queue and a policy.
        mimusb::detail::EndpointRuntime runtime;
        runtime.address = effective.ep_in.address;
        runtime.max_packet_size = effective.ep_in.max_packet_size;
        runtime.policy = effective.ep_in.policy;
        node->in_endpoints.push_back(runtime);
    }
    collect_report_ids(spec->descriptor, &node->report_ids, &node->has_report_ids);
    if (const mimusb_result initialized = initialize_node_state(node.get());
        initialized != MIMUSB_OK)
        return initialized;

    // A touch Spec that declares Contact Count Maximum answers the target's Feature read.
    if (const auto* touch = std::get_if<mimusb::detail::TouchConfig>(&spec->config);
        touch != nullptr && touch->fields.enable_contact_count_maximum_feature_declaration == 1U) {
        mimusb_node::FeatureEntry entry;
        entry.report_id =
            touch->fields.report_id.enabled == 1U ? touch->fields.report_id.value : 0U;
        const std::size_t bytes = (touch->fields.contact_count.bit_width + 7U) / 8U;
        for (std::size_t i = 0U; i < bytes; ++i)
            entry.data.push_back(
                static_cast<std::uint8_t>((touch->fields.maximum_contacts >> (8U * i)) & 0xFFU));
        node->features.push_back(std::move(entry));
    }

    mimusb::detail::retain_spec(spec);
    mimusb_node* published = node.get();
    mimusb::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMUSB_OK;
    }
    const mimusb_result committed = commit_device(device, 0U, waits);
    if (committed != MIMUSB_OK) {
        // The board's active configuration is unchanged or unknown; drop this node again.
        mimusb::detail::unpublish_node(device, published);
        mimusb::detail::release_spec(spec);
        node->spec = nullptr;
        if (device->configured) {
            commit_device(device, 0U, waits);
        }
        return committed;
    }
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "node opened");
    *out_node = node.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("node.open_profile")

mimusb_result MIMUSB_CALL mimusb_node_set_feature_response(
    mimusb_node* node, const uint8_t report_id,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const uint8_t* data, const size_t length, const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->generic ||
        (data == nullptr && length != 0U) || length > node->device->board.max_control_data_bytes) {
        set_error(MIMUSB_ERR_PARAM, "node.feature",
                  "A live profile Node and a response of at most the board's max_feature_bytes "
                  "are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    mimusb_device* device = node->device;
    mimusb_node::FeatureEntry* entry = nullptr;
    for (auto& existing : node->features) {
        if (existing.report_id == report_id)
            entry = &existing;
    }
    const bool existed = entry != nullptr;
    const std::size_t old_length = existed ? entry->data.size() : 0U;
    if (entry == nullptr) {
        node->features.push_back(mimusb_node::FeatureEntry{report_id, {}});
        entry = &node->features.back();
    }
    entry->data.assign(data, data + length);
    if (!device->configured || device->batch_open || node->staged_only) {
        device->dirty = device->dirty || device->configured;
        return MIMUSB_OK; // sent with the next commit
    }
    if (existed && old_length == length) {
        // The rule exists with room for these bytes: only its bytes change. The target reads the
        // Report ID first when there is one (HID 1.11 7.2.1 and 8.1).
        std::vector<std::uint8_t> bytes;
        if (report_id != 0U)
            bytes.push_back(report_id);
        bytes.insert(bytes.end(), entry->data.begin(), entry->data.end());
        std::size_t index = device->table.size();
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            for (std::size_t i = 0U; i < device->table.size(); ++i) {
                const mimusb::usb::BoardRule& rule = device->table[i];
                if (rule.role == mimusb::usb::BoardRule::Role::hid_other &&
                    rule.iface == node->iface && rule.rule.action == MIMUSB_CONTROL_ANSWER &&
                    rule.rule.request_value == 0x01U &&
                    rule.rule.value_value == static_cast<std::uint16_t>(0x0300U | report_id))
                    index = i;
            }
        }
        if (index < device->table.size()) {
            const mimusb_result sent = mimusb::detail::send_rule_data(
                device, index, bytes.data(), bytes.size(), control_timeout_ms);
            if (sent == MIMUSB_OK) {
                std::lock_guard<std::mutex> lock(device->rules_mutex);
                if (index < device->table.size())
                    device->table[index].data = std::move(bytes);
            }
            return sent;
        }
    }
    // A new Feature report or another length: the table is made again.
    return mimusb::detail::rebuild_rules(device, control_timeout_ms);
}
MIMUSB_C_RESULT_CATCH("node.set_feature_response")

mimusb_result MIMUSB_CALL mimusb_node_set_rule_data(mimusb_node* node, const size_t rule_index,
                                                    const uint8_t* data, const size_t length,
                                                    const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    return mimusb::detail::change_rule_data(node->device, node, rule_index, data, length,
                                            control_timeout_ms);
}
MIMUSB_C_RESULT_CATCH("node.set_rule_data")

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimusb_result MIMUSB_CALL mimusb_hid_descriptor_bytes(const uint16_t bcd_hid,
                                                      const uint8_t country_code,
                                                      const size_t report_descriptor_length,
                                                      uint8_t out[9]) try {
    mimusb::detail::clear_error();
    if (out == nullptr || report_descriptor_length == 0U || report_descriptor_length > 0xFFFFU) {
        set_error(MIMUSB_ERR_PARAM, "report_descriptor_length",
                  "out is required and the length is 1 to 65535.");
        return MIMUSB_ERR_PARAM;
    }
    if (bcd_hid == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "bcd_hid", "bcd_hid must be given (e.g. 0x0111).");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    // HID 1.11 6.2.1: bLength 9, bDescriptorType 0x21, bcdHID, bCountryCode, bNumDescriptors 1, and
    // the report descriptor's type 0x22 with its length.
    out[0] = 9U;
    out[1] = 0x21U;
    out[2] = static_cast<uint8_t>(bcd_hid & 0xFFU);
    out[3] = static_cast<uint8_t>(bcd_hid >> 8U);
    out[4] = country_code;
    out[5] = 1U;
    out[6] = 0x22U;
    out[7] = static_cast<uint8_t>(report_descriptor_length & 0xFFU);
    out[8] = static_cast<uint8_t>((report_descriptor_length >> 8U) & 0xFFU);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("hid_descriptor_bytes")

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimusb_result MIMUSB_CALL mimusb_hid_descriptor_rule(const uint8_t interface_number,
                                                     const uint8_t descriptor_type,
                                                     const uint8_t* bytes, const size_t length,
                                                     mimusb_control_rule* out_rule) try {
    mimusb::detail::clear_error();
    if (bytes == nullptr || out_rule == nullptr || length == 0U || length > 0xFFFFU ||
        (descriptor_type != 0x22U && descriptor_type != 0x21U)) {
        set_error(MIMUSB_ERR_PARAM, "hid_descriptor_rule",
                  "bytes and out_rule are required, the length is 1 to 65535, and the type 0x21 "
                  "or 0x22.");
        return MIMUSB_ERR_PARAM;
    }
    // USB 2.0 9.4.3 GET_DESCRIPTOR: standard, device to host, interface; wValue's high byte is the
    // type (HID 7.1.1), any index of descriptor; wIndex the interface.
    *out_rule = mimusb_control_rule{};
    out_rule->request_type_mask = 0xFFU;
    out_rule->request_type_value = 0x81U;
    out_rule->request_mask = 0xFFU;
    out_rule->request_value = 0x06U;
    out_rule->value_mask = 0xFF00U;
    out_rule->value_value = static_cast<uint16_t>(descriptor_type << 8U);
    out_rule->index_mask = 0xFFFFU;
    out_rule->index_value = interface_number;
    out_rule->action = MIMUSB_CONTROL_ANSWER;
    out_rule->data = bytes;
    out_rule->length = length;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("hid_descriptor_rule")

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimusb_result MIMUSB_CALL mimusb_cdc_acm_descriptors_make(
    const uint16_t bcd_cdc, const uint8_t control_interface,
    const uint8_t call_management_capabilities, const uint8_t acm_capabilities,
    mimusb_cdc_acm_descriptors* out) try {
    mimusb::detail::clear_error();
    if (out == nullptr || control_interface == 0xFFU) {
        set_error(MIMUSB_ERR_PARAM, "control_interface",
                  "out is required and the data interface needs the next number.");
        return MIMUSB_ERR_PARAM;
    }
    if (bcd_cdc == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "bcd_cdc", "bcd_cdc must be given (e.g. 0x0120).");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    const uint8_t data_interface = static_cast<uint8_t>(control_interface + 1U);
    // CDC 1.2 5.2.3: bLength, bDescriptorType 0x24 (CS_INTERFACE), bDescriptorSubtype, data.
    const uint8_t header[5] = {5U, 0x24U, 0x00U, static_cast<uint8_t>(bcd_cdc & 0xFFU),
                               static_cast<uint8_t>(bcd_cdc >> 8U)};
    const uint8_t call[5] = {5U, 0x24U, 0x01U, call_management_capabilities, data_interface};
    const uint8_t acm[4] = {4U, 0x24U, 0x02U, acm_capabilities};
    const uint8_t onion[5] = {5U, 0x24U, 0x06U, control_interface, data_interface};
    // The Interface Association descriptor (USB IAD ECN): first interface, two interfaces, class
    // 0x02, subclass 0x02, protocol 0x01, no string.
    const uint8_t association[8] = {8U, 0x0BU, control_interface, 2U, 0x02U, 0x02U, 0x01U, 0U};
    std::memcpy(out->header, header, sizeof header);
    std::memcpy(out->call_management, call, sizeof call);
    std::memcpy(out->acm, acm, sizeof acm);
    std::memcpy(out->union_, onion, sizeof onion);
    std::memcpy(out->association, association, sizeof association);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("cdc_acm_descriptors_make")

mimusb_result MIMUSB_CALL mimusb_cdc_acm_rules(const uint8_t control_interface,
                                               const uint8_t line_coding[7],
                                               mimusb_control_rule out_rules[3]) try {
    mimusb::detail::clear_error();
    if (line_coding == nullptr || out_rules == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "cdc_acm_rules", "line_coding and out_rules are required.");
        return MIMUSB_ERR_PARAM;
    }
    // PSTN 1.2 6.3: class requests to the communication interface. bmRequestType 0x21 is host to
    // device, class, interface; 0xA1 is device to host.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const auto make = [&](const uint8_t type, const uint8_t request,
                          const mimusb_control_rule_action action) {
        mimusb_control_rule r{};
        r.request_type_mask = 0xFFU;
        r.request_type_value = type;
        r.request_mask = 0xFFU;
        r.request_value = request;
        r.index_mask = 0xFFFFU;
        r.index_value = control_interface;
        r.action = action;
        return r;
    };
    out_rules[0] = make(0x21U, 0x20U, MIMUSB_CONTROL_ACCEPT); // SET_LINE_CODING
    out_rules[1] = make(0xA1U, 0x21U, MIMUSB_CONTROL_ANSWER); // GET_LINE_CODING
    out_rules[1].data = line_coding;
    out_rules[1].length = 7U;
    out_rules[2] = make(0x21U, 0x22U, MIMUSB_CONTROL_ACCEPT); // SET_CONTROL_LINE_STATE
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("cdc_acm_rules")

uint16_t MIMUSB_CALL mimusb_node_id(const mimusb_node* node) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return 0U;
    }
    return public_node_id(node);
}
MIMUSB_C_VALUE_CATCH("node.id", 0U)

mimusb_result MIMUSB_CALL mimusb_node_manifest(const mimusb_node* node,
                                               mimusb_capability_manifest* manifest) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return MIMUSB_ERR_PARAM;
    }
    return mimusb_spec_manifest(node->spec, manifest);
}
MIMUSB_C_RESULT_CATCH("node.manifest")

mimusb_result MIMUSB_CALL mimusb_node_close(mimusb_node* node,
                                            const mimusb_commit_timeouts* timeouts,
                                            const uint32_t drain_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "node.close",
                  "The Node handle is null, closed, detached, or owned by a closing Device.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (const mimusb_result t = require_timeout(drain_timeout_ms, "drain_timeout_ms");
        t != MIMUSB_OK)
        return t;
    mimusb_device* device = node->device;
    // From here the Node goes whatever fails; the first failure is what the caller hears about.
    const mimusb_result neutral = send_neutral(node, drain_timeout_ms);
    const mimusb_error_detail neutral_detail = *mimusb_last_error();
    mimusb::detail::unpublish_node(device, node);
    device->dirty = true;
    mimusb_result committed = MIMUSB_OK;
    if (!device->batch_open && !node->staged_only)
        committed = commit_device(device, 0U, waits);
    node->closed = true;
    mimusb::detail::release_spec(node->spec);
    node->spec = nullptr;
    delete node;
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "node closed");
    if (neutral != MIMUSB_OK) {
        mimusb::detail::restore_error(neutral_detail);
        return neutral;
    }
    if (committed != MIMUSB_OK)
        return committed;
    mimusb::detail::clear_error();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("node.close")

} // extern "C"
