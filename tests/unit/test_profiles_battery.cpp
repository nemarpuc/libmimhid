// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The battery report every profile can add: the builder, the options and the state. */
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

void test_battery_builder() {
    using mimhid::hid::BatteryField;
    using mimhid::hid::DescriptorBuilder;
    using mimhid::hid::FieldSemantic;
    using mimhid::hid::ReportLayout;

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
    MIMHID_CHECK(emit(counting));
    counting.finish();
    MIMHID_CHECK(counting.good());
    std::vector<std::uint8_t> bytes(counting.descriptor_size());
    DescriptorBuilder emitting(bytes.data(), bytes.size(), nullptr);
    emitting.set_battery(battery, nullptr);
    MIMHID_CHECK(emit(emitting));
    emitting.finish();
    MIMHID_CHECK(emitting.good() && emitting.descriptor_size() == bytes.size());

    // Input layout is untouched by the battery report.
    MIMHID_CHECK(input.has_report_id && input.report_id == 1U && input.wire_bytes == 2U);
    MIMHID_CHECK(battery_layout.has_report_id && battery_layout.report_id == 2U &&
                 battery_layout.wire_bytes == 2U && battery_layout.fields.size() == 1U &&
                 battery_layout.fields[0].semantic == FieldSemantic::battery_strength &&
                 battery_layout.fields[0].bit_offset == 8U &&
                 battery_layout.fields[0].logical_minimum == 1 &&
                 battery_layout.fields[0].logical_maximum == 100);

    const std::vector<std::uint8_t> tail{0x85U, 0x02U, 0x05U, 0x06U, 0x09U, 0x20U,
                                         0x15U, 0x01U, 0x25U, 0x64U, 0x75U, 0x08U,
                                         0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    MIMHID_CHECK(bytes.size() > tail.size() &&
                 std::equal(tail.begin(), tail.end(),
                            bytes.end() - static_cast<std::ptrdiff_t>(tail.size())));

    MIMHID_CHECK(mimhid::hid::validate_generated(bytes.data(), bytes.size(), input, &battery_layout)
                     .result == MIMHID_OK);
    // The same bytes checked against a wrong battery layout are refused.
    ReportLayout wrong = battery_layout;
    wrong.report_id = 3U;
    MIMHID_CHECK(
        mimhid::hid::validate_generated(bytes.data(), bytes.size(), input, &wrong).result ==
        MIMHID_ERR_INTERNAL);
    // Without a battery layout a second Report ID is still refused.
    MIMHID_CHECK(mimhid::hid::validate_generated(bytes.data(), bytes.size(), input).result ==
                 MIMHID_ERR_INTERNAL);

    // A 7-bit field is padded to a whole byte.
    const BatteryField narrow{2U, 0, 100, 7U, {}};
    ReportLayout narrow_input;
    ReportLayout narrow_layout;
    DescriptorBuilder narrow_builder(nullptr, 65535U, &narrow_input);
    narrow_builder.set_battery(narrow, &narrow_layout);
    MIMHID_CHECK(emit(narrow_builder));
    narrow_builder.finish();
    MIMHID_CHECK(narrow_builder.good() && narrow_layout.wire_bytes == 2U &&
                 narrow_layout.fields.size() == 2U && !narrow_layout.fields[0].has_null_state &&
                 narrow_layout.fields[1].semantic == FieldSemantic::constant_padding);
}

void test_battery_options() {
    mimhid_keyboard_options keyboard = keyboard_options();
    check_battery_variant(keyboard, &mimhid_spec_create_keyboard);

    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    const auto rejects = [&](const char* field) {
        mimhid_spec* refused = nullptr;
        const bool param = mimhid_spec_create_keyboard(&keyboard, &refused) == MIMHID_ERR_PARAM;
        const std::string_view named = mimhid_last_error()->field;
        mimhid_spec_release(refused);
        return param && named == field;
    };
    const mimhid_keyboard_options good = keyboard;

    keyboard.enable_battery = 2U;
    MIMHID_CHECK(rejects("keyboard.enable_battery"));
    keyboard = good;
    keyboard.enable_battery = 0U; // battery left filled in
    MIMHID_CHECK(rejects("keyboard.battery"));
    keyboard = good;
    keyboard.report_id = {0U, 0U, {0U, 0U, 0U}}; // input report has no ID
    MIMHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id = {0U, 0U, {0U, 0U, 0U}};
    MIMHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id.value = 1U; // same as the input report
    MIMHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.strength = {100, 1, 8U, {}};
    MIMHID_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {-1, 100, 8U, {}};
    MIMHID_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {1, 300, 8U, {}}; // does not fit the width
    MIMHID_CHECK(rejects("keyboard.battery.strength"));
}

void test_battery_state() {
    mimhid_keyboard_options keyboard = keyboard_options();
    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    mimhid_spec* plain = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&keyboard, &plain) == MIMHID_OK);
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {0, 100, 8U, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&keyboard, &spec) == MIMHID_OK);
    if (plain == nullptr || spec == nullptr) {
        mimhid_spec_release(plain);
        mimhid_spec_release(spec);
        return;
    }

    mimhid_node without{};
    without.spec = plain;
    without.state = mimhid::detail::KeyboardState{};
    MIMHID_CHECK(mimhid_battery_update(&without, 50) == MIMHID_ERR_PARAM);

    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::KeyboardState{};
    std::array<std::uint8_t, 8U> report{};
    std::size_t length = 0U;

    // Nothing was ever set, so there is nothing to send.
    MIMHID_CHECK(!node.battery_dirty);
    // The first value is a change even when it is 0, the initial storage.
    MIMHID_CHECK(mimhid_battery_update(&node, 0) == MIMHID_OK && node.battery_dirty);
    MIMHID_CHECK(mimhid::detail::serialize_battery(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(length == 2U && report[0] == 2U && report[1] == 0U);

    MIMHID_CHECK(mimhid_battery_update(&node, 80) == MIMHID_OK);
    MIMHID_CHECK(node.battery_dirty && !node.dirty);
    MIMHID_CHECK(mimhid::detail::serialize_battery(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(length == 2U && report[0] == 2U && report[1] == 80U);
    MIMHID_CHECK(mimhid::detail::validate_serialized_report(spec, spec->battery_layout,
                                                            report.data(), length) == MIMHID_OK);
    report[1] = 101U;
    MIMHID_CHECK(mimhid::detail::validate_serialized_report(
                     spec, spec->battery_layout, report.data(), length) == MIMHID_ERR_INTERNAL);

    // The same value again asks for nothing.
    node.battery_dirty = false;
    MIMHID_CHECK(mimhid_battery_update(&node, 80) == MIMHID_OK && !node.battery_dirty);
    MIMHID_CHECK(mimhid_battery_update(&node, 79) == MIMHID_OK && node.battery_dirty);

    MIMHID_CHECK(mimhid_battery_update(&node, -1) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_battery_update(&node, 101) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_battery_update(nullptr, 50) == MIMHID_ERR_PARAM);

    node.transfer_inflight.store(true);
    MIMHID_CHECK(mimhid_battery_update(&node, 10) == MIMHID_ERR_BUSY);
    node.transfer_inflight.store(false);

    mimhid_spec_release(plain);
    mimhid_spec_release(spec);
}

} // namespace

void test_profiles_battery() {
    test_battery_builder();
    test_battery_options();
    test_battery_state();
}
