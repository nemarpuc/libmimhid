// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * What the report state machines of the HID profiles share: the checks a mutator starts with, the
 * bit packing of a field, the transitions that must reach one report before they may change again,
 * and the exception barrier of the public calls. Mutators do no I/O and allocate no memory after
 * the Node is made. A Node permits one report in flight, which makes completion accounting
 * deterministic for relative axes.
 */

#include "api/internal.hpp"
#include "classes/hid/hid_class.hpp"
#include "hid/usages.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

namespace mimusb::state_build {

using mimusb::detail::ContactPhase;
using mimusb::detail::link_available;
using mimusb::detail::set_error;
using mimusb::hid::FieldLayout;
using mimusb::hid::FieldSemantic;

inline mimusb_result usable(const mimusb::detail::HidNode* node, const mimusb_profile_kind kind,
                            const char* field) noexcept {
    if (node == nullptr || node->owner->closed || node->spec == nullptr ||
        node->spec->kind != kind) {
        set_error(MIMUSB_ERR_PARAM, field, "The operation does not match the node profile.");
        return MIMUSB_ERR_PARAM;
    }
    const mimusb_result link = link_available(node, field);
    if (link != MIMUSB_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, field, "The node has one report in flight.");
        return MIMUSB_ERR_BUSY;
    }
    return MIMUSB_OK;
}

inline bool in_range(const std::int64_t value, const mimusb_integer_field& field) noexcept {
    return value >= field.logical_minimum && value <= field.logical_maximum;
}

inline bool add_checked(std::int64_t* target, const std::int32_t value) noexcept {
    if ((value > 0 && *target > std::numeric_limits<std::int64_t>::max() - value) ||
        (value < 0 && *target < std::numeric_limits<std::int64_t>::min() - value)) {
        return false;
    }
    *target += value;
    return true;
}

inline bool store_bits(std::uint8_t* report, const std::size_t capacity, const FieldLayout& field,
                       const std::int64_t value, const bool allow_null = false) noexcept {
    if (report == nullptr || field.bit_width == 0U || field.bit_width > 32U ||
        field.bit_offset + field.bit_width > capacity * 8U ||
        (allow_null && !field.has_null_state)) {
        return false;
    }
    if (!allow_null && (value < field.logical_minimum || value > field.logical_maximum)) {
        return false;
    }
    std::uint64_t encoded =
        static_cast<std::uint64_t>(value) & ((std::uint64_t{1} << field.bit_width) - 1U);
    std::size_t position = field.bit_offset;
    std::uint32_t remaining = field.bit_width;
    // A field spans at most four bytes, so it is written a byte at a time.
    while (remaining != 0U) {
        const auto shift = static_cast<std::uint32_t>(position & 7U);
        const std::uint32_t taken = std::min(remaining, 8U - shift);
        const std::uint32_t byte_mask = ((1U << taken) - 1U) << shift;
        std::uint8_t& target = report[position / 8U];
        target =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(target) & ~byte_mask) |
                                      ((static_cast<std::uint32_t>(encoded) << shift) & byte_mask));
        encoded >>= taken;
        position += taken;
        remaining -= taken;
    }
    return true;
}

inline std::int64_t hat_null(const FieldLayout& field) noexcept {
    if (field.is_signed) {
        const std::int64_t minimum = field.bit_width == 32U
                                         ? std::numeric_limits<std::int32_t>::min()
                                         : -(std::int64_t{1} << (field.bit_width - 1U));
        if (minimum < field.logical_minimum) {
            return minimum;
        }
        return field.bit_width == 32U ? std::numeric_limits<std::int32_t>::max()
                                      : (std::int64_t{1} << (field.bit_width - 1U)) - 1;
    }
    const std::int64_t maximum =
        field.bit_width == 32U
            ? static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())
            : (std::int64_t{1} << field.bit_width) - 1;
    if (0 < field.logical_minimum) {
        return 0;
    }
    return maximum;
}

template <typename T> const T* config(const mimusb::detail::HidNode* node) noexcept {
    return std::get_if<T>(&node->spec->config);
}

template <typename T> T* state(mimusb::detail::HidNode* node) noexcept {
    return std::get_if<T>(&node->state);
}

inline mimusb_result pending_transition(const char* field) noexcept {
    set_error(MIMUSB_ERR_BUSY, field,
              "An opposite lifecycle transition is pending its first accepted report.");
    return MIMUSB_ERR_BUSY;
}

inline bool transition_storage_matches(const std::vector<std::uint8_t>& values,
                                       const std::vector<std::uint8_t>& transitions,
                                       const char* field) noexcept {
    if (values.size() == transitions.size()) {
        return true;
    }
    set_error(MIMUSB_ERR_INTERNAL, field,
              "The preallocated lifecycle-transition state does not match the profile layout.");
    return false;
}

inline std::uint32_t pen_barrel_changes(const mimusb_pen_sample& before,
                                        const mimusb_pen_sample& after) noexcept {
    return before.barrel_buttons ^ after.barrel_buttons;
}

inline bool pen_transition_conflicts(const mimusb::detail::PenState& pen,
                                     const mimusb_pen_sample& next) noexcept {
    const bool changes_lifecycle =
        pen.desired.in_range != next.in_range || pen.desired.tip != next.tip ||
        pen.desired.eraser != next.eraser || pen_barrel_changes(pen.desired, next) != 0U;
    const bool lifecycle_pending = pen.in_range_transition_pending || pen.tip_transition_pending ||
                                   pen.eraser_transition_pending || pen.barrel_transitions != 0U;
    return changes_lifecycle && lifecycle_pending;
}

inline void record_pen_transitions(mimusb::detail::PenState* pen,
                                   const mimusb_pen_sample& next) noexcept {
    pen->in_range_transition_pending =
        pen->in_range_transition_pending || pen->desired.in_range != next.in_range;
    pen->tip_transition_pending = pen->tip_transition_pending || pen->desired.tip != next.tip;
    pen->eraser_transition_pending =
        pen->eraser_transition_pending || pen->desired.eraser != next.eraser;
    pen->barrel_transitions |= pen_barrel_changes(pen->desired, next);
}

inline void consume_lifecycle_transitions(mimusb::detail::HidNode* node) noexcept {
    if (auto* keyboard = state<mimusb::detail::KeyboardState>(node); keyboard != nullptr) {
        std::fill(keyboard->key_transitions.begin(), keyboard->key_transitions.end(),
                  std::uint8_t{0});
        keyboard->modifier_transitions = 0U;
    } else if (auto* mouse = state<mimusb::detail::MouseState>(node); mouse != nullptr) {
        std::fill(mouse->button_transitions.begin(), mouse->button_transitions.end(),
                  std::uint8_t{0});
    } else if (auto* consumer = state<mimusb::detail::ConsumerState>(node); consumer != nullptr) {
        std::fill(consumer->transitions.begin(), consumer->transitions.end(), std::uint8_t{0});
    } else if (auto* gamepad = state<mimusb::detail::GamepadState>(node); gamepad != nullptr) {
        std::fill(gamepad->button_transitions.begin(), gamepad->button_transitions.end(),
                  std::uint8_t{0});
        gamepad->hat_transition_pending = false;
    } else if (auto* touch = state<mimusb::detail::TouchState>(node); touch != nullptr) {
        for (auto& contact : touch->contacts) {
            contact.lifecycle_transition_pending = false;
        }
        std::fill(touch->button_transitions.begin(), touch->button_transitions.end(),
                  std::uint8_t{0});
    } else if (auto* pen = state<mimusb::detail::PenState>(node); pen != nullptr) {
        pen->barrel_transitions = 0U;
        pen->in_range_transition_pending = false;
        pen->tip_transition_pending = false;
        pen->eraser_transition_pending = false;
    }
}

inline std::size_t
collect_touch(const mimusb::detail::TouchState& touch,
              std::array<const mimusb::detail::ContactState*, 16>* active) noexcept {
    std::size_t count = 0U;
    for (const auto& contact : touch.contacts) {
        if (contact.phase != ContactPhase::none) {
            (*active)[count++] = &contact;
        }
    }
    return count;
}

inline bool validate_contact(const mimusb::detail::TouchConfig& config_value,
                             const mimusb::detail::TouchContact& contact,
                             const char* operation) noexcept {
    const auto& options = config_value.fields;
    if (contact.contact_id > static_cast<std::uint32_t>(options.contact_id.logical_maximum) ||
        static_cast<std::int64_t>(contact.contact_id) < options.contact_id.logical_minimum ||
        !in_range(contact.x, options.x) || !in_range(contact.y, options.y) ||
        (options.enable_pressure == 1U && !in_range(contact.pressure, options.pressure)) ||
        (options.enable_width == 1U && !in_range(contact.width, options.width)) ||
        (options.enable_height == 1U && !in_range(contact.height, options.height)) ||
        // Linux hid-multitouch.c treats Azimuth samples as [0, Logical
        // Maximum): the maximum is the full-turn endpoint, not a valid sample,
        // even though HID logical extents are otherwise inclusive.
        (options.enable_azimuth == 1U &&
         (contact.azimuth < 0 || contact.azimuth >= options.azimuth.logical_maximum)) ||
        (options.enable_pressure == 0U && contact.pressure != 0) ||
        (options.enable_width == 0U && contact.width != 0) ||
        (options.enable_height == 0U && contact.height != 0) ||
        (options.enable_azimuth == 0U && contact.azimuth != 0)) {
        set_error(MIMUSB_ERR_PARAM, operation,
                  "A contact value is outside its declared logical range.");
        return false;
    }
    return true;
}

template <typename Function>
mimusb_result abi_state_result([[maybe_unused]] mimusb_node* node, const char* field,
                               Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        set_error(MIMUSB_ERR_INTERNAL, field,
                  "Memory allocation failed inside the C ABI boundary.");
        return MIMUSB_ERR_INTERNAL;
    } catch (...) {
        set_error(MIMUSB_ERR_INTERNAL, field,
                  "A C++ exception was contained at the C ABI boundary.");
        return MIMUSB_ERR_INTERNAL;
    }
}

} // namespace mimusb::state_build
