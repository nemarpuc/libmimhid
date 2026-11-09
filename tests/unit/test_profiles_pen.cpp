// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Pen profile: coordinates away from the surface and the tool switch, checked against the way
 * the Linux hid-input driver reads Invert and In Range. */
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

struct PinnedLinuxPenTools {
    bool invert{};
    bool pen{};
    bool rubber{};
};

void apply_pinned_linux_pen_report(const mimhid_spec* spec, const std::uint8_t* report,
                                   PinnedLinuxPenTools* tools) {
    std::vector<const mimhid::hid::FieldLayout*> ordered;
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == mimhid::hid::FieldSemantic::eraser ||
            candidate.semantic == mimhid::hid::FieldSemantic::in_range) {
            ordered.push_back(&candidate);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->bit_offset < right->bit_offset;
    });
    for (const auto* candidate : ordered) {
        const bool asserted = extract(report, *candidate) != 0U;
        if (candidate->semantic == mimhid::hid::FieldSemantic::eraser) {
            tools->invert = asserted;
        } else if (!asserted) {
            tools->pen = false;
            tools->rubber = false;
        } else if (tools->invert) {
            tools->rubber = true;
        } else {
            tools->pen = true;
        }
    }
}

void test_pen_away_coordinates() {
    const std::array<std::uint16_t, 1> barrels{0x44U};
    mimhid_pen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.barrel_usages = barrels.data();
    options.barrel_usage_count = barrels.size();
    options.enable_hover = 1U;
    mimhid_spec* spec = nullptr;
    check_battery_variant(options, &mimhid_spec_create_pen);
    MIMHID_CHECK(mimhid_spec_create_pen(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    const auto* x = field(spec, mimhid::hid::FieldSemantic::x);
    const auto* y = field(spec, mimhid::hid::FieldSemantic::y);
    MIMHID_CHECK(x != nullptr && y != nullptr);
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    mimhid_node node{};
    node.spec = spec;
    node.state = mimhid::detail::PenState{};
    mimhid_pen_sample sample{};
    sample.in_range = 1U;
    sample.x = 100;
    sample.y = 200;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    // The away report carries the sample's own X/Y, not the last position.
    sample.in_range = 0U;
    sample.x = 500;
    sample.y = 600;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* range = field(spec, mimhid::hid::FieldSemantic::in_range);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    MIMHID_CHECK(x != nullptr && extract(report.data(), *x) == 500U);
    MIMHID_CHECK(y != nullptr && extract(report.data(), *y) == 600U);
    mimhid_spec_release(spec);
}

void test_pen_switch() {
    const std::array<std::uint16_t, 1> barrels{0x44U};
    mimhid_pen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_tilt = 0U;
    options.enable_twist_target_specific = 0U;
    options.barrel_usages = barrels.data();
    options.barrel_usage_count = barrels.size();
    options.enable_eraser = 1U;
    options.enable_hover = 1U;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_pen(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_capability_manifest manifest{};
    manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
    MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::PenState state_value{};
    state_value.emitted.in_range = 1U;
    state_value.emitted.barrel_buttons = 1U;
    state_value.emitted.x = 100;
    state_value.emitted.y = 200;
    node.state = state_value;
    mimhid_pen_sample sample{};
    sample.in_range = 1U;
    sample.eraser = 1U;
    sample.barrel_buttons = 1U;
    sample.x = 100;
    sample.y = 200;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    sample.in_range = 0U;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_ERR_BUSY);
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* range = field(spec, mimhid::hid::FieldSemantic::in_range);
    const auto* invert = field(spec, mimhid::hid::FieldSemantic::eraser);
    const auto* barrel = field(spec, mimhid::hid::FieldSemantic::buttons);
    const auto* tip = field(spec, mimhid::hid::FieldSemantic::tip);
    MIMHID_CHECK(invert != nullptr && tip != nullptr && range != nullptr &&
                 invert->bit_offset < tip->bit_offset && tip->bit_offset < range->bit_offset);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    MIMHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    MIMHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    // Model the order-sensitive tool selection in kernel commit
    // 35556bed836f8dc07ac55f69c8d17dce3e7f0e25. Departure releases both tools;
    // Invert must then be processed before In Range on eraser re-entry.
    PinnedLinuxPenTools tools{false, true, false};
    apply_pinned_linux_pen_report(spec, report.data(), &tools);
    MIMHID_CHECK(!tools.pen && !tools.rubber);
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 1U);
    MIMHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 1U);
    apply_pinned_linux_pen_report(spec, report.data(), &tools);
    MIMHID_CHECK(!tools.pen && tools.rubber);

    // Commit the eraser-side arrival, then leave range without making the
    // caller clear the remembered tool selector. The wire report must still
    // clear Invert so current Linux tool state can release BTN_TOOL_RUBBER.
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    sample.in_range = 0U;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* pressure = field(spec, mimhid::hid::FieldSemantic::pressure);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    MIMHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 0U);
    MIMHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    MIMHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    MIMHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 0U);

    // Re-enter on the remembered eraser side, then switch back to the pen side.
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    sample.in_range = 1U;
    sample.eraser = 1U;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 1U);
    MIMHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 1U);
    node.transfer_inflight.store(true);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    sample.eraser = 0U;
    MIMHID_CHECK(mimhid_pen_update(&node, &sample) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    MIMHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    MIMHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    mimhid_spec_release(spec);

    mimhid_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    digitizer.report_id = options.report_id;
    digitizer.x = options.x;
    digitizer.y = options.y;
    digitizer.enable_pressure = options.enable_pressure;
    digitizer.pressure = options.pressure;
    digitizer.barrel_usages = options.barrel_usages;
    digitizer.barrel_usage_count = options.barrel_usage_count;
    digitizer.enable_eraser = options.enable_eraser;
    digitizer.enable_hover = options.enable_hover;
    spec = nullptr;
    check_battery_variant(digitizer, &mimhid_spec_create_digitizer);
    MIMHID_CHECK(mimhid_spec_create_digitizer(&digitizer, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        manifest = {};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        MIMHID_CHECK(manifest.profile_kind == MIMHID_PROFILE_DIGITIZER);
        mimhid_node digitizer_node{};
        digitizer_node.spec = spec;
        digitizer_node.state = mimhid::detail::PenState{};
        digitizer_node.transfer_inflight.store(true);
        mimhid::detail::transfer_complete(&digitizer_node, MIMHID_OK, 0);
        mimhid_digitizer_sample digitizer_sample{};
        digitizer_sample.in_range = 1U;
        digitizer_sample.x = 10;
        digitizer_sample.y = 20;
        MIMHID_CHECK(mimhid_digitizer_update(&digitizer_node, &digitizer_sample) == MIMHID_OK);
        digitizer_sample.in_range = 0U;
        MIMHID_CHECK(mimhid_digitizer_update(&digitizer_node, &digitizer_sample) !=
                     MIMHID_ERR_PARAM);
        mimhid_spec_release(spec);
    }
}

} // namespace

void test_profiles_pen() {
    test_pen_away_coordinates();
    test_pen_switch();
}
