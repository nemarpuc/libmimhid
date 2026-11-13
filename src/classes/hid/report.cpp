// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The report of a HID Node: serializing its state bit for bit into the report its descriptor
 * declares, checking a report against the descriptor, and settling the state when the report was
 * written (transfer_complete).
 */

#include "classes/hid/state_support.hpp"

using namespace mimusb::state_build;

namespace mimusb::detail {

mimusb_result link_available(const mimusb::detail::HidNode* node, const char* field) noexcept {
    if (node == nullptr) {
        set_error(MIMUSB_ERR_PARAM, field, "The node is not attached to a live device.");
        return MIMUSB_ERR_PARAM;
    }
    // Pure state-machine tests construct the private node representation
    // without a device. Every public Node is attached by mimusb_node_open().
    if (node->owner->device == nullptr) {
        return MIMUSB_OK;
    }
    const std::uint16_t node_id = static_cast<std::uint16_t>(node->owner->node_id + 1U);
    const std::uint16_t report_id = node->spec != nullptr && node->spec->layout.has_report_id
                                        ? node->spec->layout.report_id
                                        : 0U;
    if (node->owner->device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, field, "The serial link is gone; its node state is frozen.",
                  0, 0, node_id, report_id);
        return MIMUSB_ERR_NO_DEVICE;
    }
    if (node->owner->staged_only ||
        node->owner->device->reconfiguring.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, field,
                  "The board has not applied this node's configuration yet (batch open or "
                  "re-enumeration in progress).",
                  0, 0, node_id, report_id);
        return MIMUSB_ERR_BUSY;
    }
    return MIMUSB_OK;
}

mimusb_result validate_serialized_report(const mimusb::detail::HidSpec* spec,
                                         const std::uint8_t* report,
                                         const std::size_t report_length) noexcept {
    if (spec == nullptr) {
        set_error(MIMUSB_ERR_INTERNAL, "report.length",
                  "The generated report does not match its immutable wire length.");
        return MIMUSB_ERR_INTERNAL;
    }
    return validate_serialized_report(spec, spec->layout, report, report_length);
}

mimusb_result validate_serialized_report(const mimusb::detail::HidSpec* spec,
                                         const mimusb::hid::ReportLayout& layout,
                                         const std::uint8_t* report,
                                         const std::size_t report_length) noexcept {
    if (spec == nullptr || report == nullptr || report_length != layout.wire_bytes ||
        report_length == 0U) {
        set_error(MIMUSB_ERR_INTERNAL, "report.length",
                  "The generated report does not match its immutable wire length.");
        return MIMUSB_ERR_INTERNAL;
    }
    if (layout.has_report_id && report[0] != layout.report_id) {
        set_error(MIMUSB_ERR_INTERNAL, "report.report_id",
                  "The generated report prefix does not match its immutable Report ID.", 0, 57, 0,
                  layout.report_id, 0, static_cast<std::uint32_t>(report_length));
        return MIMUSB_ERR_INTERNAL;
    }
    for (const FieldLayout& field : layout.fields) {
        if (field.bit_width == 0U || field.bit_width > 32U ||
            field.bit_offset > report_length * 8U ||
            field.bit_width > report_length * 8U - field.bit_offset) {
            set_error(MIMUSB_ERR_INTERNAL, "report.layout",
                      "An immutable field lies outside the generated report.");
            return MIMUSB_ERR_INTERNAL;
        }
        std::uint64_t encoded = 0U;
        std::size_t position = field.bit_offset;
        for (std::uint32_t done = 0U; done < field.bit_width;) {
            const auto shift = static_cast<std::uint32_t>(position & 7U);
            const std::uint32_t taken = std::min(field.bit_width - done, 8U - shift);
            encoded |= static_cast<std::uint64_t>(
                           (static_cast<std::uint32_t>(report[position / 8U]) >> shift) &
                           ((1U << taken) - 1U))
                       << done;
            position += taken;
            done += taken;
        }
        std::int64_t value = static_cast<std::int64_t>(encoded);
        if (field.is_signed && (encoded & (std::uint64_t{1} << (field.bit_width - 1U))) != 0U) {
            value = static_cast<std::int64_t>(encoded | (~std::uint64_t{0} << field.bit_width));
        }
        const bool null_semantic = field.has_null_state && value == hat_null(field);
        const bool in_declared_range =
            value >= field.logical_minimum && value <= field.logical_maximum;
        if ((!in_declared_range && !null_semantic) ||
            (field.semantic == FieldSemantic::constant_padding && value != 0)) {
            set_error(
                MIMUSB_ERR_INTERNAL, "report.field",
                "A generated field is outside its immutable logical range or padding is nonzero.");
            return MIMUSB_ERR_INTERNAL;
        }
    }
    return MIMUSB_OK;
}

mimusb_result serialize_battery(const mimusb::detail::HidNode* node, std::uint8_t* report,
                                const std::size_t capacity, std::size_t* report_length) noexcept {
    if (node == nullptr || node->spec == nullptr || !node->spec->has_battery || report == nullptr ||
        report_length == nullptr || capacity < node->spec->battery_layout.wire_bytes) {
        set_error(MIMUSB_ERR_PARAM, "report", "The serializer buffer is missing or too small.");
        return MIMUSB_ERR_PARAM;
    }
    const auto& layout = node->spec->battery_layout;
    std::fill_n(report, layout.wire_bytes, std::uint8_t{0});
    report[0] = layout.report_id;
    if (!store_bits(report, capacity, layout.fields.front(), node->battery.strength, false)) {
        set_error(MIMUSB_ERR_INTERNAL, "report.field",
                  "The battery level does not fit its immutable field.");
        return MIMUSB_ERR_INTERNAL;
    }
    *report_length = layout.wire_bytes;
    return MIMUSB_OK;
}

mimusb_result serialize_node(mimusb::detail::HidNode* node, std::uint8_t* report,
                             const std::size_t capacity, std::size_t* report_length) noexcept {
    if (node == nullptr || node->spec == nullptr || report == nullptr || report_length == nullptr ||
        capacity < node->spec->layout.wire_bytes) {
        set_error(MIMUSB_ERR_PARAM, "report", "The serializer buffer is missing or too small.");
        return MIMUSB_ERR_PARAM;
    }
    const auto& layout = node->spec->layout;
    std::fill_n(report, layout.wire_bytes, std::uint8_t{0});
    if (layout.has_report_id) {
        report[0] = layout.report_id;
    }

    const KeyboardState* keyboard = state<KeyboardState>(node);
    const MouseState* mouse = state<MouseState>(node);
    const ConsumerState* consumer = state<ConsumerState>(node);
    const GamepadState* gamepad = state<GamepadState>(node);
    const TouchState* touch = state<TouchState>(node);
    const PenState* pen = state<PenState>(node);
    const KeyboardConfig* keyboard_config = config<KeyboardConfig>(node);
    const GamepadConfig* gamepad_config = config<GamepadConfig>(node);
    const TouchConfig* touch_config = config<TouchConfig>(node);
    std::array<const ContactState*, 16> active{};
    const std::size_t active_count = touch == nullptr ? 0U : collect_touch(*touch, &active);
    const std::size_t touch_start = touch == nullptr ? 0U : touch->packet_cursor;
    bool ok = true;

    for (const FieldLayout& field : layout.fields) {
        std::int64_t value = 0;
        bool allow_null = false;
        switch (field.semantic) {
        case FieldSemantic::modifier:
            value = keyboard == nullptr ? 0 : (keyboard->modifiers >> field.instance) & 1U;
            break;
        case FieldSemantic::key_bitmap:
            if (keyboard != nullptr && keyboard_config != nullptr) {
                value = field.instance < keyboard->pressed_bitmap.size()
                            ? keyboard->pressed_bitmap[field.instance]
                            : 0;
            }
            break;
        case FieldSemantic::buttons:
            if (mouse != nullptr && field.instance < mouse->buttons.size()) {
                value = mouse->buttons[field.instance];
            } else if (gamepad != nullptr && field.instance < gamepad->buttons.size()) {
                value = gamepad->buttons[field.instance];
            } else if (gamepad != nullptr && gamepad_config != nullptr &&
                       gamepad_config->options.dpad_representation == MIMUSB_DPAD_BUTTONS &&
                       field.instance >= gamepad_config->options.button_count &&
                       field.instance < gamepad_config->options.button_count + 4U) {
                value =
                    (gamepad->hat >> (field.instance - gamepad_config->options.button_count)) & 1;
            } else if (touch != nullptr && field.instance < touch->buttons.size()) {
                value = touch->buttons[field.instance];
            } else if (pen != nullptr) {
                value = pen->tool_switch_departure || pen->desired.in_range == 0U
                            ? 0
                            : (pen->desired.barrel_buttons >> field.instance) & 1U;
            }
            break;
        case FieldSemantic::x:
        case FieldSemantic::y:
        case FieldSemantic::pressure:
        case FieldSemantic::width:
        case FieldSemantic::height:
        case FieldSemantic::azimuth: {
            if (mouse != nullptr) {
                const std::int64_t pending =
                    field.semantic == FieldSemantic::x ? mouse->dx : mouse->dy;
                value = std::clamp(pending, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                if (field.semantic == FieldSemantic::x) {
                    node->submitted_dx = value;
                } else {
                    node->submitted_dy = value;
                }
            } else if (touch != nullptr && touch_config != nullptr) {
                const std::size_t contact_index = touch_start + field.instance;
                if (contact_index < active_count) {
                    const auto& sample = active[contact_index]->sample;
                    if (field.semantic == FieldSemantic::x)
                        value = sample.x;
                    if (field.semantic == FieldSemantic::y)
                        value = sample.y;
                    if (field.semantic == FieldSemantic::pressure) {
                        value = active[contact_index]->phase == ContactPhase::down
                                    ? std::max(sample.pressure, 1)
                                    : 0;
                    }
                    if (field.semantic == FieldSemantic::width)
                        value = sample.width;
                    if (field.semantic == FieldSemantic::height)
                        value = sample.height;
                    if (field.semantic == FieldSemantic::azimuth)
                        value = sample.azimuth;
                }
            } else if (pen != nullptr) {
                const mimusb_pen_sample& sample =
                    pen->tool_switch_departure ? pen->emitted : pen->desired;
                if (field.semantic == FieldSemantic::x)
                    value = sample.x;
                if (field.semantic == FieldSemantic::y)
                    value = sample.y;
                if (field.semantic == FieldSemantic::pressure) {
                    value = pen->tool_switch_departure || sample.tip == 0U
                                ? 0
                                : std::max(sample.pressure, 1);
                }
            }
            break;
        }
        case FieldSemantic::wheel:
            if (mouse != nullptr) {
                value = std::clamp(mouse->wheel, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                node->submitted_wheel = value;
            }
            break;
        case FieldSemantic::pan:
            if (mouse != nullptr) {
                value = std::clamp(mouse->pan, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                node->submitted_pan = value;
            }
            break;
        case FieldSemantic::consumer_usage:
            value = consumer != nullptr && field.instance < consumer->pressed.size()
                        ? consumer->pressed[field.instance]
                        : 0;
            break;
        case FieldSemantic::gamepad_axis:
            value = gamepad != nullptr && field.instance < gamepad->axes.size()
                        ? gamepad->axes[field.instance]
                        : 0;
            break;
        case FieldSemantic::hat:
            if (gamepad != nullptr && gamepad->hat_has_direction) {
                value = gamepad->hat;
            } else {
                value = hat_null(field);
                allow_null = true;
            }
            break;
        case FieldSemantic::tip:
            if (touch != nullptr) {
                const std::size_t contact_index = touch_start + field.instance;
                value = contact_index < active_count &&
                                active[contact_index]->phase == ContactPhase::down
                            ? 1
                            : 0;
            } else if (pen != nullptr) {
                value = pen->tool_switch_departure ? 0 : pen->desired.tip;
            }
            break;
        case FieldSemantic::in_range:
            value = pen == nullptr || pen->tool_switch_departure ? 0 : pen->desired.in_range;
            break;
        case FieldSemantic::eraser:
            value = pen == nullptr || pen->tool_switch_departure || pen->desired.in_range == 0U
                        ? 0
                        : pen->desired.eraser;
            break;
        case FieldSemantic::contact_id: {
            const std::size_t contact_index = touch_start + field.instance;
            value = touch != nullptr && contact_index < active_count
                        ? active[contact_index]->sample.contact_id
                        : 0;
            break;
        }
        case FieldSemantic::scan_time:
            value = touch == nullptr ? 0 : touch->scan_time;
            break;
        case FieldSemantic::contact_count:
            value =
                touch == nullptr || touch_start != 0U ? 0 : static_cast<std::int64_t>(active_count);
            break;
        case FieldSemantic::tilt_x:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.tilt_x : pen->desired.tilt_x);
            break;
        case FieldSemantic::tilt_y:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.tilt_y : pen->desired.tilt_y);
            break;
        case FieldSemantic::twist:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.twist : pen->desired.twist);
            break;
        case FieldSemantic::battery_strength:
            ok = false;
            break;
        case FieldSemantic::constant_padding:
            continue;
        }
        ok = ok && store_bits(report, capacity, field, value, allow_null);
    }
    if (!ok) {
        set_error(MIMUSB_ERR_INTERNAL, "report.layout",
                  "A state value could not be serialized into its immutable layout.");
        return MIMUSB_ERR_INTERNAL;
    }
    if (touch != nullptr && touch_config != nullptr) {
        node->submitted_touch_count =
            std::min<std::size_t>(touch_config->fields.contacts_per_report,
                                  active_count > touch_start ? active_count - touch_start : 0U);
        node->submitted_touch_final_packet =
            touch_start + node->submitted_touch_count >= active_count;
    }
    *report_length = layout.wire_bytes;
    return MIMUSB_OK;
}

void transfer_complete(HidNode* node, const mimusb_result result,
                       const std::int32_t native_status) noexcept {
    if (node == nullptr) {
        return;
    }
    // A negative native_status means no byte of the report reached the serial driver (the
    // port was gone, or a write timed out before any progress). The state then stays pending
    // for the caller to send again, and a failed write never consumes caller state.
    const bool accepted_by_port = native_status >= 0;
    // A refusal keeps the state pending. A report that was cut short mid-frame may or may not
    // have reached the target (the firmware drops a frame that fails its checksum), so it is
    // consumed and never sent again automatically.
    const bool may_have_applied = accepted_by_port;
    // The battery report never touches input state. Like input, a refused
    // transfer stays pending and an uncertain one is not sent again.
    const bool battery_report = node->submitted_battery;
    node->submitted_battery = false;
    bool lifecycle_report_complete = true;
    if (battery_report) {
        node->battery_dirty = !may_have_applied;
    } else if (!may_have_applied) {
        node->dirty = true;
    } else if (auto* mouse = state<MouseState>(node); mouse != nullptr) {
        // A timed-out/cancelled control transfer may have reached the device.
        // Consuming the submitted relative component prevents an unsafe retry
        // from applying the same movement twice.
        mouse->dx -= node->submitted_dx;
        mouse->dy -= node->submitted_dy;
        mouse->wheel -= node->submitted_wheel;
        mouse->pan -= node->submitted_pan;
        node->dirty = result == MIMUSB_OK &&
                      (mouse->dx != 0 || mouse->dy != 0 || mouse->wheel != 0 || mouse->pan != 0);
    } else if (result == MIMUSB_OK) {
        if (auto* touch = state<TouchState>(node); touch != nullptr) {
            if (node->submitted_touch_final_packet) {
                for (auto& contact : touch->contacts) {
                    if (contact.phase == ContactPhase::up) {
                        contact.phase = ContactPhase::none;
                    }
                }
                touch->packet_cursor = 0U;
                node->dirty = false;
                const bool still_active =
                    std::any_of(touch->contacts.begin(), touch->contacts.end(),
                                [](const ContactState& contact) {
                                    return contact.phase == ContactPhase::down;
                                }) ||
                    std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                [](const std::uint8_t value) { return value != 0U; });
                if (!still_active) {
                    // HUT 1.7 section 16.5 defines the base as the first frame
                    // after inactivity, so the next activity starts at zero.
                    touch->scan_epoch_active = false;
                }
            } else {
                touch->packet_cursor += node->submitted_touch_count;
                node->dirty = true;
                lifecycle_report_complete = false;
            }
        } else if (auto* pen = state<PenState>(node); pen != nullptr) {
            if (pen->tool_switch_departure) {
                pen->emitted.in_range = 0U;
                pen->emitted.tip = 0U;
                pen->tool_switch_departure = false;
                node->dirty = true;
                lifecycle_report_complete = false;
            } else {
                pen->emitted = pen->desired;
                node->dirty = false;
            }
        } else {
            node->dirty = false;
        }
    } else {
        // A write cut short mid-frame cannot establish whether the report reached the device.
        // Retaining desired state is useful for the next explicit mutation, but an automatic retry
        // could duplicate a key, selector, contact frame, or raw event.
        if (auto* touch = state<TouchState>(node); touch != nullptr) {
            touch->packet_cursor = 0U;
        }
        node->dirty = false;
    }
    if (!battery_report && may_have_applied && (result != MIMUSB_OK || lifecycle_report_complete)) {
        // An accepted terminal transfer may have reached the target even when
        // completion is uncertain. Consume its edge baseline so a later
        // opposite transition can release any state the target may hold.
        consume_lifecycle_transitions(node);
    }
    if (!battery_report && may_have_applied) {
        if (result == MIMUSB_OK) {
            node->emitted_non_neutral = node->submitted_non_neutral;
        } else {
            // A terminal callback cannot establish whether the target applied
            // an OUT transfer. Preserve any possible held state so close emits
            // a conservative neutral report if the link still exists.
            node->emitted_non_neutral = node->emitted_non_neutral || node->submitted_non_neutral;
        }
    }
    node->submitted_dx = 0;
    node->submitted_dy = 0;
    node->submitted_wheel = 0;
    node->submitted_pan = 0;
    node->submitted_non_neutral = false;
    node->completion_native_status.store(native_status, std::memory_order_relaxed);
    node->completion_report_length.store(static_cast<std::uint32_t>(node->submitted_report_length),
                                         std::memory_order_relaxed);
    node->completion_report_id.store(node->submitted_report_id, std::memory_order_relaxed);
    node->submitted_report_length = 0U;
    node->submitted_report_id = 0U;
    node->completion_result.store(result, std::memory_order_release);
    node->transfer_inflight.store(false, std::memory_order_release);
}

} // namespace mimusb::detail
