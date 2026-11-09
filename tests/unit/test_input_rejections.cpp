// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The input calls' refusals on a Node without a device: values outside the Spec, flags that are
 * not booleans, a call of another profile, pending sums that would overflow, and changes while a
 * report is in flight. Each refusal leaves the Node as it was (nothing becomes dirty). */
#include "hid/usages.hpp"
#include "mimhid.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace mimhid_test;

namespace {

bool refused(const mimhid_node& node, const mimhid_result result, const mimhid_result expected,
             const char* field) {
    const char* named = mimhid_last_error()->field;
    const bool ok = result == expected && named != nullptr &&
                    std::strncmp(named, field, std::strlen(field)) == 0 && !node.dirty;
    if (!ok)
        std::fprintf(stderr, "expected %s at %s, got %s at %s\n", mimhid_result_name(expected),
                     field, mimhid_result_name(result), named != nullptr ? named : "(null)");
    return ok;
}

void test_keyboard_and_mouse() {
    mimhid_keyboard_options keyboard = keyboard_options();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&keyboard, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_node node{};
        node.spec = spec;
        mimhid::detail::KeyboardState state{};
        const std::size_t count =
            static_cast<std::size_t>(keyboard.usage_maximum - keyboard.usage_minimum) + 1U;
        state.pressed_bitmap.resize(count);
        state.key_transitions.resize(count);
        node.state = std::move(state);
        MIMHID_CHECK(
            refused(node, mimhid_keyboard_key(&node, 0x04, 2), MIMHID_ERR_PARAM, "keyboard"));
        // A call of another profile.
        MIMHID_CHECK(refused(node, mimhid_mouse_move(&node, 1, 0), MIMHID_ERR_PARAM, "mouse"));
        node.transfer_inflight.store(true);
        MIMHID_CHECK(mimhid_keyboard_key(&node, 0x04, 1) == MIMHID_ERR_BUSY);
        node.transfer_inflight.store(false);
        mimhid_spec_release(spec);
    }

    mimhid_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 2U;
    mouse.x = {-127, 127, 8U, {}};
    mouse.y = {-127, 127, 8U, {}};
    mouse.enable_wheel = 1U;
    mouse.wheel = {-31, 31, 8U, {}};
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_mouse(&mouse, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::MouseState state{};
    state.buttons.resize(2U);
    state.button_transitions.resize(2U);
    state.dx = std::numeric_limits<std::int64_t>::max();
    state.wheel = std::numeric_limits<std::int64_t>::min();
    node.state = std::move(state);
    const std::int32_t big = std::numeric_limits<std::int32_t>::max();
    MIMHID_CHECK(
        refused(node, mimhid_mouse_move(&node, big, 0), MIMHID_ERR_OVERFLOW, "mouse.delta"));
    MIMHID_CHECK(
        refused(node, mimhid_mouse_scroll(&node, -big, 0), MIMHID_ERR_OVERFLOW, "mouse.scroll"));
    MIMHID_CHECK(refused(node, mimhid_mouse_scroll(&node, 0, 1), MIMHID_ERR_UNSUPPORTED,
                         "mouse.scroll")); // no pan in the spec
    MIMHID_CHECK(refused(node, mimhid_mouse_button(&node, 3, 1), MIMHID_ERR_PARAM, "mouse.button"));
    MIMHID_CHECK(refused(node, mimhid_mouse_button(&node, 1, 2), MIMHID_ERR_PARAM, "mouse.button"));
    mimhid_spec_release(spec);
}

void test_toggle_and_gamepad() {
    static const std::array<std::uint16_t, 1> usages{0x00CDU};
    static const std::array<mimhid_usage_semantic, 1> semantics{MIMHID_USAGE_ONE_SHOT};
    static const std::array<const char*, 1> types{"TEST_TYPE"};
    static const std::array<const char*, 1> codes{"TEST_CODE"};
    mimhid_toggle_options toggle{};
    toggle.struct_size = static_cast<std::uint32_t>(sizeof(toggle));
    toggle.application_page = mimhid::hid::usage::page_consumer;
    toggle.application_usage = mimhid::hid::usage::consumer_control;
    toggle.field_page = mimhid::hid::usage::page_consumer;
    toggle.allowed_usages = usages.data();
    toggle.allowed_usage_count = usages.size();
    toggle.usage_semantics = semantics.data();
    toggle.expected_linux_event_types = types.data();
    toggle.expected_linux_codes = codes.data();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_toggle(&toggle, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_node node{};
        node.spec = spec;
        mimhid::detail::ConsumerState state{};
        state.pressed.resize(1U);
        state.transitions.resize(1U);
        node.state = std::move(state);
        MIMHID_CHECK(
            refused(node, mimhid_toggle_usage(&node, 0x00CD, 2), MIMHID_ERR_PARAM, "toggle"));
        MIMHID_CHECK(refused(node, mimhid_toggle_usage(&node, 0x00E9, 1), MIMHID_ERR_PARAM,
                             "toggle")); // not on the allow-list
        node.transfer_inflight.store(true);
        MIMHID_CHECK(
            refused(node, mimhid_toggle_usage(&node, 0x00CD, 1), MIMHID_ERR_BUSY, "toggle"));
        node.transfer_inflight.store(false);
        mimhid_spec_release(spec);
    }

    std::array<mimhid_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    mimhid_gamepad_options gamepad{};
    gamepad.struct_size = static_cast<std::uint32_t>(sizeof(gamepad));
    gamepad.axes = axes.data();
    gamepad.axis_count = axes.size();
    gamepad.button_count = 2U;
    gamepad.button_usage_minimum = 1U;
    gamepad.dpad_representation = MIMHID_DPAD_HAT;
    gamepad.hat_logical_minimum = 0;
    gamepad.hat_logical_maximum = 7;
    gamepad.hat_bit_width = 4U;
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&gamepad, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::GamepadState state{};
    state.axes.resize(2U);
    state.buttons.resize(2U);
    state.button_transitions.resize(2U);
    node.state = std::move(state);
    MIMHID_CHECK(
        refused(node, mimhid_gamepad_button(&node, 3, 1), MIMHID_ERR_PARAM, "gamepad.button"));
    MIMHID_CHECK(
        refused(node, mimhid_gamepad_button(&node, 1, 2), MIMHID_ERR_PARAM, "gamepad.button"));
    MIMHID_CHECK(refused(node, mimhid_gamepad_axis(&node, 2, 0), MIMHID_ERR_PARAM, "gamepad.axis"));
    MIMHID_CHECK(
        refused(node, mimhid_gamepad_axis(&node, 0, 128), MIMHID_ERR_PARAM, "gamepad.axis"));
    MIMHID_CHECK(refused(node, mimhid_gamepad_dpad(&node, 1, 1, 0, 0), MIMHID_ERR_PARAM,
                         "gamepad.dpad")); // up and down at once
    MIMHID_CHECK(
        refused(node, mimhid_gamepad_dpad(&node, 2, 0, 0, 0), MIMHID_ERR_PARAM, "gamepad.dpad"));
    mimhid_spec_release(spec);
}

void test_touch_and_pen() {
    mimhid_touchpad_options touchpad = touchpad_options();
    touchpad.button_count = 1U;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchpad(&touchpad, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_node node{};
        node.spec = spec;
        mimhid::detail::TouchState state{};
        state.buttons.resize(1U);
        state.button_transitions.resize(1U);
        node.state = std::move(state);
        MIMHID_CHECK(refused(node, mimhid_touchpad_button(&node, 2, 1), MIMHID_ERR_PARAM,
                             "touchpad.button"));
        MIMHID_CHECK(refused(node, mimhid_touchpad_contact(&node, 0, 1, 2000, 0, nullptr),
                             MIMHID_ERR_PARAM, "touch"));
        // Three contacts fill the slots; a fourth has none.
        for (std::uint32_t id = 0; id < 3U; ++id)
            MIMHID_CHECK(mimhid_touchpad_contact(&node, id, 1, 10, 10, nullptr) == MIMHID_OK);
        node.dirty = false;
        MIMHID_CHECK(refused(node, mimhid_touchpad_contact(&node, 3, 1, 10, 10, nullptr),
                             MIMHID_ERR_OVERFLOW, "touchpad.maximum_contacts"));
        node.transfer_inflight.store(true);
        MIMHID_CHECK(refused(node, mimhid_touchpad_contact(&node, 0, 0, 10, 10, nullptr),
                             MIMHID_ERR_BUSY, "touch"));
        node.transfer_inflight.store(false);
        mimhid_spec_release(spec);
    }

    static const std::array<std::uint16_t, 1> barrels{0x44U};
    mimhid_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.x = {0, 1000, 16U, {}};
    pen.y = {0, 1000, 16U, {}};
    pen.barrel_usages = barrels.data();
    pen.barrel_usage_count = barrels.size();
    pen.enable_hover = 1U;
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_pen(&pen, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::PenState{};
    MIMHID_CHECK(refused(node, mimhid_pen_update(&node, nullptr), MIMHID_ERR_PARAM, "pen.sample"));
    const mimhid_pen_sample good{1U, 0U, 0U, 0U, 10, 10, 0, 0, 0, 0};
    const struct {
        void (*change)(mimhid_pen_sample&);
    } changes[] = {
        {[](mimhid_pen_sample& s) { s.in_range = 2U; }},
        {[](mimhid_pen_sample& s) {
            s.in_range = 0U;
            s.tip = 1U;
        }},
        {[](mimhid_pen_sample& s) { s.eraser = 1U; }},         // no eraser in the spec
        {[](mimhid_pen_sample& s) { s.barrel_buttons = 2U; }}, // one barrel only
        {[](mimhid_pen_sample& s) { s.x = 1001; }},
        {[](mimhid_pen_sample& s) { s.pressure = 1; }}, // no pressure in the spec
        {[](mimhid_pen_sample& s) { s.tilt_x = 1; }},
        {[](mimhid_pen_sample& s) { s.twist = 1; }},
    };
    for (const auto& c : changes) {
        mimhid_pen_sample sample = good;
        c.change(sample);
        MIMHID_CHECK(
            refused(node, mimhid_pen_update(&node, &sample), MIMHID_ERR_PARAM, "pen.sample"));
    }
    // A digitizer call on a pen node is a call of another profile.
    mimhid_digitizer_sample digitizer{};
    MIMHID_CHECK(mimhid_digitizer_update(&node, &digitizer) == MIMHID_ERR_PARAM);
    mimhid_spec_release(spec);
}

} // namespace

void test_input_rejections() {
    test_keyboard_and_mouse();
    test_toggle_and_gamepad();
    test_touch_and_pen();
}
