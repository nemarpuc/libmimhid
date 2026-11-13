// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The mouse profile's state: relative motion, wheel, pan and buttons.
 */

#include "classes/hid/state_support.hpp"

using namespace mimusb::state_build;

extern "C" {

static mimusb_result mimusb_mouse_move_impl(mimusb::detail::HidNode* node, const std::int32_t dx,
                                            const std::int32_t dy) {
    mimusb::detail::clear_error();
    const mimusb_result check = usable(node, MIMUSB_PROFILE_MOUSE, "mouse.move");
    if (check != MIMUSB_OK)
        return check;
    auto* mouse = state<mimusb::detail::MouseState>(node);
    std::int64_t next_dx = mouse->dx;
    std::int64_t next_dy = mouse->dy;
    if (!add_checked(&next_dx, dx) || !add_checked(&next_dy, dy)) {
        set_error(MIMUSB_ERR_OVERFLOW, "mouse.delta",
                  "The pending relative movement overflowed int64_t.");
        return MIMUSB_ERR_OVERFLOW;
    }
    mouse->dx = next_dx;
    mouse->dy = next_dy;
    node->dirty = node->dirty || dx != 0 || dy != 0;
    return MIMUSB_OK;
}

static mimusb_result mimusb_mouse_scroll_impl(mimusb::detail::HidNode* node,
                                              const std::int32_t wheel, const std::int32_t pan) {
    mimusb::detail::clear_error();
    const mimusb_result check = usable(node, MIMUSB_PROFILE_MOUSE, "mouse.scroll");
    if (check != MIMUSB_OK)
        return check;
    auto* mouse = state<mimusb::detail::MouseState>(node);
    const auto* spec = config<mimusb::detail::MouseConfig>(node);
    if ((wheel != 0 && spec->options.enable_wheel != 1U) ||
        (pan != 0 && spec->options.enable_pan != 1U)) {
        set_error(MIMUSB_ERR_UNSUPPORTED, "mouse.scroll",
                  "The requested scroll axis is absent from the spec.");
        return MIMUSB_ERR_UNSUPPORTED;
    }
    std::int64_t next_wheel = mouse->wheel;
    std::int64_t next_pan = mouse->pan;
    if (!add_checked(&next_wheel, wheel) || !add_checked(&next_pan, pan)) {
        set_error(MIMUSB_ERR_OVERFLOW, "mouse.scroll",
                  "The pending relative scroll overflowed int64_t.");
        return MIMUSB_ERR_OVERFLOW;
    }
    mouse->wheel = next_wheel;
    mouse->pan = next_pan;
    node->dirty = node->dirty || wheel != 0 || pan != 0;
    return MIMUSB_OK;
}

static mimusb_result mimusb_mouse_button_impl(mimusb::detail::HidNode* node,
                                              const std::uint32_t button,
                                              const std::uint32_t down) {
    mimusb::detail::clear_error();
    const mimusb_result check = usable(node, MIMUSB_PROFILE_MOUSE, "mouse.button");
    if (check != MIMUSB_OK)
        return check;
    auto* mouse = state<mimusb::detail::MouseState>(node);
    if (button == 0U || button > mouse->buttons.size() || !mimusb::detail::valid_boolean(down)) {
        set_error(MIMUSB_ERR_PARAM, "mouse.button",
                  "Button is one-based and down must be zero or one.");
        return MIMUSB_ERR_PARAM;
    }
    if (!transition_storage_matches(mouse->buttons, mouse->button_transitions, "mouse.button")) {
        return MIMUSB_ERR_INTERNAL;
    }
    const std::size_t index = button - 1U;
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = mouse->buttons[index] != value;
    if (changed && mouse->button_transitions[index] != 0U) {
        return pending_transition("mouse.button");
    }
    if (changed) {
        mouse->buttons[index] = value;
        mouse->button_transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return MIMUSB_OK;
}

// The usage/down order is the fixed, already-shipped mimusb_toggle_usage(node,
// usage, down) ABI order and cannot be changed to avoid this warning.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)

mimusb_result MIMUSB_CALL mimusb_mouse_move(mimusb_node* node, const std::int32_t dx,
                                            const std::int32_t dy) {
    return abi_state_result(node, "mouse.move", [&] {
        return mimusb_mouse_move_impl(mimusb::detail::hid_node(node), dx, dy);
    });
}

mimusb_result MIMUSB_CALL mimusb_mouse_scroll(mimusb_node* node, const std::int32_t wheel,
                                              const std::int32_t pan) {
    return abi_state_result(node, "mouse.scroll", [&] {
        return mimusb_mouse_scroll_impl(mimusb::detail::hid_node(node), wheel, pan);
    });
}

mimusb_result MIMUSB_CALL mimusb_mouse_button(mimusb_node* node, const std::uint32_t button,
                                              const std::uint32_t down) {
    return abi_state_result(node, "mouse.button", [&] {
        return mimusb_mouse_button_impl(mimusb::detail::hid_node(node), button, down);
    });
}

} // extern "C"
