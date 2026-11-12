// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The factories' refusals, one wrong field at a time: each case starts from options the factory
 * accepts, changes one thing, and expects the documented result and the field that names it,
 * with no Spec handed out. The accepted forms are tested in the test_profiles_* files. */
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimusb.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <array>
#include <cstring>
#include <functional>
#include <vector>

using namespace mimusb_test;

namespace {

template <typename Options>
using Factory = mimusb_result(MIMUSB_CALL*)(const Options*, mimusb_spec**);

template <typename Options> struct Case {
    const char* field; // the field the error names (a prefix is enough)
    mimusb_result expected;
    std::function<void(Options&)> change;
};

// The base must be accepted; every case must then be refused as listed.
template <typename Options>
void expect_refusals(const Options& base, Factory<Options> factory,
                     std::initializer_list<Case<Options>> cases) {
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(factory(&base, &spec) == MIMUSB_OK);
    mimusb_spec_release(spec);
    for (const Case<Options>& c : cases) {
        Options options = base;
        c.change(options);
        spec = nullptr;
        const mimusb_result r = factory(&options, &spec);
        const char* field = mimusb_last_error()->field;
        const bool named =
            field != nullptr && std::strncmp(field, c.field, std::strlen(c.field)) == 0;
        if (r != c.expected || !named) {
            std::fprintf(stderr, "case %s: got %s, field %s\n", c.field, mimusb_result_name(r),
                         field != nullptr ? field : "(null)");
        }
        MIMUSB_CHECK(r == c.expected && named && spec == nullptr);
        mimusb_spec_release(spec);
    }
    // A null output pointer is refused after the options are found good.
    MIMUSB_CHECK(factory(&base, nullptr) == MIMUSB_ERR_PARAM);
    // A struct_size from another release.
    Options other = base;
    other.struct_size += 4U;
    MIMUSB_CHECK(factory(&other, &spec) == MIMUSB_ERR_PARAM);
}

void test_mouse() {
    mimusb_mouse_options base{};
    base.struct_size = static_cast<std::uint32_t>(sizeof(base));
    base.function.bcd_hid = 0x0111;
    base.button_count = 1U;
    base.x = {-127, 127, 8U, {}};
    base.y = {-127, 127, 8U, {}};
    base.enable_wheel = 1U;
    base.wheel = {-31, 31, 8U, {}};
    expect_refusals<mimusb_mouse_options>(
        base, &mimusb_spec_create_mouse,
        {
            {"mouse", MIMUSB_ERR_PARAM, [](auto& o) { o.button_count = 0U; }},
            {"mouse", MIMUSB_ERR_PARAM, [](auto& o) { o.enable_pan = 2U; }},
            {"mouse.wheel", MIMUSB_ERR_PARAM, [](auto& o) { o.wheel.bit_width = 0U; }},
            {"mouse.disabled_fields", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.pan = {-1, 1, 2U, {}};
             }},
            {"mouse.scroll_range", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.wheel = {0, 31, 8U, {}};
             }},
            {"mouse", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.x.physical = {2U, 0, 0, 0, 0};
             }}, // physical.enabled is a boolean
        });
}

void test_toggle() {
    static const std::array<std::uint16_t, 2> usages{0x00CDU, 0x00E9U};
    static const std::array<mimusb_usage_semantic, 2> semantics{MIMUSB_USAGE_ONE_SHOT,
                                                                MIMUSB_USAGE_RETRIGGER};
    static const std::array<const char*, 2> types{"TEST_TYPE", "TEST_TYPE"};
    static const std::array<const char*, 2> codes{"TEST_CODE", "TEST_CODE"};
    static const std::array<std::uint16_t, 2> twice{0x00CDU, 0x00CDU};
    static const std::array<std::uint16_t, 2> application{0x00CDU, 0x0001U};
    static const std::array<std::uint16_t, 2> camera_usages{0x0020U, 0x0022U};
    static const std::array<mimusb_usage_semantic, 2> linear{MIMUSB_USAGE_LINEAR,
                                                             MIMUSB_USAGE_ONE_SHOT};
    static const std::array<mimusb_usage_semantic, 2> undefined{
        static_cast<mimusb_usage_semantic>(999), MIMUSB_USAGE_ONE_SHOT};
    static const std::array<const char*, 2> empty_type{"", "TEST_TYPE"};
    mimusb_toggle_options base{};
    base.struct_size = static_cast<std::uint32_t>(sizeof(base));
    base.function.bcd_hid = 0x0111;
    base.application_page = mimusb::hid::usage::page_consumer;
    base.application_usage = mimusb::hid::usage::consumer_control;
    base.field_page = mimusb::hid::usage::page_consumer;
    base.allowed_usages = usages.data();
    base.allowed_usage_count = usages.size();
    base.usage_semantics = semantics.data();
    base.expected_linux_event_types = types.data();
    base.expected_linux_codes = codes.data();
    expect_refusals<mimusb_toggle_options>(
        base, &mimusb_spec_create_toggle,
        {
            {"toggle.reserved", MIMUSB_ERR_PARAM, [](auto& o) { o.reserved16 = 1U; }},
            {"toggle.application", MIMUSB_ERR_UNSET_FIELD, [](auto& o) { o.field_page = 0U; }},
            {"toggle.allowed_usages", MIMUSB_ERR_UNSET_FIELD,
             [](auto& o) { o.allowed_usage_count = 0U; }},
            {"toggle.usage_semantics", MIMUSB_ERR_UNSET_FIELD,
             [](auto& o) { o.usage_semantics = nullptr; }},
            {"toggle.expected_linux_events", MIMUSB_ERR_UNSET_FIELD,
             [](auto& o) { o.expected_linux_codes = nullptr; }},
            {"toggle.allowed_usage_count", MIMUSB_ERR_OVERFLOW,
             [](auto& o) { o.allowed_usage_count = 70000U; }},
            {"toggle.allowed_usages", MIMUSB_ERR_PARAM,
             [](auto& o) { o.expected_linux_event_types = empty_type.data(); }},
            {"toggle.allowed_usages", MIMUSB_ERR_PARAM,
             [](auto& o) { o.allowed_usages = twice.data(); }},
            {"toggle.allowed_usages", MIMUSB_ERR_PARAM,
             [](auto& o) { o.allowed_usages = application.data(); }},
            {"toggle.usage_semantics", MIMUSB_ERR_UNSUPPORTED,
             [](auto& o) { o.usage_semantics = linear.data(); }},
            {"toggle.usage_semantics", MIMUSB_ERR_PARAM,
             [](auto& o) { o.usage_semantics = undefined.data(); }},
            {"toggle.allowed_usages", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.field_page = mimusb::hid::usage::page_camera_control;
                 o.allowed_usages = camera_usages.data();
             }},
        });
}

void test_gamepad() {
    static std::array<mimusb_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMUSB_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMUSB_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    static std::array<mimusb_gamepad_axis_option, 2> two_x = axes;
    two_x[1].role = MIMUSB_AXIS_X;
    mimusb_gamepad_options base{};
    base.struct_size = static_cast<std::uint32_t>(sizeof(base));
    base.function.bcd_hid = 0x0111;
    base.axes = axes.data();
    base.axis_count = axes.size();
    base.button_count = 1U;
    base.button_usage_minimum = 1U;
    base.dpad_representation = MIMUSB_DPAD_NONE;
    expect_refusals<mimusb_gamepad_options>(
        base, &mimusb_spec_create_gamepad,
        {
            {"gamepad", MIMUSB_ERR_PARAM, [](auto& o) { o.axis_count = 1U; }},
            {"gamepad", MIMUSB_ERR_PARAM, [](auto& o) { o.dpad_representation = 9U; }},
            {"gamepad.axis_count", MIMUSB_ERR_OVERFLOW, [](auto& o) { o.axis_count = 70000U; }},
            {"gamepad.button_usage_minimum", MIMUSB_ERR_OVERFLOW,
             [](auto& o) {
                 o.button_usage_minimum = 0xFFFFU;
                 o.button_count = 2U;
             }},
            {"gamepad.button_count", MIMUSB_ERR_OVERFLOW,
             [](auto& o) {
                 o.dpad_representation = MIMUSB_DPAD_BUTTONS;
                 o.button_count = 65534U;
             }},
            {"gamepad.axes", MIMUSB_ERR_PARAM, [](auto& o) { o.axes = two_x.data(); }},
        });
}

template <typename Options>
void touch_cases(const Options& base, Factory<Options> factory, const char* prefix) {
    const std::string p(prefix);
    static std::string names[8];
    const char* suffixes[8] = {
        "",          ".contact",         ".contact_count",    ".contact_id", ".contacts_per_report",
        ".pressure", ".disabled_fields", ".inactive_geometry"};
    for (int i = 0; i < 8; ++i)
        names[i] = p + suffixes[i];
    expect_refusals<Options>(
        base, factory,
        {
            {names[0].c_str(), MIMUSB_ERR_PARAM, [](auto& o) { o.reserved = 1U; }},
            {names[0].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) { o.enable_multi_packet_frames = 2U; }},
            {names[1].c_str(), MIMUSB_ERR_PARAM, [](auto& o) { o.contact_id.bit_width = 0U; }},
            {names[2].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.contact_count = {1, 3, 2U, {}};
             }},
            {names[3].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.contact_id = {0, 1, 4U, {}};
             }},
            {names[4].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) { o.enable_multi_packet_frames = 0U; }},
            {names[5].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.pressure = {1, 255, 8U, {}};
             }},
            {names[6].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.width = {0, 5, 4U, {}};
             }},
            {names[6].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.enable_scan_time = 0U;
                 o.scan_time = {0, 0, 0U, {}};
             }},
            {names[7].c_str(), MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.enable_width = 1U;
                 o.width = {1, 100, 8U, {}};
             }},
        });
}

void test_touch() {
    touch_cases<mimusb_touchscreen_options>(touchscreen_options(), &mimusb_spec_create_touchscreen,
                                            "touchscreen");
    touch_cases<mimusb_touchpad_options>(touchpad_options(), &mimusb_spec_create_touchpad,
                                         "touchpad");
}

void test_pen() {
    static const std::array<std::uint16_t, 1> barrels{0x44U};
    static const std::array<std::uint16_t, 2> wrong{0x44U, 0x45U};
    static const std::array<std::uint16_t, 2> twice{0x44U, 0x44U};
    mimusb_pen_options base{};
    base.struct_size = static_cast<std::uint32_t>(sizeof(base));
    base.function.bcd_hid = 0x0111;
    base.x = {0, 1000, 16U, {}};
    base.y = {0, 1000, 16U, {}};
    base.enable_pressure = 1U;
    base.pressure = {0, 255, 8U, {}};
    base.barrel_usages = barrels.data();
    base.barrel_usage_count = barrels.size();
    expect_refusals<mimusb_pen_options>(
        base, &mimusb_spec_create_pen,
        {
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.pressure = {0, 0, 8U, {}};
             }},
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.enable_tilt = 1U;
                 o.tilt_x = {-60, 60, 0U, {}};
             }},
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.tilt_x = {-60, 60, 8U, {}};
             }},
            {"pen", MIMUSB_ERR_PARAM, [](auto& o) { o.barrel_usage_count = 0U; }},
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.enable_tilt = 1U;
                 o.tilt_x = {10, 60, 8U, {}};
                 o.tilt_y = {-60, 60, 8U, {}};
             }},
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.barrel_usages = wrong.data();
                 o.barrel_usage_count = wrong.size();
             }},
            {"pen", MIMUSB_ERR_PARAM,
             [](auto& o) {
                 o.barrel_usages = twice.data();
                 o.barrel_usage_count = twice.size();
             }},
        });
}

void test_raw() {
    static const std::array<std::uint8_t, 21> descriptor{0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85,
                                                         0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                                                         0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0xC0};
    static const mimusb_raw_report report{{1U, 1U, {}}, 2U};
    static const mimusb_raw_report wrong_length{{1U, 1U, {}}, 5U};
    const mimusb_raw_options base{
        sizeof(mimusb_raw_options), 0U, descriptor.data(), descriptor.size(), &report, 1U, 0U, 0U,
        {0x0111U, 0U, 0U, 0U, {}}};
    expect_refusals<mimusb_raw_options>(
        base, &mimusb_spec_create_raw,
        {
            {"raw.options", MIMUSB_ERR_PARAM, [](auto& o) { o.requires_output = 2U; }},
            {"raw.reports", MIMUSB_ERR_UNSET_FIELD, [](auto& o) { o.report_count = 0U; }},
            {"raw.reports", MIMUSB_ERR_OVERFLOW, [](auto& o) { o.report_count = 257U; }},
            {"raw", MIMUSB_ERR_PARAM, [](auto& o) { o.reports = &wrong_length; }},
        });
}

// Raw descriptors the parser refuses, each naming the item at fault.
void test_raw_descriptors() {
    const mimusb_raw_report report{{1U, 1U, {}}, 2U};
    const mimusb_raw_report two[2] = {{{1U, 1U, {}}, 2U}, {{1U, 2U, {}}, 2U}};
    const struct {
        std::vector<std::uint8_t> descriptor;
        const mimusb_raw_report* reports;
        std::size_t count;
        const char* field;
    } cases[] = {
        // A Usage Page above 16 bits.
        {{0x07, 0x00, 0x00, 0x01, 0x00, 0x09, 0x06, 0xA1, 0x01, 0xC0},
         &report,
         1,
         "raw.usage_page"},
        // A Pop with no Push.
        {{0xB4, 0x05, 0x01}, &report, 1, "raw.global_stack"},
        // Report ID zero.
        {{0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x00, 0xC0}, &report, 1, "raw.report_id"},
        // Input outside any collection.
        {{0x75, 0x08, 0x95, 0x01, 0x81, 0x02}, &report, 1, "raw.input"},
        // Input without a Report Size.
        {{0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x95, 0x01, 0x81, 0x02, 0xC0},
         &report,
         1,
         "raw.input"},
        // Report 1 in two top-level collections.
        {{0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02,
          0xC0, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02, 0xC0},
         two,
         1,
         "raw.input"},
    };
    for (const auto& c : cases) {
        const mimusb::hid::ValidationIssue issue =
            mimusb::hid::validate_raw(c.descriptor.data(), c.descriptor.size(), c.reports, c.count);
        const bool named = issue.field != nullptr && std::strcmp(issue.field, c.field) == 0;
        if (issue.result == MIMUSB_OK || !named)
            std::fprintf(stderr, "raw case %s: got %s at %s\n", c.field,
                         mimusb_result_name(issue.result), issue.field ? issue.field : "(null)");
        MIMUSB_CHECK(issue.result != MIMUSB_OK && named);
    }
}

} // namespace

void test_spec_rejections() {
    test_raw_descriptors();
    test_mouse();
    test_toggle();
    test_gamepad();
    test_touch();
    test_pen();
    test_raw();
}
