// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* What every profile shares: report validation at send time, error fields that name the
 * caller's structure, and the profile numbers of the ABI. */
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

static_assert(sizeof(mimusb_usage_semantic) == sizeof(std::int32_t));

void test_send_time_report_validation() {
    mimusb_keyboard_options options = keyboard_options();
    options.report_id = {1U, 5U, {0U, 0U, 0U}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;

    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    mimusb::detail::hid_node(&node)->state = std::move(state_value);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x04U, 1U) == MIMUSB_OK);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb::detail::validate_serialized_report(mimusb::detail::hid_spec(spec),
                                                            report.data(), length) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb::detail::validate_serialized_report(mimusb::detail::hid_spec(spec),
                                                            report.data(),
                                                            length - 1U) == MIMUSB_ERR_INTERNAL);

    report[0] = 4U;
    MIMUSB_CHECK(mimusb::detail::validate_serialized_report(
                     mimusb::detail::hid_spec(spec), report.data(), length) == MIMUSB_ERR_INTERNAL);
    report[0] = 5U;
    const auto* padding = field(spec, mimusb::hid::FieldSemantic::constant_padding);
    MIMUSB_CHECK(padding != nullptr);
    if (padding != nullptr) {
        report[padding->bit_offset / 8U] = static_cast<std::uint8_t>(
            report[padding->bit_offset / 8U] | (1U << (padding->bit_offset & 7U)));
        MIMUSB_CHECK(mimusb::detail::validate_serialized_report(mimusb::detail::hid_spec(spec),
                                                                report.data(),
                                                                length) == MIMUSB_ERR_INTERNAL);
    }
    mimusb_spec_release(spec);
}

// Every options error names the structure the caller filled in, not the
// implementation two profiles share.
void test_error_fields_name_their_profile() {
    const auto field_starts_with = [](const std::string_view prefix) {
        const char* field = mimusb_last_error()->field;
        return field != nullptr && std::string_view(field).substr(0, prefix.size()) == prefix;
    };
    mimusb_spec* spec = nullptr;
    mimusb_touchscreen_options touchscreen = touchscreen_options();
    touchscreen.x = {};
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&touchscreen, &spec) != MIMUSB_OK);
    MIMUSB_CHECK(field_starts_with("touchscreen"));
    mimusb_touchpad_options touchpad = touchpad_options();
    touchpad.x = {};
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&touchpad, &spec) != MIMUSB_OK);
    MIMUSB_CHECK(field_starts_with("touchpad"));
    mimusb_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.function.bcd_hid = 0x0111;
    MIMUSB_CHECK(mimusb_spec_create_pen(&pen, &spec) != MIMUSB_OK);
    MIMUSB_CHECK(field_starts_with("pen"));
    mimusb_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    digitizer.function.bcd_hid = 0x0111;
    MIMUSB_CHECK(mimusb_spec_create_digitizer(&digitizer, &spec) != MIMUSB_OK);
    MIMUSB_CHECK(field_starts_with("digitizer"));
}

} // namespace

void test_profiles_common() {
    // The profile numbers are part of the ABI.
    MIMUSB_CHECK(MIMUSB_PROFILE_KEYBOARD == 1 && MIMUSB_PROFILE_MOUSE == 2 &&
                 MIMUSB_PROFILE_TOUCHSCREEN == 3 && MIMUSB_PROFILE_TOUCHPAD == 4 &&
                 MIMUSB_PROFILE_PEN == 5 && MIMUSB_PROFILE_DIGITIZER == 6 &&
                 MIMUSB_PROFILE_GAMEPAD == 7 && MIMUSB_PROFILE_TOGGLE == 8 &&
                 MIMUSB_PROFILE_HID == 9);

    test_send_time_report_validation();
    test_error_fields_name_their_profile();
}
