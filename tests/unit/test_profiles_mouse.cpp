// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Mouse profile: relative deltas split over reports and consumed, physical properties, and
 * field packing of wide axes. */
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

void test_mouse_delta_splitting_and_consumption() {
    mimhid_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.button_count = 1U;
    options.x = {-127, 127, 8U, {}};
    options.y = {-127, 127, 8U, {}};
    options.enable_wheel = 1U;
    options.wheel = {-31, 31, 8U, {}};
    options.enable_pan = 1U;
    options.pan = {-15, 15, 8U, {}};
    mimhid_spec* spec = nullptr;
    check_battery_variant(options, &mimhid_spec_create_mouse);
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::MouseState state_value{};
    state_value.buttons.resize(1U);
    state_value.button_transitions.resize(1U);
    node.state = std::move(state_value);
    MIMHID_CHECK(mimhid_mouse_move(&node, 300, -300) == MIMHID_OK);
    MIMHID_CHECK(mimhid_mouse_scroll(&node, 95, -34) == MIMHID_OK);

    const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
    const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
    const auto* wheel = field(spec, mimhid::hid::FieldSemantic::wheel);
    const auto* pan = field(spec, mimhid::hid::FieldSemantic::pan);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    const std::array<std::int32_t, 4U> expected_x{127, 127, 46, 0};
    const std::array<std::int32_t, 4U> expected_y{-127, -127, -46, 0};
    const std::array<std::int32_t, 4U> expected_wheel{31, 31, 31, 2};
    const std::array<std::int32_t, 4U> expected_pan{-15, -15, -4, 0};
    for (std::size_t index = 0U; index < expected_x.size(); ++index) {
        report.fill(0U);
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                     MIMHID_OK);
        MIMHID_CHECK(x != nullptr &&
                     extract(report.data(), *x) == static_cast<std::uint8_t>(expected_x[index]));
        MIMHID_CHECK(y != nullptr &&
                     extract(report.data(), *y) == static_cast<std::uint8_t>(expected_y[index]));
        MIMHID_CHECK(wheel != nullptr && extract(report.data(), *wheel) ==
                                             static_cast<std::uint8_t>(expected_wheel[index]));
        MIMHID_CHECK(pan != nullptr && extract(report.data(), *pan) ==
                                           static_cast<std::uint8_t>(expected_pan[index]));
        mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
        MIMHID_CHECK(node.dirty == (index + 1U < expected_x.size()));
    }

    report.fill(0xFFU);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(x != nullptr && extract(report.data(), *x) == 0U);
    MIMHID_CHECK(y != nullptr && extract(report.data(), *y) == 0U);
    MIMHID_CHECK(wheel != nullptr && extract(report.data(), *wheel) == 0U);
    MIMHID_CHECK(pan != nullptr && extract(report.data(), *pan) == 0U);
    MIMHID_CHECK(!node.dirty);
    MIMHID_CHECK(mimhid_mouse_button(&node, 1U, 1U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_mouse_button(&node, 1U, 0U) == MIMHID_ERR_BUSY);
    mimhid::detail::transfer_complete(&node, MIMHID_ERR_IO, 1);
    MIMHID_CHECK(mimhid_mouse_button(&node, 1U, 0U) == MIMHID_OK);
    mimhid_spec_release(spec);
}

void test_physical_properties_and_reset() {
    mimhid_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.button_count = 1U;
    options.x = {-127, 127, 8U, {1U, -10, 10, -2, 0x11U}};
    options.y = {-127, 127, 8U, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
        const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
        MIMHID_CHECK(x != nullptr && x->physical.enabled == 1U && x->physical.minimum == -10 &&
                     x->physical.maximum == 10 && x->physical.unit_exponent == -2 &&
                     x->physical.unit == 0x11U);
        MIMHID_CHECK(y != nullptr && y->physical.enabled == 0U && y->physical.minimum == 0 &&
                     y->physical.maximum == 0 && y->physical.unit_exponent == 0 &&
                     y->physical.unit == 0U);
        MIMHID_CHECK(has_sequence(spec->descriptor,
                                  {0x35U, 0xF6U, 0x45U, 0x0AU, 0x55U, 0x0EU, 0x65U, 0x11U}));
        MIMHID_CHECK(has_sequence(spec->descriptor, {0x81U, 0x06U, 0x35U, 0x00U, 0x45U, 0x00U,
                                                     0x55U, 0x00U, 0x65U, 0x00U}));
    }
    mimhid_spec_release(spec);

    options.x.physical.maximum = -10;
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, 8, 0x11U};
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, -2, 0U};
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, -2, 0x05U};
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_ERR_PARAM);
    options.x.physical = {0U, 0, 1, 0, 0U};
    MIMHID_CHECK(mimhid_spec_create_mouse(&options, &spec) == MIMHID_ERR_PARAM);
}

void test_field_span_padding() {
    // X ends mid-byte, so a 32-bit Y right behind it would touch five bytes.
    mimhid_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 1U;
    mouse.x = {-1073741823, 1073741823, 31U, {}};
    mouse.y = {-2147483647, 2147483647, 32U, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_mouse(&mouse, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
        const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
        MIMHID_CHECK(x != nullptr && x->bit_offset == 8U);
        MIMHID_CHECK(y != nullptr && y->bit_offset == 40U);
    }
    mimhid_spec_release(spec);
    spec = nullptr;

    // Fields that already fit keep their packed positions.
    mouse.x = {-127, 127, 8U, {}};
    mouse.y = {-2047, 2047, 12U, {}};
    MIMHID_CHECK(mimhid_spec_create_mouse(&mouse, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
        MIMHID_CHECK(y != nullptr && y->bit_offset == 16U);
    }
    mimhid_spec_release(spec);
    spec = nullptr;

    // A 4-bit Contact Identifier followed by a 31-bit X.
    mimhid_touchscreen_options touch{};
    touch.struct_size = static_cast<std::uint32_t>(sizeof(touch));
    touch.maximum_contacts = 1U;
    touch.contacts_per_report = 1U;
    touch.contact_id = {0, 15, 4U, {}};
    touch.x = {0, 2147483646, 31U, {}};
    touch.y = {0, 2559, 12U, {}};
    touch.contact_count = {0, 1, 1U, {}};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&touch, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
        MIMHID_CHECK(x != nullptr && x->bit_offset % 8U == 0U);
    }
    mimhid_spec_release(spec);
}

} // namespace

void test_profiles_mouse() {
    test_mouse_delta_splitting_and_consumption();
    test_physical_properties_and_reset();
    test_field_span_padding();
}
