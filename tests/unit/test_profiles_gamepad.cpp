// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Gamepad profile: the hat switch domain, the D-pad and the extended axes. */
#include "api/internal.hpp"
#include "classes/hid/hid_class.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimusb.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

using namespace mimusb_test;

namespace {

void test_hat_domain() {
    std::array<mimusb_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMUSB_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMUSB_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    mimusb_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.function.bcd_hid = 0x0111;
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMUSB_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 15;
    options.hat_bit_width = 4U;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    options.hat_logical_maximum = 7;
    check_battery_variant(options, &mimusb_spec_create_gamepad);
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMUSB_CHECK(mimusb_spec_manifest(spec, &manifest) == MIMUSB_OK);
        mimusb_node node{};
        mimusb::detail::attach_hid(&node, spec);
        mimusb::detail::GamepadState state_value{};
        state_value.axes.resize(axes.size());
        state_value.buttons.resize(options.button_count);
        state_value.button_transitions.resize(options.button_count);
        mimusb::detail::hid_node(&node)->state = std::move(state_value);
        std::array<std::uint8_t, 32U> report{};
        std::size_t length = 0U;
        MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                    report.size(), &length) == MIMUSB_OK);
        const auto* hat = field(spec, mimusb::hid::FieldSemantic::hat);
        MIMUSB_CHECK(hat != nullptr && extract(report.data(), *hat) == 15U);
    }
    mimusb_spec_release(spec);

    axes[0].usage_page = 0x02U;
    axes[0].usage = 0xC4U;
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[0].usage_page = 0x01U;
    axes[0].usage = 0x30U;

    axes[1].role = MIMUSB_AXIS_X;
    axes[1].usage = 0x30U;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[1].role = MIMUSB_AXIS_Y;
    axes[1].usage = 0x31U;

    axes[1].expected_linux_code = "";
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_UNSET_FIELD);
}

void test_controller_dpad_and_extended_axes() {
    std::array<mimusb_gamepad_axis_option, 6U> axes{
        mimusb_gamepad_axis_option{MIMUSB_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"},
        mimusb_gamepad_axis_option{MIMUSB_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"},
        mimusb_gamepad_axis_option{MIMUSB_AXIS_DIAL, 0x01U, 0x37U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimusb_gamepad_axis_option{MIMUSB_AXIS_WHEEL, 0x01U, 0x38U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimusb_gamepad_axis_option{
            MIMUSB_AXIS_SIMULATION_RUDDER, 0x02U, 0xBAU, {-127, 127, 8U, {}}, 0, "TEST"},
        mimusb_gamepad_axis_option{
            MIMUSB_AXIS_SIMULATION_THROTTLE, 0x02U, 0xBBU, {0, 255, 8U, {}}, 0, "TEST"}};
    mimusb_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.function.bcd_hid = 0x0111;
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMUSB_DPAD_BUTTONS;

    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_capability_manifest raw_dpad_manifest{};
    raw_dpad_manifest.struct_size = static_cast<std::uint32_t>(sizeof(raw_dpad_manifest));
    MIMUSB_CHECK(mimusb_spec_manifest(spec, &raw_dpad_manifest) == MIMUSB_OK);
    MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0x09U, 0x90U) == 1U);
    MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0x09U, 0x91U) == 1U);
    MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0x09U, 0x92U) == 1U);
    MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0x09U, 0x93U) == 1U);
    MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0x81U, 0x22U) == 4U);

    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::GamepadState gamepad{};
    gamepad.axes.resize(axes.size());
    gamepad.buttons.resize(options.button_count);
    gamepad.button_transitions.resize(options.button_count);
    mimusb::detail::hid_node(&node)->state = std::move(gamepad);
    mimusb::detail::hid_node(&node)->dirty = false;
    MIMUSB_CHECK(mimusb_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb::detail::hid_node(&node)->dirty);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    const auto* up = field(spec, mimusb::hid::FieldSemantic::buttons, 1U);
    const auto* down = field(spec, mimusb::hid::FieldSemantic::buttons, 2U);
    const auto* right = field(spec, mimusb::hid::FieldSemantic::buttons, 3U);
    const auto* left = field(spec, mimusb::hid::FieldSemantic::buttons, 4U);
    MIMUSB_CHECK(up != nullptr && extract(report.data(), *up) == 1U);
    MIMUSB_CHECK(down != nullptr && extract(report.data(), *down) == 0U);
    MIMUSB_CHECK(right != nullptr && extract(report.data(), *right) == 1U);
    MIMUSB_CHECK(left != nullptr && extract(report.data(), *left) == 0U);
    MIMUSB_CHECK(mimusb_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMUSB_ERR_BUSY);
    mimusb::detail::hid_node(&node)->dirty = false;
    MIMUSB_CHECK(mimusb_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMUSB_OK);
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->dirty);
    mimusb::detail::transfer_complete(mimusb::detail::hid_node(&node), MIMUSB_OK, 0);
    MIMUSB_CHECK(mimusb_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb::detail::hid_node(&node)->dirty);
    mimusb::detail::hid_node(&node)->dirty = false;
    MIMUSB_CHECK(mimusb_gamepad_axis(&node, 0U, 0) == MIMUSB_OK);
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->dirty);
    mimusb_spec_release(spec);
    spec = nullptr;

    options.button_count = 4U;
    options.dpad_representation = MIMUSB_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 7;
    options.hat_bit_width = 4U;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_capability_manifest four_button_manifest{};
        four_button_manifest.struct_size = static_cast<std::uint32_t>(sizeof(four_button_manifest));
        MIMUSB_CHECK(mimusb_spec_manifest(spec, &four_button_manifest) == MIMUSB_OK);
        mimusb_spec_release(spec);
        spec = nullptr;
    }
    options.button_count = 5U;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMUSB_CHECK(mimusb_spec_manifest(spec, &manifest) == MIMUSB_OK);
        const auto* hat = field(spec, mimusb::hid::FieldSemantic::hat);
        MIMUSB_CHECK(hat != nullptr && hat->physical.enabled == 1U && hat->physical.minimum == 0 &&
                     hat->physical.maximum == 315 && hat->physical.unit_exponent == 0 &&
                     hat->physical.unit == 0x14U);
        mimusb_node hat_node{};
        mimusb::detail::attach_hid(&hat_node, spec);
        mimusb::detail::GamepadState hat_state{};
        hat_state.axes.resize(axes.size());
        hat_state.buttons.resize(options.button_count);
        hat_state.button_transitions.resize(options.button_count);
        mimusb::detail::hid_node(&hat_node)->state = std::move(hat_state);
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
            MIMUSB_CHECK(mimusb_gamepad_dpad(&hat_node, direction.up, direction.down,
                                             direction.right, direction.left) == MIMUSB_OK);
            report.fill(0U);
            MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&hat_node),
                                                        report.data(), report.size(),
                                                        &length) == MIMUSB_OK);
            MIMUSB_CHECK(hat != nullptr && extract(report.data(), *hat) == direction.wire);
            mimusb::detail::transfer_complete(mimusb::detail::hid_node(&hat_node), MIMUSB_OK, 0);
            mimusb::detail::hid_node(&hat_node)->dirty = false;
        }
        MIMUSB_CHECK(mimusb_gamepad_dpad(&hat_node, 1U, 1U, 0U, 0U) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_gamepad_dpad(&hat_node, 0U, 0U, 1U, 1U) == MIMUSB_ERR_PARAM);
        report.fill(0U);
        MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&hat_node),
                                                    report.data(), report.size(),
                                                    &length) == MIMUSB_OK);
        MIMUSB_CHECK(hat != nullptr && extract(report.data(), *hat) == 7U);
        MIMUSB_CHECK(!mimusb::detail::hid_node(&hat_node)->dirty);
        mimusb_spec_release(spec);
        spec = nullptr;
    }

    axes[4].neutral_value = 1;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[4].neutral_value = 0;
    axes[4].value = {0, 127, 8U, {}};
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[4].value = {-127, 127, 8U, {}};
    axes[5].neutral_value = 1;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[5].neutral_value = 0;
    axes[5].value = {-1, 255, 16U, {}};
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
    axes[5].value = {0, 255, 8U, {}};

    options.dpad_representation = MIMUSB_DPAD_NONE;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 0;
    options.hat_bit_width = 4U;
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&options, &spec) == MIMUSB_ERR_PARAM);
}

} // namespace

void test_profiles_gamepad() {
    test_hat_domain();
    test_controller_dpad_and_extended_axes();
}
