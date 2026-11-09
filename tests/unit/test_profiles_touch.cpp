// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Touchscreen and touchpad profiles: units and the contact-count feature, azimuth, packets of
 * contacts, lift frames, idle suppression and the button-only touchpad. */
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

void test_touch_units_feature_and_manifest_status() {
    mimhid_touchscreen_options options = touchscreen_options();
    options.enable_contact_count_maximum_feature_declaration = 1U;
    mimhid_spec* spec = nullptr;
    check_battery_variant(options, &mimhid_spec_create_touchscreen);
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        MIMHID_CHECK(count_item(spec->descriptor, 0xB1U, 0x03U) == 1U);
        mimhid_spec_release(spec);
    }

    options = touchscreen_options();
    options.x.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_width = 1U;
    options.width = {0, 100, 8U, {1U, 0, 10, -2, 0x11U}};
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    mimhid_spec_release(spec);
    options.width.physical.unit_exponent = -3;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_ERR_PARAM);

    options = touchscreen_options();
    options.y.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_height = 1U;
    options.height = {0, 100, 8U, {1U, 0, 10, -2, 0x12U}};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_ERR_PARAM);

    options = touchscreen_options();
    options.scan_time.physical = {1U, 0, 65535, -4, 0x1001U};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_ERR_PARAM);
}

void test_touch_azimuth() {
    mimhid_touchscreen_options options = touchscreen_options();
    options.enable_azimuth = 1U;
    options.azimuth = {0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    const auto* azimuth = field(spec, mimhid::hid::FieldSemantic::azimuth, 0U);
    MIMHID_CHECK(azimuth != nullptr && azimuth->logical_minimum == 0 &&
                 azimuth->logical_maximum == 36000);
    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::TouchState{};
    const mimhid::detail::TouchContact contact{1U, 100, 200, 10, 0, 0, 35999};
    mimhid_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                   contact.azimuth};
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == MIMHID_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(azimuth != nullptr && extract(report.data(), *azimuth) == 35999U);
    node.dirty = false;
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);
    mimhid_touchscreen_extra invalid_extra = extra;
    invalid_extra.azimuth = 36000;
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &invalid_extra) == MIMHID_ERR_PARAM);
    mimhid_spec_release(spec);

    options.azimuth.logical_minimum = 1;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_ERR_PARAM);
    options.enable_azimuth = 0U;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_ERR_PARAM);
    options.azimuth = {};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_node disabled_node{};
        disabled_node.spec = spec;
        disabled_node.state = mimhid::detail::TouchState{};
        mimhid_touchscreen_extra disabled_extra{10, 0, 0, 1};
        MIMHID_CHECK(mimhid_touchscreen_contact(&disabled_node, 1U, 1U, 100, 200,
                                                &disabled_extra) == MIMHID_ERR_PARAM);
    }
    mimhid_spec_release(spec);
}

void test_touch_packets() {
    mimhid_touchscreen_options options = touchscreen_options();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::TouchState{};
    for (std::uint32_t id = 0U; id < 3U; ++id) {
        const mimhid::detail::TouchContact contact{id,
                                                   static_cast<std::int32_t>(100U + id),
                                                   static_cast<std::int32_t>(200U + id),
                                                   id == 0U ? 0 : 10,
                                                   0,
                                                   0,
                                                   0};
        mimhid_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                       contact.azimuth};
        MIMHID_CHECK(mimhid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                                &extra) == MIMHID_OK);
    }
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* count = field(spec, mimhid::hid::FieldSemantic::contact_count);
    const auto* pressure = field(spec, mimhid::hid::FieldSemantic::pressure, 0U);
    const auto* scan_time = field(spec, mimhid::hid::FieldSemantic::scan_time);
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 3U);
    MIMHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    MIMHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    const mimhid::detail::TouchContact changed{0U, 300, 400, 10, 0, 0, 0};
    mimhid_touchscreen_extra changed_extra{changed.pressure, changed.width, changed.height,
                                           changed.azimuth};
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, changed.contact_id, 1U, changed.x, changed.y,
                                            &changed_extra) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimhid_spec_release(spec);
}

void test_touch_lift_frame_and_idle_suppression() {
    mimhid_touchscreen_options options = touchscreen_options();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::TouchState{};
    const mimhid::detail::TouchContact contact{7U, 100, 200, 0, 0, 0, 0};
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMHID_ERR_BUSY);

    const auto* count = field(spec, mimhid::hid::FieldSemantic::contact_count);
    const auto* tip = field(spec, mimhid::hid::FieldSemantic::tip);
    const auto* identifier = field(spec, mimhid::hid::FieldSemantic::contact_id);
    const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
    const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
    const auto* pressure = field(spec, mimhid::hid::FieldSemantic::pressure);
    const auto* scan_time = field(spec, mimhid::hid::FieldSemantic::scan_time);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    MIMHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 1U);
    MIMHID_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    MIMHID_CHECK(x != nullptr && extract(report.data(), *x) == 100U);
    MIMHID_CHECK(y != nullptr && extract(report.data(), *y) == 200U);
    MIMHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    MIMHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(!node.dirty);

    MIMHID_CHECK(mimhid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    // The explicit Up report keeps the same slot and final coordinates while
    // clearing Tip; it is still counted until its successful completion.
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    MIMHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 0U);
    MIMHID_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    MIMHID_CHECK(x != nullptr && extract(report.data(), *x) == 300U);
    MIMHID_CHECK(y != nullptr && extract(report.data(), *y) == 400U);
    MIMHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 0U);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    const auto* state_value = std::get_if<mimhid::detail::TouchState>(&node.state);
    MIMHID_CHECK(state_value != nullptr);
    if (state_value != nullptr) {
        MIMHID_CHECK(std::all_of(state_value->contacts.begin(), state_value->contacts.end(),
                                 [](const mimhid::detail::ContactState& entry) {
                                     return entry.phase == mimhid::detail::ContactPhase::none;
                                 }));
    }
    // Idle touch state is clean, so the public submit path suppresses a
    // zero-count report instead of sending one and reusing the previous count.
    MIMHID_CHECK(!node.dirty);
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMHID_ERR_PARAM);
    const mimhid::detail::TouchContact next_contact{8U, 500, 600, 10, 0, 0, 0};
    mimhid_touchscreen_extra next_extra{next_contact.pressure, next_contact.width,
                                        next_contact.height, next_contact.azimuth};
    MIMHID_CHECK(mimhid_touchscreen_contact(&node, next_contact.contact_id, 1U, next_contact.x,
                                            next_contact.y, &next_extra) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimhid_spec_release(spec);
}

void test_touchpad_button_only_and_mid_frame_guard() {
    mimhid_touchpad_options options = touchpad_options();
    options.button_count = 1U;
    mimhid_spec* spec = nullptr;
    check_battery_variant(options, &mimhid_spec_create_touchpad);
    MIMHID_CHECK(mimhid_spec_create_touchpad(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::TouchState state_value{};
    state_value.buttons.resize(1U);
    state_value.button_transitions.resize(1U);
    node.state = std::move(state_value);

    MIMHID_CHECK(mimhid_touchpad_button(&node, 1U, 1U) == MIMHID_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* count = field(spec, mimhid::hid::FieldSemantic::contact_count);
    const auto* button = field(spec, mimhid::hid::FieldSemantic::buttons);
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMHID_CHECK(button != nullptr && extract(report.data(), *button) == 1U);
    MIMHID_CHECK(mimhid_touchpad_button(&node, 1U, 0U) == MIMHID_ERR_BUSY);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    auto* touch = std::get_if<mimhid::detail::TouchState>(&node.state);
    MIMHID_CHECK(touch != nullptr);
    if (touch != nullptr) {
        touch->packet_cursor = 1U;
        MIMHID_CHECK(mimhid_touchpad_button(&node, 1U, 0U) == MIMHID_ERR_BUSY);
        MIMHID_CHECK(touch->buttons[0] == 1U);
        touch->packet_cursor = 0U;
    }

    MIMHID_CHECK(mimhid_touchpad_button(&node, 1U, 0U) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMHID_CHECK(button != nullptr && extract(report.data(), *button) == 0U);
    mimhid_spec_release(spec);
}

} // namespace

void test_profiles_touch() {
    mimhid_spec* spec = nullptr;
    mimhid_touchscreen_options invalid_touch = touchscreen_options();
    invalid_touch.contact_id = {1, 15, 4U, {}};
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&invalid_touch, &spec) == MIMHID_ERR_PARAM);

    mimhid_touchpad_options invalid_touchpad = touchpad_options();
    invalid_touchpad.button_count = 65536U;
    MIMHID_CHECK(mimhid_spec_create_touchpad(&invalid_touchpad, &spec) == MIMHID_ERR_OVERFLOW);

    mimhid_touchpad_options buttonless = touchpad_options();
    buttonless.button_count = 0U;
    MIMHID_CHECK(mimhid_spec_create_touchpad(&buttonless, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        MIMHID_CHECK(manifest.profile_kind == MIMHID_PROFILE_TOUCHPAD);
    }
    mimhid_spec_release(spec);

    test_touch_units_feature_and_manifest_status();
    test_touch_azimuth();
    test_touch_packets();
    test_touch_lift_frame_and_idle_suppression();
    test_touchpad_button_only_and_mid_frame_guard();
}
