// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* What every profile shares: report validation at send time, error fields that name the
 * caller's structure, and the profile numbers of the ABI. */
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

static_assert(sizeof(mimhid_usage_semantic) == sizeof(std::int32_t));

void test_send_time_report_validation() {
    mimhid_keyboard_options options = keyboard_options();
    options.report_id = {1U, 5U, {0U, 0U, 0U}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x04U, 1U) == MIMHID_OK);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid::detail::validate_serialized_report(spec, report.data(), length) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid::detail::validate_serialized_report(spec, report.data(), length - 1U) ==
                 MIMHID_ERR_INTERNAL);

    report[0] = 4U;
    MIMHID_CHECK(mimhid::detail::validate_serialized_report(spec, report.data(), length) ==
                 MIMHID_ERR_INTERNAL);
    report[0] = 5U;
    const auto* padding = field(spec, mimhid::hid::FieldSemantic::constant_padding);
    MIMHID_CHECK(padding != nullptr);
    if (padding != nullptr) {
        report[padding->bit_offset / 8U] = static_cast<std::uint8_t>(
            report[padding->bit_offset / 8U] | (1U << (padding->bit_offset & 7U)));
        MIMHID_CHECK(mimhid::detail::validate_serialized_report(spec, report.data(), length) ==
                     MIMHID_ERR_INTERNAL);
    }
    mimhid_spec_release(spec);
}

// Every options error names the structure the caller filled in, not the
// implementation two profiles share.
void test_error_fields_name_their_profile() {
    const auto field_starts_with = [](const std::string_view prefix) {
        const char* field = mimhid_last_error()->field;
        return field != nullptr && std::string_view(field).substr(0, prefix.size()) == prefix;
    };
    mimhid_spec* spec = nullptr;
    mimhid_touchscreen_options touchscreen = touchscreen_options();
    touchscreen.x = {};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&touchscreen, &spec) != MIMHID_OK);
    MIMHID_CHECK(field_starts_with("touchscreen"));
    mimhid_touchpad_options touchpad = touchpad_options();
    touchpad.x = {};
    MIMHID_CHECK(mimhid_spec_create_touchpad(&touchpad, &spec) != MIMHID_OK);
    MIMHID_CHECK(field_starts_with("touchpad"));
    mimhid_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    MIMHID_CHECK(mimhid_spec_create_pen(&pen, &spec) != MIMHID_OK);
    MIMHID_CHECK(field_starts_with("pen"));
    mimhid_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    MIMHID_CHECK(mimhid_spec_create_digitizer(&digitizer, &spec) != MIMHID_OK);
    MIMHID_CHECK(field_starts_with("digitizer"));
}

} // namespace

void test_profiles_common() {
    // The profile numbers are part of the ABI.
    MIMHID_CHECK(MIMHID_PROFILE_KEYBOARD == 1 && MIMHID_PROFILE_MOUSE == 2 &&
                 MIMHID_PROFILE_TOUCHSCREEN == 3 && MIMHID_PROFILE_TOUCHPAD == 4 &&
                 MIMHID_PROFILE_PEN == 5 && MIMHID_PROFILE_DIGITIZER == 6 &&
                 MIMHID_PROFILE_GAMEPAD == 7 && MIMHID_PROFILE_TOGGLE == 8 &&
                 MIMHID_PROFILE_RAW == 9);

    test_send_time_report_validation();
    test_error_fields_name_their_profile();
}
