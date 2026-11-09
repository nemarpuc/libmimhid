// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Gamepad profile: the hat switch domain, the D-pad and the extended axes. */
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimhid.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

using namespace mimhid_test;

namespace {

void test_hat_domain() {
    std::array<mimhid_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    mimhid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 15;
    options.hat_bit_width = 4U;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    options.hat_logical_maximum = 7;
    check_battery_variant(options, &mimhid_spec_create_gamepad);
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        mimhid_node node{};
        node.spec = spec;
        mimhid::detail::GamepadState state_value{};
        state_value.axes.resize(axes.size());
        state_value.buttons.resize(options.button_count);
        state_value.button_transitions.resize(options.button_count);
        node.state = std::move(state_value);
        std::array<std::uint8_t, 32U> report{};
        std::size_t length = 0U;
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                     MIMHID_OK);
        const auto* hat = field(spec, mimhid::hid::FieldSemantic::hat);
        MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 15U);
    }
    mimhid_spec_release(spec);

    axes[0].usage_page = 0x02U;
    axes[0].usage = 0xC4U;
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[0].usage_page = 0x01U;
    axes[0].usage = 0x30U;

    axes[1].role = MIMHID_AXIS_X;
    axes[1].usage = 0x30U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[1].role = MIMHID_AXIS_Y;
    axes[1].usage = 0x31U;

    axes[1].expected_linux_code = "";
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_UNSET_FIELD);
}

void test_controller_dpad_and_extended_axes() {
    std::array<mimhid_gamepad_axis_option, 6U> axes{
        mimhid_gamepad_axis_option{MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"},
        mimhid_gamepad_axis_option{MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"},
        mimhid_gamepad_axis_option{MIMHID_AXIS_DIAL, 0x01U, 0x37U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{MIMHID_AXIS_WHEEL, 0x01U, 0x38U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_SIMULATION_RUDDER, 0x02U, 0xBAU, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_SIMULATION_THROTTLE, 0x02U, 0xBBU, {0, 255, 8U, {}}, 0, "TEST"}};
    mimhid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMHID_DPAD_BUTTONS;

    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_capability_manifest raw_dpad_manifest{};
    raw_dpad_manifest.struct_size = static_cast<std::uint32_t>(sizeof(raw_dpad_manifest));
    MIMHID_CHECK(mimhid_spec_manifest(spec, &raw_dpad_manifest) == MIMHID_OK);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x90U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x91U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x92U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x93U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 4U);

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::GamepadState gamepad{};
    gamepad.axes.resize(axes.size());
    gamepad.buttons.resize(options.button_count);
    gamepad.button_transitions.resize(options.button_count);
    node.state = std::move(gamepad);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMHID_OK);
    MIMHID_CHECK(node.dirty);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* up = field(spec, mimhid::hid::FieldSemantic::buttons, 1U);
    const auto* down = field(spec, mimhid::hid::FieldSemantic::buttons, 2U);
    const auto* right = field(spec, mimhid::hid::FieldSemantic::buttons, 3U);
    const auto* left = field(spec, mimhid::hid::FieldSemantic::buttons, 4U);
    MIMHID_CHECK(up != nullptr && extract(report.data(), *up) == 1U);
    MIMHID_CHECK(down != nullptr && extract(report.data(), *down) == 0U);
    MIMHID_CHECK(right != nullptr && extract(report.data(), *right) == 1U);
    MIMHID_CHECK(left != nullptr && extract(report.data(), *left) == 0U);
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMHID_ERR_BUSY);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMHID_OK);
    MIMHID_CHECK(node.dirty);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_axis(&node, 0U, 0) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);
    mimhid_spec_release(spec);
    spec = nullptr;

    options.button_count = 4U;
    options.dpad_representation = MIMHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 7;
    options.hat_bit_width = 4U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest four_button_manifest{};
        four_button_manifest.struct_size = static_cast<std::uint32_t>(sizeof(four_button_manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &four_button_manifest) == MIMHID_OK);
        mimhid_spec_release(spec);
        spec = nullptr;
    }
    options.button_count = 5U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        const auto* hat = field(spec, mimhid::hid::FieldSemantic::hat);
        MIMHID_CHECK(hat != nullptr && hat->physical.enabled == 1U && hat->physical.minimum == 0 &&
                     hat->physical.maximum == 315 && hat->physical.unit_exponent == 0 &&
                     hat->physical.unit == 0x14U);
        mimhid_node hat_node{};
        hat_node.spec = spec;
        mimhid::detail::GamepadState hat_state{};
        hat_state.axes.resize(axes.size());
        hat_state.buttons.resize(options.button_count);
        hat_state.button_transitions.resize(options.button_count);
        hat_node.state = std::move(hat_state);
        struct DirectionCase {
            std::uint32_t up;
            std::uint32_t down;
            std::uint32_t right;
            std::uint32_t left;
            std::uint32_t wire;
        };
        const std::array<DirectionCase, 9U> directions{
            DirectionCase{0U, 0U, 0U, 0U, 15U}, DirectionCase{1U, 0U, 0U, 0U, 0U},
            DirectionCase{1U, 0U, 1U, 0U, 1U},  DirectionCase{0U, 0U, 1U, 0U, 2U},
            DirectionCase{0U, 1U, 1U, 0U, 3U},  DirectionCase{0U, 1U, 0U, 0U, 4U},
            DirectionCase{0U, 1U, 0U, 1U, 5U},  DirectionCase{0U, 0U, 0U, 1U, 6U},
            DirectionCase{1U, 0U, 0U, 1U, 7U}};
        for (const auto& direction : directions) {
            MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, direction.up, direction.down,
                                             direction.right, direction.left) == MIMHID_OK);
            report.fill(0U);
            MIMHID_CHECK(mimhid::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                        &length) == MIMHID_OK);
            MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == direction.wire);
            mimhid::detail::transfer_complete(&hat_node, MIMHID_OK, 0);
            hat_node.dirty = false;
        }
        MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, 1U, 1U, 0U, 0U) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, 0U, 0U, 1U, 1U) == MIMHID_ERR_PARAM);
        report.fill(0U);
        MIMHID_CHECK(mimhid::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                    &length) == MIMHID_OK);
        MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 7U);
        MIMHID_CHECK(!hat_node.dirty);
        mimhid_spec_release(spec);
        spec = nullptr;
    }

    axes[4].neutral_value = 1;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[4].neutral_value = 0;
    axes[4].value = {0, 127, 8U, {}};
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[4].value = {-127, 127, 8U, {}};
    axes[5].neutral_value = 1;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[5].neutral_value = 0;
    axes[5].value = {-1, 255, 16U, {}};
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[5].value = {0, 255, 8U, {}};

    options.dpad_representation = MIMHID_DPAD_NONE;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 0;
    options.hat_bit_width = 4U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
}

} // namespace

void test_profiles_gamepad() {
    test_hat_domain();
    test_controller_dpad_and_extended_axes();
}
