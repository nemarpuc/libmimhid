// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Touchscreen and touchpad profiles: units and the contact-count feature, azimuth, packets of
 * contacts, lift frames, idle suppression and the button-only touchpad. */
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

void test_touch_units_feature_and_manifest_status() {
    mimusb_touchscreen_options options = touchscreen_options();
    options.enable_contact_count_maximum_feature_declaration = 1U;
    mimusb_spec* spec = nullptr;
    check_battery_variant(options, &mimusb_spec_create_touchscreen);
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        MIMUSB_CHECK(count_item(mimusb::detail::hid_spec(spec)->descriptor, 0xB1U, 0x03U) == 1U);
        mimusb_spec_release(spec);
    }

    options = touchscreen_options();
    options.x.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_width = 1U;
    options.width = {0, 100, 8U, {1U, 0, 10, -2, 0x11U}};
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    mimusb_spec_release(spec);
    options.width.physical.unit_exponent = -3;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_ERR_PARAM);

    options = touchscreen_options();
    options.y.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_height = 1U;
    options.height = {0, 100, 8U, {1U, 0, 10, -2, 0x12U}};
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_ERR_PARAM);

    options = touchscreen_options();
    options.scan_time.physical = {1U, 0, 65535, -4, 0x1001U};
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_ERR_PARAM);
}

void test_touch_azimuth() {
    mimusb_touchscreen_options options = touchscreen_options();
    options.enable_azimuth = 1U;
    options.azimuth = {0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;

    const auto* azimuth = field(spec, mimusb::hid::FieldSemantic::azimuth, 0U);
    MIMUSB_CHECK(azimuth != nullptr && azimuth->logical_minimum == 0 &&
                 azimuth->logical_maximum == 36000);
    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::hid_node(&node)->state = mimusb::detail::TouchState{};
    const mimusb::detail::TouchContact contact{1U, 100, 200, 10, 0, 0, 35999};
    mimusb_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                   contact.azimuth};
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == MIMUSB_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(azimuth != nullptr && extract(report.data(), *azimuth) == 35999U);
    mimusb::detail::hid_node(&node)->dirty = false;
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == MIMUSB_OK);
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->dirty);
    mimusb_touchscreen_extra invalid_extra = extra;
    invalid_extra.azimuth = 36000;
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &invalid_extra) == MIMUSB_ERR_PARAM);
    mimusb_spec_release(spec);

    options.azimuth.logical_minimum = 1;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_ERR_PARAM);
    options.enable_azimuth = 0U;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_ERR_PARAM);
    options.azimuth = {};
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_node disabled_node{};
        mimusb::detail::attach_hid(&disabled_node, spec);
        mimusb::detail::hid_node(&disabled_node)->state = mimusb::detail::TouchState{};
        mimusb_touchscreen_extra disabled_extra{10, 0, 0, 1};
        MIMUSB_CHECK(mimusb_touchscreen_contact(&disabled_node, 1U, 1U, 100, 200,
                                                &disabled_extra) == MIMUSB_ERR_PARAM);
    }
    mimusb_spec_release(spec);
}

void test_touch_packets() {
    mimusb_touchscreen_options options = touchscreen_options();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::hid_node(&node)->state = mimusb::detail::TouchState{};
    for (std::uint32_t id = 0U; id < 3U; ++id) {
        const mimusb::detail::TouchContact contact{id,
                                                   static_cast<std::int32_t>(100U + id),
                                                   static_cast<std::int32_t>(200U + id),
                                                   id == 0U ? 0 : 10,
                                                   0,
                                                   0,
                                                   0};
        mimusb_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                       contact.azimuth};
        MIMUSB_CHECK(mimusb_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                                &extra) == MIMUSB_OK);
    }
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    const auto* count = field(spec, mimusb::hid::FieldSemantic::contact_count);
    const auto* pressure = field(spec, mimusb::hid::FieldSemantic::pressure, 0U);
    const auto* scan_time = field(spec, mimusb::hid::FieldSemantic::scan_time);
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 3U);
    MIMUSB_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    MIMUSB_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimusb::detail::hid_node(&node)->transfer_inflight.store(true);
    mimusb::detail::transfer_complete(mimusb::detail::hid_node(&node), MIMUSB_OK, 0);
    const mimusb::detail::TouchContact changed{0U, 300, 400, 10, 0, 0, 0};
    mimusb_touchscreen_extra changed_extra{changed.pressure, changed.width, changed.height,
                                           changed.azimuth};
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, changed.contact_id, 1U, changed.x, changed.y,
                                            &changed_extra) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMUSB_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimusb_spec_release(spec);
}

void test_touch_lift_frame_and_idle_suppression() {
    mimusb_touchscreen_options options = touchscreen_options();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;

    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::hid_node(&node)->state = mimusb::detail::TouchState{};
    const mimusb::detail::TouchContact contact{7U, 100, 200, 0, 0, 0, 0};
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            nullptr) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMUSB_ERR_BUSY);

    const auto* count = field(spec, mimusb::hid::FieldSemantic::contact_count);
    const auto* tip = field(spec, mimusb::hid::FieldSemantic::tip);
    const auto* identifier = field(spec, mimusb::hid::FieldSemantic::contact_id);
    const auto* x = field(spec, mimusb::hid::FieldSemantic::x);
    const auto* y = field(spec, mimusb::hid::FieldSemantic::y);
    const auto* pressure = field(spec, mimusb::hid::FieldSemantic::pressure);
    const auto* scan_time = field(spec, mimusb::hid::FieldSemantic::scan_time);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    MIMUSB_CHECK(tip != nullptr && extract(report.data(), *tip) == 1U);
    MIMUSB_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    MIMUSB_CHECK(x != nullptr && extract(report.data(), *x) == 100U);
    MIMUSB_CHECK(y != nullptr && extract(report.data(), *y) == 200U);
    MIMUSB_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    MIMUSB_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimusb::detail::transfer_complete(mimusb::detail::hid_node(&node), MIMUSB_OK, 0);
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->dirty);

    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMUSB_OK);
    report.fill(0U);
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    // The explicit Up report keeps the same slot and final coordinates while
    // clearing Tip; it is still counted until its successful completion.
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    MIMUSB_CHECK(tip != nullptr && extract(report.data(), *tip) == 0U);
    MIMUSB_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    MIMUSB_CHECK(x != nullptr && extract(report.data(), *x) == 300U);
    MIMUSB_CHECK(y != nullptr && extract(report.data(), *y) == 400U);
    MIMUSB_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 0U);
    mimusb::detail::transfer_complete(mimusb::detail::hid_node(&node), MIMUSB_OK, 0);

    const auto* state_value =
        std::get_if<mimusb::detail::TouchState>(&mimusb::detail::hid_node(&node)->state);
    MIMUSB_CHECK(state_value != nullptr);
    if (state_value != nullptr) {
        MIMUSB_CHECK(std::all_of(state_value->contacts.begin(), state_value->contacts.end(),
                                 [](const mimusb::detail::ContactState& entry) {
                                     return entry.phase == mimusb::detail::ContactPhase::none;
                                 }));
    }
    // Idle touch state is clean, so the public submit path suppresses a
    // zero-count report instead of sending one and reusing the previous count.
    MIMUSB_CHECK(!mimusb::detail::hid_node(&node)->dirty);
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == MIMUSB_ERR_PARAM);
    const mimusb::detail::TouchContact next_contact{8U, 500, 600, 10, 0, 0, 0};
    mimusb_touchscreen_extra next_extra{next_contact.pressure, next_contact.width,
                                        next_contact.height, next_contact.azimuth};
    MIMUSB_CHECK(mimusb_touchscreen_contact(&node, next_contact.contact_id, 1U, next_contact.x,
                                            next_contact.y, &next_extra) == MIMUSB_OK);
    report.fill(0U);
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    mimusb_spec_release(spec);
}

void test_touchpad_button_only_and_mid_frame_guard() {
    mimusb_touchpad_options options = touchpad_options();
    options.button_count = 1U;
    mimusb_spec* spec = nullptr;
    check_battery_variant(options, &mimusb_spec_create_touchpad);
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;

    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::TouchState state_value{};
    state_value.buttons.resize(1U);
    state_value.button_transitions.resize(1U);
    mimusb::detail::hid_node(&node)->state = std::move(state_value);

    MIMUSB_CHECK(mimusb_touchpad_button(&node, 1U, 1U) == MIMUSB_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    const auto* count = field(spec, mimusb::hid::FieldSemantic::contact_count);
    const auto* button = field(spec, mimusb::hid::FieldSemantic::buttons);
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMUSB_CHECK(button != nullptr && extract(report.data(), *button) == 1U);
    MIMUSB_CHECK(mimusb_touchpad_button(&node, 1U, 0U) == MIMUSB_ERR_BUSY);
    mimusb::detail::transfer_complete(mimusb::detail::hid_node(&node), MIMUSB_OK, 0);

    auto* touch = std::get_if<mimusb::detail::TouchState>(&mimusb::detail::hid_node(&node)->state);
    MIMUSB_CHECK(touch != nullptr);
    if (touch != nullptr) {
        touch->packet_cursor = 1U;
        MIMUSB_CHECK(mimusb_touchpad_button(&node, 1U, 0U) == MIMUSB_ERR_BUSY);
        MIMUSB_CHECK(touch->buttons[0] == 1U);
        touch->packet_cursor = 0U;
    }

    MIMUSB_CHECK(mimusb_touchpad_button(&node, 1U, 0U) == MIMUSB_OK);
    report.fill(0U);
    MIMUSB_CHECK(mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                                report.size(), &length) == MIMUSB_OK);
    MIMUSB_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    MIMUSB_CHECK(button != nullptr && extract(report.data(), *button) == 0U);
    mimusb_spec_release(spec);
}

} // namespace

void test_profiles_touch() {
    mimusb_spec* spec = nullptr;
    mimusb_touchscreen_options invalid_touch = touchscreen_options();
    invalid_touch.contact_id = {1, 15, 4U, {}};
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&invalid_touch, &spec) == MIMUSB_ERR_PARAM);

    mimusb_touchpad_options invalid_touchpad = touchpad_options();
    invalid_touchpad.button_count = 65536U;
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&invalid_touchpad, &spec) == MIMUSB_ERR_OVERFLOW);

    mimusb_touchpad_options buttonless = touchpad_options();
    buttonless.button_count = 0U;
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&buttonless, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMUSB_CHECK(mimusb_spec_manifest(spec, &manifest) == MIMUSB_OK);
        MIMUSB_CHECK(manifest.profile_kind == MIMUSB_PROFILE_TOUCHPAD);
    }
    mimusb_spec_release(spec);

    test_touch_units_feature_and_manifest_status();
    test_touch_azimuth();
    test_touch_packets();
    test_touch_lift_frame_and_idle_suppression();
    test_touchpad_button_only_and_mid_frame_guard();
}
