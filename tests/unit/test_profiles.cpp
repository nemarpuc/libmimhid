// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Verifies generated profile descriptors, layouts, and state obligations in
 * memory; it does not claim target-device acceptance. */
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimhid.h"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace {
static_assert(sizeof(mimhid_usage_semantic) == sizeof(std::int32_t));

std::uint64_t extract(const std::uint8_t* report, const mimhid::hid::FieldLayout& field) {
    std::uint64_t value = 0U;
    for (std::uint8_t bit = 0U; bit < field.bit_width; ++bit) {
        const std::size_t position = field.bit_offset + bit;
        value |= static_cast<std::uint64_t>((report[position / 8U] >> (position & 7U)) & 1U) << bit;
    }
    return value;
}

const mimhid::hid::FieldLayout* field(const mimhid_spec* spec,
                                      const mimhid::hid::FieldSemantic semantic,
                                      const std::uint16_t instance = 0U) {
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == semantic && candidate.instance == instance)
            return &candidate;
    }
    return nullptr;
}

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

std::size_t count_item(const std::vector<std::uint8_t>& descriptor, const std::uint8_t prefix,
                       const std::uint8_t value) {
    std::size_t count = 0U;
    for (std::size_t index = 1U; index < descriptor.size(); ++index) {
        if (descriptor[index - 1U] == prefix && descriptor[index] == value)
            ++count;
    }
    return count;
}

bool has_sequence(const std::vector<std::uint8_t>& descriptor,
                  const std::initializer_list<std::uint8_t> sequence) {
    return std::search(descriptor.begin(), descriptor.end(), sequence.begin(), sequence.end()) !=
           descriptor.end();
}

mimhid_keyboard_options keyboard_options() {
    mimhid_keyboard_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.reserved = 0U;
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.usage_minimum = 0x04U;
    options.usage_maximum = 0x65U;
    return options;
}

// A profile's valid options must also work with a battery report added, and
// the input side of the descriptor must not change.
template <typename Options>
void check_battery_variant(Options options,
                           mimhid_result(MIMHID_CALL* create)(const Options*, mimhid_spec**)) {
    options.report_id = {1U, 1U, {0U, 0U, 0U}};
    mimhid_spec* plain = nullptr;
    MIMHID_CHECK(create(&options, &plain) == MIMHID_OK);
    options.enable_battery = 1U;
    options.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    options.battery.strength = {1, 100, 8U, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(create(&options, &spec) == MIMHID_OK);
    if (plain != nullptr && spec != nullptr) {
        // The input side is the plain descriptor without its closing End
        // Collection; the battery report is the 16 bytes before the new one.
        MIMHID_CHECK(spec->descriptor.size() == plain->descriptor.size() + 16U);
        MIMHID_CHECK(std::equal(plain->descriptor.begin(), plain->descriptor.end() - 1,
                                spec->descriptor.begin()));
        MIMHID_CHECK(spec->layout.wire_bytes == plain->layout.wire_bytes);
        MIMHID_CHECK(!plain->has_battery && spec->has_battery &&
                     spec->battery_layout.report_id == 2U && spec->battery_layout.wire_bytes == 2U);
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        MIMHID_CHECK(manifest.report_count == 2U && manifest.reports[0].report_id.value == 1U &&
                     manifest.reports[1].report_id.enabled == 1U &&
                     manifest.reports[1].report_id.value == 2U &&
                     manifest.reports[1].wire_length == 2U);
        MIMHID_CHECK(mimhid_spec_manifest(plain, &manifest) == MIMHID_OK);
        MIMHID_CHECK(manifest.report_count == 1U);
    }
    mimhid_spec_release(plain);
    mimhid_spec_release(spec);
}

void test_keyboard_nkro_concurrent_keys() {
    mimhid_keyboard_options options = keyboard_options();
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
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0xE0U, 1U) == MIMHID_OK);
    node.dirty = false;
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0xE0U, 1U) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);

    // Twenty simultaneously held non-modifier keys: well beyond the legacy
    // six-slot Array limit. Full NKRO gives every Usage its own bitmap bit,
    // so there is no ErrorRollOver overflow to hit.
    constexpr std::uint16_t k_held_keys = 20U;
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage)
        MIMHID_CHECK(mimhid_keyboard_key(&node, usage, 1U) == MIMHID_OK);

    std::array<std::uint8_t, 64> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(report[0] == 0x01U);
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x04U, 0U) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* released = field(spec, mimhid::hid::FieldSemantic::key_bitmap, 0U);
    MIMHID_CHECK(released != nullptr && extract(report.data(), *released) == 0U);
    for (std::uint16_t usage = 0x05U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x66U, 0U) == MIMHID_ERR_PARAM);
    mimhid_spec_release(spec);
}

void test_keyboard_bitmap_and_lifecycle_transitions() {
    mimhid_keyboard_options options = keyboard_options();
    options.usage_maximum = 0x0BU;
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

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_ERR_BUSY);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap, 2U);
    MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_ERR_BUSY);
    report.fill(0xFFU);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 0U);
    mimhid::detail::transfer_complete(&node, MIMHID_ERR_IO, 1);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_ERR_BUSY);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_OK);
    mimhid_spec_release(spec);
}

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

// One pressed/transition slot per allowed Usage, as mimhid_node_open builds it.
mimhid::detail::ConsumerState consumer_state_for(const mimhid_spec* spec) {
    const auto* config = std::get_if<mimhid::detail::ConsumerConfig>(&spec->config);
    mimhid::detail::ConsumerState state{};
    state.pressed.resize(config->allowed_usages.size());
    state.transitions.resize(config->allowed_usages.size());
    return state;
}

void test_usage_control_semantics() {
    const std::array<std::uint16_t, 6U> usages{0x00CDU, 0x00E9U, 0x00E2U,
                                               0x0201U, 0x00B0U, 0x00BBU};
    const std::array<mimhid_usage_semantic, 6U> semantics{
        MIMHID_USAGE_ONE_SHOT,        MIMHID_USAGE_RETRIGGER,     MIMHID_USAGE_ON_OFF_MAINTAINED,
        MIMHID_USAGE_SELECTOR_BITMAP, MIMHID_USAGE_ON_OFF_TOGGLE, MIMHID_USAGE_MOMENTARY};
    // These are deliberately test markers, not claims about a target mapping.
    const std::array<const char*, 6U> event_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 6U> event_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};

    mimhid_toggle_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.application_page = mimhid::hid::usage::page_consumer;
    options.application_usage = mimhid::hid::usage::consumer_control;
    options.field_page = mimhid::hid::usage::page_consumer;
    options.allowed_usages = usages.data();
    options.allowed_usage_count = usages.size();
    options.usage_semantics = semantics.data();
    options.expected_linux_event_types = event_types.data();
    options.expected_linux_codes = event_codes.data();

    mimhid_spec* spec = nullptr;
    check_battery_variant(options, &mimhid_spec_create_toggle);
    MIMHID_CHECK(mimhid_spec_create_toggle(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        MIMHID_CHECK(count_item(spec->descriptor, 0x81U, 0x06U) == 2U);
        MIMHID_CHECK(count_item(spec->descriptor, 0x81U, 0x02U) == 3U);
        MIMHID_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimhid::hid::FieldSemantic::consumer_usage, index);
            MIMHID_CHECK(control != nullptr && control->bit_width == 1U &&
                         control->logical_minimum == 0 && control->logical_maximum == 1);
        }

        mimhid_node node{};
        node.spec = spec;
        node.state = consumer_state_for(spec);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00E9U, 1U) == MIMHID_OK);
        std::array<std::uint8_t, 8U> report{};
        std::size_t report_length = 0U;
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMHID_OK);
        MIMHID_CHECK(report_length == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimhid::hid::FieldSemantic::consumer_usage, index);
            MIMHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (index == 1U ? 1U : 0U));
        }
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00E9U, 0U) == MIMHID_ERR_BUSY);
        mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00E9U, 0U) == MIMHID_OK);
        report.fill(0xFFU);
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMHID_OK);
        MIMHID_CHECK(report[0] == 0U);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00E0U, 1U) == MIMHID_ERR_PARAM);

        // Several Usages can be held at once, and each is released by name.
        // Usage 0 is not in the allow-list, so there is no release-all call.
        mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00CDU, 1U) == MIMHID_OK);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00E9U, 1U) == MIMHID_OK);
        report.fill(0U);
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMHID_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimhid::hid::FieldSemantic::consumer_usage, index);
            const bool held = usages[index] == 0x00CDU || usages[index] == 0x00E9U;
            MIMHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (held ? 1U : 0U));
        }
        mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0x00CDU, 0U) == MIMHID_OK);
        report.fill(0U);
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMHID_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimhid::hid::FieldSemantic::consumer_usage, index);
            MIMHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (usages[index] == 0x00E9U ? 1U : 0U));
        }
        mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
        MIMHID_CHECK(mimhid_toggle_usage(&node, 0U, 0U) == MIMHID_ERR_PARAM);

        // The state object enforces the same accepted 1->0 lifecycle for each
        // caller-declared HUT semantic. Semantic flags stay descriptor policy;
        // the caller never hand-crafts an impossible re-trigger sequence.
        for (std::size_t selected = 0U; selected < usages.size(); ++selected) {
            mimhid_node semantic_node{};
            semantic_node.spec = spec;
            semantic_node.state = consumer_state_for(spec);
            MIMHID_CHECK(mimhid_toggle_usage(&semantic_node, usages[selected], 1U) == MIMHID_OK);
            report.fill(0U);
            MIMHID_CHECK(mimhid::detail::serialize_node(&semantic_node, report.data(),
                                                        report.size(),
                                                        &report_length) == MIMHID_OK);
            for (std::size_t index = 0U; index < usages.size(); ++index) {
                const auto* control = field(spec, mimhid::hid::FieldSemantic::consumer_usage,
                                            static_cast<std::uint16_t>(index));
                MIMHID_CHECK(control != nullptr &&
                             extract(report.data(), *control) == (index == selected ? 1U : 0U));
            }
            MIMHID_CHECK(mimhid_toggle_usage(&semantic_node, usages[selected], 0U) ==
                         MIMHID_ERR_BUSY);
            mimhid::detail::transfer_complete(&semantic_node, MIMHID_OK, 0);
            MIMHID_CHECK(mimhid_toggle_usage(&semantic_node, usages[selected], 0U) == MIMHID_OK);
            report.fill(0xFFU);
            MIMHID_CHECK(mimhid::detail::serialize_node(&semantic_node, report.data(),
                                                        report.size(),
                                                        &report_length) == MIMHID_OK);
            MIMHID_CHECK(mimhid_toggle_usage(&semantic_node, usages[selected], 1U) ==
                         MIMHID_ERR_BUSY);
            mimhid::detail::transfer_complete(&semantic_node, MIMHID_OK, 0);
            MIMHID_CHECK(mimhid_toggle_usage(&semantic_node, usages[selected], 1U) == MIMHID_OK);
        }
    }
    mimhid_spec_release(spec);

    const auto exercise_shared_control_state = [](mimhid_spec* control_spec,
                                                  const std::uint16_t usage) {
        if (control_spec == nullptr)
            return;
        mimhid_node control_node{};
        control_node.spec = control_spec;
        control_node.state = consumer_state_for(control_spec);
        MIMHID_CHECK(mimhid_toggle_usage(&control_node, usage, 1U) == MIMHID_OK);
        std::array<std::uint8_t, 16U> control_report{};
        std::size_t control_length = 0U;
        MIMHID_CHECK(mimhid::detail::serialize_node(&control_node, control_report.data(),
                                                    control_report.size(),
                                                    &control_length) == MIMHID_OK);
        const auto* selected = field(control_spec, mimhid::hid::FieldSemantic::consumer_usage, 0U);
        MIMHID_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 1U);
        mimhid::detail::transfer_complete(&control_node, MIMHID_OK, 0);
        MIMHID_CHECK(mimhid_toggle_usage(&control_node, usage, 0U) == MIMHID_OK);
        control_report.fill(0xFFU);
        MIMHID_CHECK(mimhid::detail::serialize_node(&control_node, control_report.data(),
                                                    control_report.size(),
                                                    &control_length) == MIMHID_OK);
        MIMHID_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 0U);
    };

    mimhid_toggle_options invalid = options;
    invalid.usage_semantics = nullptr;
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_UNSET_FIELD);

    std::array<std::uint16_t, 2U> duplicate_usages{0x00CDU, 0x00CDU};
    std::array<mimhid_usage_semantic, 2U> duplicate_semantics{MIMHID_USAGE_ONE_SHOT,
                                                              MIMHID_USAGE_ONE_SHOT};
    std::array<const char*, 2U> duplicate_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    std::array<const char*, 2U> duplicate_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid = {};
    invalid.struct_size = static_cast<std::uint32_t>(sizeof(invalid));
    invalid.application_page = mimhid::hid::usage::page_consumer;
    invalid.application_usage = mimhid::hid::usage::consumer_control;
    invalid.field_page = mimhid::hid::usage::page_consumer;
    invalid.allowed_usages = duplicate_usages.data();
    invalid.allowed_usage_count = duplicate_usages.size();
    invalid.usage_semantics = duplicate_semantics.data();
    invalid.expected_linux_event_types = duplicate_types.data();
    invalid.expected_linux_codes = duplicate_codes.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_PARAM);

    const std::array<std::uint16_t, 1U> volume_usage{0x00E0U};
    const std::array<mimhid_usage_semantic, 1U> volume_semantic{MIMHID_USAGE_LINEAR};
    const std::array<const char*, 1U> volume_type{"TEST_EVENT_TYPE"};
    const std::array<const char*, 1U> volume_code{"TEST_EVENT_CODE"};
    invalid.allowed_usages = volume_usage.data();
    invalid.allowed_usage_count = volume_usage.size();
    invalid.usage_semantics = volume_semantic.data();
    invalid.expected_linux_event_types = volume_type.data();
    invalid.expected_linux_codes = volume_code.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_UNSUPPORTED);

    std::array<mimhid_usage_semantic, 1U> invalid_semantic{-1};
    invalid.usage_semantics = invalid_semantic.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_PARAM);

    const std::array<std::uint16_t, 1U> collection_usage{0x0001U};
    invalid_semantic[0] = MIMHID_USAGE_SELECTOR_BITMAP;
    invalid.allowed_usages = collection_usage.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_PARAM);

    const std::array<std::uint16_t, 2U> camera_usages{0x20U, 0x21U};
    std::array<mimhid_usage_semantic, 2U> camera_semantics{MIMHID_USAGE_ONE_SHOT,
                                                           MIMHID_USAGE_ONE_SHOT};
    const std::array<const char*, 2U> camera_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 2U> camera_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.field_page = mimhid::hid::usage::page_camera_control;
    invalid.allowed_usages = camera_usages.data();
    invalid.allowed_usage_count = camera_usages.size();
    invalid.usage_semantics = camera_semantics.data();
    invalid.expected_linux_event_types = camera_types.data();
    invalid.expected_linux_codes = camera_codes.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_OK);
    exercise_shared_control_state(spec, camera_usages[0]);
    mimhid_spec_release(spec);
    spec = nullptr;
    camera_semantics[0] = MIMHID_USAGE_SELECTOR_BITMAP;
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_ERR_UNSUPPORTED);

    const std::array<std::uint16_t, 4U> system_usages{0x81U, 0x8AU, 0x97U, 0x98U};
    const std::array<mimhid_usage_semantic, 4U> system_semantics{
        MIMHID_USAGE_ONE_SHOT, MIMHID_USAGE_RETRIGGER, MIMHID_USAGE_MOMENTARY,
        MIMHID_USAGE_ON_OFF_TOGGLE};
    const std::array<const char*, 4U> system_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                   "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> system_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                   "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = mimhid::hid::usage::page_generic_desktop;
    invalid.application_usage = mimhid::hid::usage::system_control;
    invalid.field_page = mimhid::hid::usage::page_generic_desktop;
    invalid.allowed_usages = system_usages.data();
    invalid.allowed_usage_count = system_usages.size();
    invalid.usage_semantics = system_semantics.data();
    invalid.expected_linux_event_types = system_types.data();
    invalid.expected_linux_codes = system_codes.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_OK);
    exercise_shared_control_state(spec, system_usages[0]);
    mimhid_spec_release(spec);
    spec = nullptr;

    const std::array<std::uint16_t, 4U> phone_usages{0x20U, 0x21U, 0x24U, 0xB0U};
    const std::array<mimhid_usage_semantic, 4U> phone_semantics{
        MIMHID_USAGE_ON_OFF_MAINTAINED, MIMHID_USAGE_MOMENTARY, MIMHID_USAGE_ONE_SHOT,
        MIMHID_USAGE_SELECTOR_BITMAP};
    const std::array<const char*, 4U> phone_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> phone_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = mimhid::hid::usage::page_telephony;
    invalid.application_usage = mimhid::hid::usage::phone;
    invalid.field_page = mimhid::hid::usage::page_telephony;
    invalid.allowed_usages = phone_usages.data();
    invalid.allowed_usage_count = phone_usages.size();
    invalid.usage_semantics = phone_semantics.data();
    invalid.expected_linux_event_types = phone_types.data();
    invalid.expected_linux_codes = phone_codes.data();
    MIMHID_CHECK(mimhid_spec_create_toggle(&invalid, &spec) == MIMHID_OK);
    exercise_shared_control_state(spec, phone_usages[0]);
    mimhid_spec_release(spec);
}

void test_hat_domain() {
    std::array<mimhid_gamepad_axis_option, 2> axes{};
    axes[0] = {MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"};
    axes[1] = {MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"};
    mimhid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 15;
    options.hat_bit_width = 4U;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    options.hat_logical_maximum = 7;
    check_battery_variant(options, &mimhid_spec_create_gamepad);
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        mimhid_node node{};
        node.spec = spec;
        mimhid::detail::GamepadState state_value{};
        state_value.axes.resize(axes.size());
        state_value.buttons.resize(options.button_count);
        state_value.button_transitions.resize(options.button_count);
        node.state = std::move(state_value);
        std::array<std::uint8_t, 32U> report{};
        std::size_t length = 0U;
        MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                     MIMHID_OK);
        const auto* hat = field(spec, mimhid::hid::FieldSemantic::hat);
        MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 15U);
    }
    mimhid_spec_release(spec);

    axes[0].usage_page = 0x02U;
    axes[0].usage = 0xC4U;
    spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[0].usage_page = 0x01U;
    axes[0].usage = 0x30U;

    axes[1].role = MIMHID_AXIS_X;
    axes[1].usage = 0x30U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[1].role = MIMHID_AXIS_Y;
    axes[1].usage = 0x31U;

    axes[1].expected_linux_code = "";
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_UNSET_FIELD);
}

void test_controller_dpad_and_extended_axes() {
    std::array<mimhid_gamepad_axis_option, 6U> axes{
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_DIAL, 0x01U, 0x37U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_WHEEL, 0x01U, 0x38U, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_SIMULATION_RUDDER, 0x02U, 0xBAU, {-127, 127, 8U, {}}, 0, "TEST"},
        mimhid_gamepad_axis_option{
            MIMHID_AXIS_SIMULATION_THROTTLE, 0x02U, 0xBBU, {0, 255, 8U, {}}, 0, "TEST"}};
    mimhid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMHID_DPAD_BUTTONS;

    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_capability_manifest raw_dpad_manifest{};
    raw_dpad_manifest.struct_size = static_cast<std::uint32_t>(sizeof(raw_dpad_manifest));
    MIMHID_CHECK(mimhid_spec_manifest(spec, &raw_dpad_manifest) == MIMHID_OK);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x90U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x91U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x92U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x09U, 0x93U) == 1U);
    MIMHID_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 4U);

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::GamepadState gamepad{};
    gamepad.axes.resize(axes.size());
    gamepad.buttons.resize(options.button_count);
    gamepad.button_transitions.resize(options.button_count);
    node.state = std::move(gamepad);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMHID_OK);
    MIMHID_CHECK(node.dirty);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* up = field(spec, mimhid::hid::FieldSemantic::buttons, 1U);
    const auto* down = field(spec, mimhid::hid::FieldSemantic::buttons, 2U);
    const auto* right = field(spec, mimhid::hid::FieldSemantic::buttons, 3U);
    const auto* left = field(spec, mimhid::hid::FieldSemantic::buttons, 4U);
    MIMHID_CHECK(up != nullptr && extract(report.data(), *up) == 1U);
    MIMHID_CHECK(down != nullptr && extract(report.data(), *down) == 0U);
    MIMHID_CHECK(right != nullptr && extract(report.data(), *right) == 1U);
    MIMHID_CHECK(left != nullptr && extract(report.data(), *left) == 0U);
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMHID_ERR_BUSY);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(mimhid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == MIMHID_OK);
    MIMHID_CHECK(node.dirty);
    node.dirty = false;
    MIMHID_CHECK(mimhid_gamepad_axis(&node, 0U, 0) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);
    mimhid_spec_release(spec);
    spec = nullptr;

    options.button_count = 4U;
    options.dpad_representation = MIMHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 7;
    options.hat_bit_width = 4U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest four_button_manifest{};
        four_button_manifest.struct_size = static_cast<std::uint32_t>(sizeof(four_button_manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &four_button_manifest) == MIMHID_OK);
        mimhid_spec_release(spec);
        spec = nullptr;
    }
    options.button_count = 5U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        const auto* hat = field(spec, mimhid::hid::FieldSemantic::hat);
        MIMHID_CHECK(hat != nullptr && hat->physical.enabled == 1U && hat->physical.minimum == 0 &&
                     hat->physical.maximum == 315 && hat->physical.unit_exponent == 0 &&
                     hat->physical.unit == 0x14U);
        mimhid_node hat_node{};
        hat_node.spec = spec;
        mimhid::detail::GamepadState hat_state{};
        hat_state.axes.resize(axes.size());
        hat_state.buttons.resize(options.button_count);
        hat_state.button_transitions.resize(options.button_count);
        hat_node.state = std::move(hat_state);
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
            MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, direction.up, direction.down,
                                             direction.right, direction.left) == MIMHID_OK);
            report.fill(0U);
            MIMHID_CHECK(mimhid::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                        &length) == MIMHID_OK);
            MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == direction.wire);
            mimhid::detail::transfer_complete(&hat_node, MIMHID_OK, 0);
            hat_node.dirty = false;
        }
        MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, 1U, 1U, 0U, 0U) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_gamepad_dpad(&hat_node, 0U, 0U, 1U, 1U) == MIMHID_ERR_PARAM);
        report.fill(0U);
        MIMHID_CHECK(mimhid::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                    &length) == MIMHID_OK);
        MIMHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 7U);
        MIMHID_CHECK(!hat_node.dirty);
        mimhid_spec_release(spec);
        spec = nullptr;
    }

    axes[4].neutral_value = 1;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[4].neutral_value = 0;
    axes[4].value = {0, 127, 8U, {}};
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[4].value = {-127, 127, 8U, {}};
    axes[5].neutral_value = 1;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[5].neutral_value = 0;
    axes[5].value = {-1, 255, 16U, {}};
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
    axes[5].value = {0, 255, 8U, {}};

    options.dpad_representation = MIMHID_DPAD_NONE;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 0;
    options.hat_bit_width = 4U;
    MIMHID_CHECK(mimhid_spec_create_gamepad(&options, &spec) == MIMHID_ERR_PARAM);
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

mimhid_touchscreen_options touchscreen_options() {
    mimhid_touchscreen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.maximum_contacts = 3U;
    options.contacts_per_report = 2U;
    options.contact_id = {0, 15, 4U, {}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.contact_count = {0, 3, 2U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_width = 0U;
    options.width = {0, 0, 0U, {}};
    options.enable_height = 0U;
    options.height = {0, 0, 0U, {}};
    options.enable_scan_time = 1U;
    options.scan_time = {0, 65535, 16U, {}};
    options.scan_time_unit_100us = 1U;
    options.enable_contact_count_maximum_feature_declaration = 0U;
    options.enable_multi_packet_frames = 1U;
    return options;
}

mimhid_touchpad_options touchpad_options() {
    mimhid_touchpad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.maximum_contacts = 3U;
    options.contacts_per_report = 2U;
    options.contact_id = {0, 15, 4U, {}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.contact_count = {0, 3, 2U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_width = 0U;
    options.width = {0, 0, 0U, {}};
    options.enable_height = 0U;
    options.height = {0, 0, 0U, {}};
    options.enable_scan_time = 1U;
    options.scan_time = {0, 65535, 16U, {}};
    options.scan_time_unit_100us = 1U;
    options.enable_contact_count_maximum_feature_declaration = 0U;
    options.enable_multi_packet_frames = 1U;
    options.button_count = 0U;
    return options;
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

void test_raw_report_id_order() {
    const std::array<std::uint8_t, 17> valid{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                             0x85U, 0x01U, 0x15U, 0x00U, 0x25U, 0x01U,
                                             0x75U, 0x01U, 0x95U, 0x01U, 0x81U};
    // Complete the Input item data and collection in a separate exact array.
    const std::array<std::uint8_t, 21> accepted{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    (void)valid;
    const mimhid_raw_report report{{1U, 1U, {}}, 2U};
    MIMHID_CHECK(mimhid::hid::validate_raw(accepted.data(), accepted.size(), &report, 1U).result ==
                 MIMHID_OK);
    const std::array<std::uint8_t, 19> rejected{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x15U,
                                                0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U,
                                                0x81U, 0x02U, 0x85U, 0x01U, 0xC0U};
    MIMHID_CHECK(mimhid::hid::validate_raw(rejected.data(), rejected.size(), &report, 1U).result !=
                 MIMHID_OK);

    const mimhid_raw_report no_id_report{{0U, 0U, {}}, 1U};
    const std::array<std::uint8_t, 17> output_item{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                   0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                   0x95U, 0x01U, 0x91U, 0x02U, 0xC0U};
    MIMHID_CHECK(
        mimhid::hid::validate_raw(output_item.data(), output_item.size(), &no_id_report, 1U)
            .result == MIMHID_ERR_UNSUPPORTED);

    const std::array<std::uint8_t, 19> restored_id_zero{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0xA4U, 0x85U, 0x01U, 0x75U,
        0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xB4U, 0x81U, 0x02U, 0xC0U};
    MIMHID_CHECK(
        mimhid::hid::validate_raw(restored_id_zero.data(), restored_id_zero.size(), &report, 1U)
            .result == MIMHID_ERR_PARAM);

    const std::array<std::uint8_t, 16> excessive_count{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                       0x75U, 0x01U, 0x97U, 0xFFU, 0xFFU, 0xFFU,
                                                       0xFFU, 0x81U, 0x02U, 0xC0U};
    MIMHID_CHECK(
        mimhid::hid::validate_raw(excessive_count.data(), excessive_count.size(), &no_id_report, 1U)
            .result == MIMHID_ERR_PARAM);

    const std::array<std::uint8_t, 19> range_exceeds_size{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U,
        0x25U, 0x03U, 0x75U, 0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    MIMHID_CHECK(
        mimhid::hid::validate_raw(range_exceeds_size.data(), range_exceeds_size.size(), &report, 1U)
            .result == MIMHID_ERR_PARAM);
}

void test_raw_report_table_report_id_rules() {
    const std::array<std::uint8_t, 21> descriptor{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                  0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                  0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const auto result_for = [&](const mimhid_raw_report& report) {
        return mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result;
    };
    MIMHID_CHECK(result_for({{1U, 1U, {}}, 2U}) == MIMHID_OK);
    // The flag is exactly zero or one.
    MIMHID_CHECK(result_for({{2U, 1U, {}}, 2U}) == MIMHID_ERR_PARAM);
    // A declared ID is 1 through 255, and a disabled ID is zero.
    MIMHID_CHECK(result_for({{1U, 0U, {}}, 2U}) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(result_for({{0U, 1U, {}}, 2U}) == MIMHID_ERR_PARAM);
    // The reserved bytes are zero.
    MIMHID_CHECK(result_for({{1U, 1U, {1U, 0U, 0U}}, 2U}) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(result_for({{1U, 1U, {0U, 0U, 1U}}, 2U}) == MIMHID_ERR_PARAM);
    // The wire length is not zero.
    MIMHID_CHECK(result_for({{1U, 1U, {}}, 0U}) == MIMHID_ERR_PARAM);

    // The same ID twice is one report identity listed twice.
    const std::array<mimhid_raw_report, 2> duplicate{mimhid_raw_report{{1U, 1U, {}}, 2U},
                                                     mimhid_raw_report{{1U, 1U, {}}, 2U}};
    MIMHID_CHECK(mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), duplicate.data(),
                                           duplicate.size())
                     .result == MIMHID_ERR_PARAM);

    // The raw spec reports the table back through the manifest unchanged.
    const mimhid_raw_report report{{1U, 1U, {}}, 2U};
    const mimhid_raw_options options{sizeof(mimhid_raw_options),
                                     0U,
                                     descriptor.data(),
                                     descriptor.size(),
                                     &report,
                                     1U,
                                     0U,
                                     0U};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_raw(&options, &spec) == MIMHID_OK);
    if (spec != nullptr) {
        mimhid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMHID_CHECK(mimhid_spec_manifest(spec, &manifest) == MIMHID_OK);
        MIMHID_CHECK(manifest.report_count == 1U);
        if (manifest.report_count == 1U) {
            MIMHID_CHECK(manifest.reports[0].report_id.enabled == 1U);
            MIMHID_CHECK(manifest.reports[0].report_id.value == 1U);
            MIMHID_CHECK(manifest.reports[0].wire_length == 2U);
        }
        mimhid_spec_release(spec);
    }
}

void test_raw_factory_bounds_before_allocation() {
    const std::array<std::uint8_t, 1U> descriptor{0xC0U};
    const mimhid_raw_report report{};
    mimhid_raw_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.descriptor = descriptor.data();
    options.descriptor_length = descriptor.size();
    options.reports = &report;
    options.report_count = std::numeric_limits<std::size_t>::max();
    mimhid_spec* spec = reinterpret_cast<mimhid_spec*>(std::uintptr_t{1U});
    MIMHID_CHECK(mimhid_spec_create_raw(&options, &spec) == MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(spec == nullptr);

    options.reports = nullptr;
    MIMHID_CHECK(mimhid_spec_create_raw(&options, &spec) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(spec == nullptr);
}

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
} // namespace

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

void test_profiles() {
    mimhid_keyboard_options zero{};
    auto* spec = reinterpret_cast<mimhid_spec*>(static_cast<std::uintptr_t>(1U));
    MIMHID_CHECK(mimhid_spec_create_keyboard(&zero, &spec) != MIMHID_OK);
    MIMHID_CHECK(spec == nullptr);
    MIMHID_CHECK(mimhid_spec_create_keyboard(&zero, nullptr) == MIMHID_ERR_PARAM);

    mimhid_keyboard_options invalid_keyboard = keyboard_options();
    invalid_keyboard.report_id.reserved8[0] = 1U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_minimum = 1U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum =
        static_cast<std::uint16_t>(invalid_keyboard.usage_minimum - 1U);
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum = 0xE0U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);

    test_keyboard_nkro_concurrent_keys();
    test_keyboard_bitmap_and_lifecycle_transitions();
    test_mouse_delta_splitting_and_consumption();
    test_usage_control_semantics();
    test_hat_domain();
    test_controller_dpad_and_extended_axes();
    test_physical_properties_and_reset();
    test_field_span_padding();
    test_touch_units_feature_and_manifest_status();
    test_touch_azimuth();
    test_touch_packets();
    test_touch_lift_frame_and_idle_suppression();
    test_touchpad_button_only_and_mid_frame_guard();
    test_pen_away_coordinates();
    test_pen_switch();
    test_raw_report_id_order();
    test_raw_report_table_report_id_rules();
    test_raw_factory_bounds_before_allocation();
    test_send_time_report_validation();
    test_battery_builder();
    test_battery_options();
    test_battery_state();
    test_error_fields_name_their_profile();

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
    spec = nullptr;

    // The profile numbers are part of the ABI.
    MIMHID_CHECK(MIMHID_PROFILE_KEYBOARD == 1 && MIMHID_PROFILE_MOUSE == 2 &&
                 MIMHID_PROFILE_TOUCHSCREEN == 3 && MIMHID_PROFILE_TOUCHPAD == 4 &&
                 MIMHID_PROFILE_PEN == 5 && MIMHID_PROFILE_DIGITIZER == 6 &&
                 MIMHID_PROFILE_GAMEPAD == 7 && MIMHID_PROFILE_TOGGLE == 8 &&
                 MIMHID_PROFILE_RAW == 9);
}
