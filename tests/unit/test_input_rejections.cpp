// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The input calls' refusals on a Node without a device: values outside the Spec, flags that are
 * not booleans, a call of another profile, pending sums that would overflow, and changes while a
 * report is in flight. Each refusal leaves the Node as it was (nothing becomes dirty). */
#include "hid/usages.hpp"
#include "mimusb.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace mimusb_test;

namespace {

bool refused(const mimusb_node& node, const mimusb_result result, const mimusb_result expected,
             const char* field) {
    const char* named = mimusb_last_error()->field;
    const bool ok = result == expected && named != nullptr &&
                    std::strncmp(named, field, std::strlen(field)) == 0 && !node.dirty;
    if (!ok)
        std::fprintf(stderr, "expected %s at %s, got %s at %s\n", mimusb_result_name(expected),
                     field, mimusb_result_name(result), named != nullptr ? named : "(null)");
    return ok;
}

void test_keyboard_and_mouse() {
    mimusb_keyboard_options keyboard = keyboard_options();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&keyboard, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_node node{};
        node.spec = spec;
        mimusb::detail::KeyboardState state{};
        const std::size_t count =
            static_cast<std::size_t>(keyboard.usage_maximum - keyboard.usage_minimum) + 1U;
        state.pressed_bitmap.resize(count);
        state.key_transitions.resize(count);
        node.state = std::move(state);
        MIMUSB_CHECK(
            refused(node, mimusb_keyboard_key(&node, 0x04, 2), MIMUSB_ERR_PARAM, "keyboard"));
        // A call of another profile.
        MIMUSB_CHECK(refused(node, mimusb_mouse_move(&node, 1, 0), MIMUSB_ERR_PARAM, "mouse"));
        node.transfer_inflight.store(true);
        MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x04, 1) == MIMUSB_ERR_BUSY);
        node.transfer_inflight.store(false);
        mimusb_spec_release(spec);
    }

    mimusb_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 2U;
    mouse.x = {-127, 127, 8U, {}};
    mouse.y = {-127, 127, 8U, {}};
    mouse.enable_wheel = 1U;
    mouse.wheel = {-31, 31, 8U, {}};
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_mouse(&mouse, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_node node{};
    node.spec = spec;
    mimusb::detail::MouseState state{};
    state.buttons.resize(2U);
    state.button_transitions.resize(2U);
    state.dx = std::numeric_limits<std::int64_t>::max();
    state.wheel = std::numeric_limits<std::int64_t>::min();
    node.state = std::move(state);
    const std::int32_t big = std::numeric_limits<std::int32_t>::max();
    MIMUSB_CHECK(
        refused(node, mimusb_mouse_move(&node, big, 0), MIMUSB_ERR_OVERFLOW, "mouse.delta"));
    MIMUSB_CHECK(
        refused(node, mimusb_mouse_scroll(&node, -big, 0), MIMUSB_ERR_OVERFLOW, "mouse.scroll"));
    MIMUSB_CHECK(refused(node, mimusb_mouse_scroll(&node, 0, 1), MIMUSB_ERR_UNSUPPORTED,
                         "mouse.scroll")); // no pan in the spec
    MIMUSB_CHECK(refused(node, mimusb_mouse_button(&node, 3, 1), MIMUSB_ERR_PARAM, "mouse.button"));
    MIMUSB_CHECK(refused(node, mimusb_mouse_button(&node, 1, 2), MIMUSB_ERR_PARAM, "mouse.button"));
    mimusb_spec_release(spec);
}

void test_toggle_and_gamepad() {
    static const std::array<std::uint16_t, 1> usages{0x00CDU};
    static const std::array<mimusb_usage_semantic, 1> semantics{MIMUSB_USAGE_ONE_SHOT};
    static const std::array<const char*, 1> types{"TEST_TYPE"};
    static const std::array<const char*, 1> codes{"TEST_CODE"};
    mimusb_toggle_options toggle{};
    toggle.struct_size = static_cast<std::uint32_t>(sizeof(toggle));
    toggle.application_page = mimusb::hid::usage::page_consumer;
    toggle.application_usage = mimusb::hid::usage::consumer_control;
    toggle.field_page = mimusb::hid::usage::page_consumer;
    toggle.allowed_usages = usages.data();
    toggle.allowed_usage_count = usages.size();
    toggle.usage_semantics = semantics.data();
    toggle.expected_linux_event_types = types.data();
    toggle.expected_linux_codes = codes.data();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_toggle(&toggle, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_node node{};
        node.spec = spec;
        mimusb::detail::ConsumerState state{};
        state.pressed.resize(1U);
        state.transitions.resize(1U);
        node.state = std::move(state);
        MIMUSB_CHECK(
            refused(node, mimusb_toggle_usage(&node, 0x00CD, 2), MIMUSB_ERR_PARAM, "toggle"));
        MIMUSB_CHECK(refused(node, mimusb_toggle_usage(&node, 0x00E9, 1), MIMUSB_ERR_PARAM,
                             "toggle")); // not on the allow-list
        node.transfer_inflight.store(true);
        MIMUSB_CHECK(
            refused(node, mimusb_toggle_usage(&node, 0x00CD, 1), MIMUSB_ERR_BUSY, "toggle"));
        node.transfer_inflight.store(false);
        mimusb_spec_release(spec);
    }

    std::array<mimusb_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMUSB_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMUSB_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    mimusb_gamepad_options gamepad{};
    gamepad.struct_size = static_cast<std::uint32_t>(sizeof(gamepad));
    gamepad.axes = axes.data();
    gamepad.axis_count = axes.size();
    gamepad.button_count = 2U;
    gamepad.button_usage_minimum = 1U;
    gamepad.dpad_representation = MIMUSB_DPAD_HAT;
    gamepad.hat_logical_minimum = 0;
    gamepad.hat_logical_maximum = 7;
    gamepad.hat_bit_width = 4U;
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&gamepad, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_node node{};
    node.spec = spec;
    mimusb::detail::GamepadState state{};
    state.axes.resize(2U);
    state.buttons.resize(2U);
    state.button_transitions.resize(2U);
    node.state = std::move(state);
    MIMUSB_CHECK(
        refused(node, mimusb_gamepad_button(&node, 3, 1), MIMUSB_ERR_PARAM, "gamepad.button"));
    MIMUSB_CHECK(
        refused(node, mimusb_gamepad_button(&node, 1, 2), MIMUSB_ERR_PARAM, "gamepad.button"));
    MIMUSB_CHECK(refused(node, mimusb_gamepad_axis(&node, 2, 0), MIMUSB_ERR_PARAM, "gamepad.axis"));
    MIMUSB_CHECK(
        refused(node, mimusb_gamepad_axis(&node, 0, 128), MIMUSB_ERR_PARAM, "gamepad.axis"));
    MIMUSB_CHECK(refused(node, mimusb_gamepad_dpad(&node, 1, 1, 0, 0), MIMUSB_ERR_PARAM,
                         "gamepad.dpad")); // up and down at once
    MIMUSB_CHECK(
        refused(node, mimusb_gamepad_dpad(&node, 2, 0, 0, 0), MIMUSB_ERR_PARAM, "gamepad.dpad"));
    mimusb_spec_release(spec);
}

void test_touch_and_pen() {
    mimusb_touchpad_options touchpad = touchpad_options();
    touchpad.button_count = 1U;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&touchpad, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_node node{};
        node.spec = spec;
        mimusb::detail::TouchState state{};
        state.buttons.resize(1U);
        state.button_transitions.resize(1U);
        node.state = std::move(state);
        MIMUSB_CHECK(refused(node, mimusb_touchpad_button(&node, 2, 1), MIMUSB_ERR_PARAM,
                             "touchpad.button"));
        MIMUSB_CHECK(refused(node, mimusb_touchpad_contact(&node, 0, 1, 2000, 0, nullptr),
                             MIMUSB_ERR_PARAM, "touch"));
        // Three contacts fill the slots; a fourth has none.
        for (std::uint32_t id = 0; id < 3U; ++id)
            MIMUSB_CHECK(mimusb_touchpad_contact(&node, id, 1, 10, 10, nullptr) == MIMUSB_OK);
        node.dirty = false;
        MIMUSB_CHECK(refused(node, mimusb_touchpad_contact(&node, 3, 1, 10, 10, nullptr),
                             MIMUSB_ERR_OVERFLOW, "touchpad.maximum_contacts"));
        node.transfer_inflight.store(true);
        MIMUSB_CHECK(refused(node, mimusb_touchpad_contact(&node, 0, 0, 10, 10, nullptr),
                             MIMUSB_ERR_BUSY, "touch"));
        node.transfer_inflight.store(false);
        mimusb_spec_release(spec);
    }

    static const std::array<std::uint16_t, 1> barrels{0x44U};
    mimusb_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.x = {0, 1000, 16U, {}};
    pen.y = {0, 1000, 16U, {}};
    pen.barrel_usages = barrels.data();
    pen.barrel_usage_count = barrels.size();
    pen.enable_hover = 1U;
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_pen(&pen, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_node node{};
    node.spec = spec;
    node.state = mimusb::detail::PenState{};
    MIMUSB_CHECK(refused(node, mimusb_pen_update(&node, nullptr), MIMUSB_ERR_PARAM, "pen.sample"));
    const mimusb_pen_sample good{1U, 0U, 0U, 0U, 10, 10, 0, 0, 0, 0};
    const struct {
        void (*change)(mimusb_pen_sample&);
    } changes[] = {
        {[](mimusb_pen_sample& s) { s.in_range = 2U; }},
        {[](mimusb_pen_sample& s) {
            s.in_range = 0U;
            s.tip = 1U;
        }},
        {[](mimusb_pen_sample& s) { s.eraser = 1U; }},         // no eraser in the spec
        {[](mimusb_pen_sample& s) { s.barrel_buttons = 2U; }}, // one barrel only
        {[](mimusb_pen_sample& s) { s.x = 1001; }},
        {[](mimusb_pen_sample& s) { s.pressure = 1; }}, // no pressure in the spec
        {[](mimusb_pen_sample& s) { s.tilt_x = 1; }},
        {[](mimusb_pen_sample& s) { s.twist = 1; }},
    };
    for (const auto& c : changes) {
        mimusb_pen_sample sample = good;
        c.change(sample);
        MIMUSB_CHECK(
            refused(node, mimusb_pen_update(&node, &sample), MIMUSB_ERR_PARAM, "pen.sample"));
    }
    // A digitizer call on a pen node is a call of another profile.
    mimusb_digitizer_sample digitizer{};
    MIMUSB_CHECK(mimusb_digitizer_update(&node, &digitizer) == MIMUSB_ERR_PARAM);
    mimusb_spec_release(spec);
}

} // namespace

void test_input_rejections() {
    test_keyboard_and_mouse();
    test_toggle_and_gamepad();
    test_touch_and_pen();
}
