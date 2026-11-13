// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The battery report every profile can add: the builder, the options and the state. */
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

void test_battery_builder() {
    using mimusb::hid::BatteryField;
    using mimusb::hid::DescriptorBuilder;
    using mimusb::hid::FieldSemantic;
    using mimusb::hid::ReportLayout;

    const BatteryField battery{2U, 1, 100, 8U, {}};
    const auto emit = [&](DescriptorBuilder& builder) {
        return builder.begin_application(0x01U, 0x06U) && builder.set_report_id(true, 1U) &&
               builder.variable_range(0x07U, 0xE0U, 0xE7U, 1U, FieldSemantic::modifier) &&
               builder.end_collection();
    };
    ReportLayout input;
    ReportLayout battery_layout;
    DescriptorBuilder counting(nullptr, 65535U, &input);
    counting.set_battery(battery, &battery_layout);
    MIMUSB_CHECK(emit(counting));
    counting.finish();
    MIMUSB_CHECK(counting.good());
    std::vector<std::uint8_t> bytes(counting.descriptor_size());
    DescriptorBuilder emitting(bytes.data(), bytes.size(), nullptr);
    emitting.set_battery(battery, nullptr);
    MIMUSB_CHECK(emit(emitting));
    emitting.finish();
    MIMUSB_CHECK(emitting.good() && emitting.descriptor_size() == bytes.size());

    // Input layout is untouched by the battery report.
    MIMUSB_CHECK(input.has_report_id && input.report_id == 1U && input.wire_bytes == 2U);
    MIMUSB_CHECK(battery_layout.has_report_id && battery_layout.report_id == 2U &&
                 battery_layout.wire_bytes == 2U && battery_layout.fields.size() == 1U &&
                 battery_layout.fields[0].semantic == FieldSemantic::battery_strength &&
                 battery_layout.fields[0].bit_offset == 8U &&
                 battery_layout.fields[0].logical_minimum == 1 &&
                 battery_layout.fields[0].logical_maximum == 100);

    const std::vector<std::uint8_t> tail{0x85U, 0x02U, 0x05U, 0x06U, 0x09U, 0x20U,
                                         0x15U, 0x01U, 0x25U, 0x64U, 0x75U, 0x08U,
                                         0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    MIMUSB_CHECK(bytes.size() > tail.size() &&
                 std::equal(tail.begin(), tail.end(),
                            bytes.end() - static_cast<std::ptrdiff_t>(tail.size())));

    MIMUSB_CHECK(mimusb::hid::validate_generated(bytes.data(), bytes.size(), input, &battery_layout)
                     .result == MIMUSB_OK);
    // The same bytes checked against a wrong battery layout are refused.
    ReportLayout wrong = battery_layout;
    wrong.report_id = 3U;
    MIMUSB_CHECK(
        mimusb::hid::validate_generated(bytes.data(), bytes.size(), input, &wrong).result ==
        MIMUSB_ERR_INTERNAL);
    // Without a battery layout a second Report ID is still refused.
    MIMUSB_CHECK(mimusb::hid::validate_generated(bytes.data(), bytes.size(), input).result ==
                 MIMUSB_ERR_INTERNAL);

    // A 7-bit field is padded to a whole byte.
    const BatteryField narrow{2U, 0, 100, 7U, {}};
    ReportLayout narrow_input;
    ReportLayout narrow_layout;
    DescriptorBuilder narrow_builder(nullptr, 65535U, &narrow_input);
    narrow_builder.set_battery(narrow, &narrow_layout);
    MIMUSB_CHECK(emit(narrow_builder));
    narrow_builder.finish();
    MIMUSB_CHECK(narrow_builder.good() && narrow_layout.wire_bytes == 2U &&
                 narrow_layout.fields.size() == 2U && !narrow_layout.fields[0].has_null_state &&
                 narrow_layout.fields[1].semantic == FieldSemantic::constant_padding);
}

void test_battery_options() {
    mimusb_keyboard_options keyboard = keyboard_options();
    check_battery_variant(keyboard, &mimusb_spec_create_keyboard);

    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    const auto rejects = [&](const char* field) {
        mimusb_spec* refused = nullptr;
        const bool param = mimusb_spec_create_keyboard(&keyboard, &refused) == MIMUSB_ERR_PARAM;
        const std::string_view named = mimusb_last_error()->field;
        mimusb_spec_release(refused);
        return param && named == field;
    };
    const mimusb_keyboard_options good = keyboard;

    keyboard.enable_battery = 2U;
    MIMUSB_CHECK(rejects("keyboard.enable_battery"));
    keyboard = good;
    keyboard.enable_battery = 0U; // battery left filled in
    MIMUSB_CHECK(rejects("keyboard.battery"));
    keyboard = good;
    keyboard.report_id = {0U, 0U, {0U, 0U, 0U}}; // input report has no ID
    MIMUSB_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id = {0U, 0U, {0U, 0U, 0U}};
    MIMUSB_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id.value = 1U; // same as the input report
    MIMUSB_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.strength = {100, 1, 8U, {}};
    MIMUSB_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {-1, 100, 8U, {}};
    MIMUSB_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {1, 300, 8U, {}}; // does not fit the width
    MIMUSB_CHECK(rejects("keyboard.battery.strength"));
}

void test_battery_state() {
    mimusb_keyboard_options keyboard = keyboard_options();
    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    mimusb_spec* plain = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&keyboard, &plain) == MIMUSB_OK);
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {0, 100, 8U, {}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&keyboard, &spec) == MIMUSB_OK);
    if (plain == nullptr || spec == nullptr) {
        mimusb_spec_release(plain);
        mimusb_spec_release(spec);
        return;
    }

    mimusb_node without{};
    mimusb::detail::attach_hid(&without, plain);
    mimusb::detail::hid_node(&without)->state = mimusb::detail::KeyboardState{};
    MIMUSB_CHECK(mimusb_battery_update(&without, 50) == MIMUSB_ERR_PARAM);

    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::hid_node(&node)->state = mimusb::detail::KeyboardState{};
    std::array<std::uint8_t, 8U> report{};
    std::size_t length = 0U;

    // Nothing was ever set, so there is nothing to send.
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->battery_dirty);
    // The first value is a change even when it is 0, the initial storage.
    MIMUSB_CHECK(mimusb_battery_update(&node, 0) == MIMUSB_OK &&
                 mimusb::detail::hid_node(&node)->battery_dirty);
    MIMUSB_CHECK(mimusb::detail::serialize_battery(mimusb::detail::hid_node(&node), report.data(),
                                                   report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(length == 2U && report[0] == 2U && report[1] == 0U);

    MIMUSB_CHECK(mimusb_battery_update(&node, 80) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb::detail::hid_node(&node)->battery_dirty &&
                 !mimusb::detail::hid_node(&node)->dirty);
    MIMUSB_CHECK(mimusb::detail::serialize_battery(mimusb::detail::hid_node(&node), report.data(),
                                                   report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(length == 2U && report[0] == 2U && report[1] == 80U);
    MIMUSB_CHECK(mimusb::detail::validate_serialized_report(
                     mimusb::detail::hid_spec(spec), mimusb::detail::hid_spec(spec)->battery_layout,
                     report.data(), length) == MIMUSB_OK);
    report[1] = 101U;
    MIMUSB_CHECK(mimusb::detail::validate_serialized_report(
                     mimusb::detail::hid_spec(spec), mimusb::detail::hid_spec(spec)->battery_layout,
                     report.data(), length) == MIMUSB_ERR_INTERNAL);

    // The same value again asks for nothing.
    mimusb::detail::hid_node(&node)->battery_dirty = false;
    MIMUSB_CHECK(mimusb_battery_update(&node, 80) == MIMUSB_OK &&
                 !mimusb::detail::hid_node(&node)->battery_dirty);
    MIMUSB_CHECK(mimusb_battery_update(&node, 79) == MIMUSB_OK &&
                 mimusb::detail::hid_node(&node)->battery_dirty);

    MIMUSB_CHECK(mimusb_battery_update(&node, -1) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_battery_update(&node, 101) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_battery_update(nullptr, 50) == MIMUSB_ERR_PARAM);

    mimusb::detail::hid_node(&node)->transfer_inflight.store(true);
    MIMUSB_CHECK(mimusb_battery_update(&node, 10) == MIMUSB_ERR_BUSY);
    mimusb::detail::hid_node(&node)->transfer_inflight.store(false);

    mimusb_spec_release(plain);
    mimusb_spec_release(spec);
}

} // namespace

void test_profiles_battery() {
    test_battery_builder();
    test_battery_options();
    test_battery_state();
}
