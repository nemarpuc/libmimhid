// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Usage-control profiles (consumer, system, toggle): one-shot, on/off and momentary semantics. */
#include "api/internal.hpp"
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

// One pressed/transition slot per allowed Usage, as mimusb_node_open builds it.
mimusb::detail::ConsumerState consumer_state_for(const mimusb_spec* spec) {
    const auto* config = std::get_if<mimusb::detail::ConsumerConfig>(&spec->config);
    mimusb::detail::ConsumerState state{};
    state.pressed.resize(config->allowed_usages.size());
    state.transitions.resize(config->allowed_usages.size());
    return state;
}

void test_usage_control_semantics() {
    const std::array<std::uint16_t, 6U> usages{0x00CDU, 0x00E9U, 0x00E2U,
                                               0x0201U, 0x00B0U, 0x00BBU};
    const std::array<mimusb_usage_semantic, 6U> semantics{
        MIMUSB_USAGE_ONE_SHOT,        MIMUSB_USAGE_RETRIGGER,     MIMUSB_USAGE_ON_OFF_MAINTAINED,
        MIMUSB_USAGE_SELECTOR_BITMAP, MIMUSB_USAGE_ON_OFF_TOGGLE, MIMUSB_USAGE_MOMENTARY};
    // These are deliberately test markers, not claims about a target mapping.
    const std::array<const char*, 6U> event_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 6U> event_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};

    mimusb_toggle_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.application_page = mimusb::hid::usage::page_consumer;
    options.application_usage = mimusb::hid::usage::consumer_control;
    options.field_page = mimusb::hid::usage::page_consumer;
    options.allowed_usages = usages.data();
    options.allowed_usage_count = usages.size();
    options.usage_semantics = semantics.data();
    options.expected_linux_event_types = event_types.data();
    options.expected_linux_codes = event_codes.data();

    mimusb_spec* spec = nullptr;
    check_battery_variant(options, &mimusb_spec_create_toggle);
    MIMUSB_CHECK(mimusb_spec_create_toggle(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        MIMUSB_CHECK(count_item(spec->descriptor, 0x81U, 0x06U) == 2U);
        MIMUSB_CHECK(count_item(spec->descriptor, 0x81U, 0x02U) == 3U);
        MIMUSB_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimusb::hid::FieldSemantic::consumer_usage, index);
            MIMUSB_CHECK(control != nullptr && control->bit_width == 1U &&
                         control->logical_minimum == 0 && control->logical_maximum == 1);
        }

        mimusb_node node{};
        node.spec = spec;
        node.state = consumer_state_for(spec);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00E9U, 1U) == MIMUSB_OK);
        std::array<std::uint8_t, 8U> report{};
        std::size_t report_length = 0U;
        MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMUSB_OK);
        MIMUSB_CHECK(report_length == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimusb::hid::FieldSemantic::consumer_usage, index);
            MIMUSB_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (index == 1U ? 1U : 0U));
        }
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00E9U, 0U) == MIMUSB_ERR_BUSY);
        mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00E9U, 0U) == MIMUSB_OK);
        report.fill(0xFFU);
        MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMUSB_OK);
        MIMUSB_CHECK(report[0] == 0U);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00E0U, 1U) == MIMUSB_ERR_PARAM);

        // Several Usages can be held at once, and each is released by name.
        // Usage 0 is not in the allow-list, so there is no release-all call.
        mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00CDU, 1U) == MIMUSB_OK);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00E9U, 1U) == MIMUSB_OK);
        report.fill(0U);
        MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMUSB_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimusb::hid::FieldSemantic::consumer_usage, index);
            const bool held = usages[index] == 0x00CDU || usages[index] == 0x00E9U;
            MIMUSB_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (held ? 1U : 0U));
        }
        mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0x00CDU, 0U) == MIMUSB_OK);
        report.fill(0U);
        MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(),
                                                    &report_length) == MIMUSB_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, mimusb::hid::FieldSemantic::consumer_usage, index);
            MIMUSB_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (usages[index] == 0x00E9U ? 1U : 0U));
        }
        mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);
        MIMUSB_CHECK(mimusb_toggle_usage(&node, 0U, 0U) == MIMUSB_ERR_PARAM);

        // The state object enforces the same accepted 1->0 lifecycle for each
        // caller-declared HUT semantic. Semantic flags stay descriptor policy;
        // the caller never hand-crafts an impossible re-trigger sequence.
        for (std::size_t selected = 0U; selected < usages.size(); ++selected) {
            mimusb_node semantic_node{};
            semantic_node.spec = spec;
            semantic_node.state = consumer_state_for(spec);
            MIMUSB_CHECK(mimusb_toggle_usage(&semantic_node, usages[selected], 1U) == MIMUSB_OK);
            report.fill(0U);
            MIMUSB_CHECK(mimusb::detail::serialize_node(&semantic_node, report.data(),
                                                        report.size(),
                                                        &report_length) == MIMUSB_OK);
            for (std::size_t index = 0U; index < usages.size(); ++index) {
                const auto* control = field(spec, mimusb::hid::FieldSemantic::consumer_usage,
                                            static_cast<std::uint16_t>(index));
                MIMUSB_CHECK(control != nullptr &&
                             extract(report.data(), *control) == (index == selected ? 1U : 0U));
            }
            MIMUSB_CHECK(mimusb_toggle_usage(&semantic_node, usages[selected], 0U) ==
                         MIMUSB_ERR_BUSY);
            mimusb::detail::transfer_complete(&semantic_node, MIMUSB_OK, 0);
            MIMUSB_CHECK(mimusb_toggle_usage(&semantic_node, usages[selected], 0U) == MIMUSB_OK);
            report.fill(0xFFU);
            MIMUSB_CHECK(mimusb::detail::serialize_node(&semantic_node, report.data(),
                                                        report.size(),
                                                        &report_length) == MIMUSB_OK);
            MIMUSB_CHECK(mimusb_toggle_usage(&semantic_node, usages[selected], 1U) ==
                         MIMUSB_ERR_BUSY);
            mimusb::detail::transfer_complete(&semantic_node, MIMUSB_OK, 0);
            MIMUSB_CHECK(mimusb_toggle_usage(&semantic_node, usages[selected], 1U) == MIMUSB_OK);
        }
    }
    mimusb_spec_release(spec);

    const auto exercise_shared_control_state = [](mimusb_spec* control_spec,
                                                  const std::uint16_t usage) {
        if (control_spec == nullptr)
            return;
        mimusb_node control_node{};
        control_node.spec = control_spec;
        control_node.state = consumer_state_for(control_spec);
        MIMUSB_CHECK(mimusb_toggle_usage(&control_node, usage, 1U) == MIMUSB_OK);
        std::array<std::uint8_t, 16U> control_report{};
        std::size_t control_length = 0U;
        MIMUSB_CHECK(mimusb::detail::serialize_node(&control_node, control_report.data(),
                                                    control_report.size(),
                                                    &control_length) == MIMUSB_OK);
        const auto* selected = field(control_spec, mimusb::hid::FieldSemantic::consumer_usage, 0U);
        MIMUSB_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 1U);
        mimusb::detail::transfer_complete(&control_node, MIMUSB_OK, 0);
        MIMUSB_CHECK(mimusb_toggle_usage(&control_node, usage, 0U) == MIMUSB_OK);
        control_report.fill(0xFFU);
        MIMUSB_CHECK(mimusb::detail::serialize_node(&control_node, control_report.data(),
                                                    control_report.size(),
                                                    &control_length) == MIMUSB_OK);
        MIMUSB_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 0U);
    };

    mimusb_toggle_options invalid = options;
    invalid.usage_semantics = nullptr;
    spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_UNSET_FIELD);

    std::array<std::uint16_t, 2U> duplicate_usages{0x00CDU, 0x00CDU};
    std::array<mimusb_usage_semantic, 2U> duplicate_semantics{MIMUSB_USAGE_ONE_SHOT,
                                                              MIMUSB_USAGE_ONE_SHOT};
    std::array<const char*, 2U> duplicate_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    std::array<const char*, 2U> duplicate_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid = {};
    invalid.struct_size = static_cast<std::uint32_t>(sizeof(invalid));
    invalid.application_page = mimusb::hid::usage::page_consumer;
    invalid.application_usage = mimusb::hid::usage::consumer_control;
    invalid.field_page = mimusb::hid::usage::page_consumer;
    invalid.allowed_usages = duplicate_usages.data();
    invalid.allowed_usage_count = duplicate_usages.size();
    invalid.usage_semantics = duplicate_semantics.data();
    invalid.expected_linux_event_types = duplicate_types.data();
    invalid.expected_linux_codes = duplicate_codes.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_PARAM);

    const std::array<std::uint16_t, 1U> volume_usage{0x00E0U};
    const std::array<mimusb_usage_semantic, 1U> volume_semantic{MIMUSB_USAGE_LINEAR};
    const std::array<const char*, 1U> volume_type{"TEST_EVENT_TYPE"};
    const std::array<const char*, 1U> volume_code{"TEST_EVENT_CODE"};
    invalid.allowed_usages = volume_usage.data();
    invalid.allowed_usage_count = volume_usage.size();
    invalid.usage_semantics = volume_semantic.data();
    invalid.expected_linux_event_types = volume_type.data();
    invalid.expected_linux_codes = volume_code.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_UNSUPPORTED);

    std::array<mimusb_usage_semantic, 1U> invalid_semantic{-1};
    invalid.usage_semantics = invalid_semantic.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_PARAM);

    const std::array<std::uint16_t, 1U> collection_usage{0x0001U};
    invalid_semantic[0] = MIMUSB_USAGE_SELECTOR_BITMAP;
    invalid.allowed_usages = collection_usage.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_PARAM);

    const std::array<std::uint16_t, 2U> camera_usages{0x20U, 0x21U};
    std::array<mimusb_usage_semantic, 2U> camera_semantics{MIMUSB_USAGE_ONE_SHOT,
                                                           MIMUSB_USAGE_ONE_SHOT};
    const std::array<const char*, 2U> camera_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 2U> camera_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.field_page = mimusb::hid::usage::page_camera_control;
    invalid.allowed_usages = camera_usages.data();
    invalid.allowed_usage_count = camera_usages.size();
    invalid.usage_semantics = camera_semantics.data();
    invalid.expected_linux_event_types = camera_types.data();
    invalid.expected_linux_codes = camera_codes.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_OK);
    exercise_shared_control_state(spec, camera_usages[0]);
    mimusb_spec_release(spec);
    spec = nullptr;
    camera_semantics[0] = MIMUSB_USAGE_SELECTOR_BITMAP;
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_ERR_UNSUPPORTED);

    const std::array<std::uint16_t, 4U> system_usages{0x81U, 0x8AU, 0x97U, 0x98U};
    const std::array<mimusb_usage_semantic, 4U> system_semantics{
        MIMUSB_USAGE_ONE_SHOT, MIMUSB_USAGE_RETRIGGER, MIMUSB_USAGE_MOMENTARY,
        MIMUSB_USAGE_ON_OFF_TOGGLE};
    const std::array<const char*, 4U> system_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                   "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> system_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                   "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = mimusb::hid::usage::page_generic_desktop;
    invalid.application_usage = mimusb::hid::usage::system_control;
    invalid.field_page = mimusb::hid::usage::page_generic_desktop;
    invalid.allowed_usages = system_usages.data();
    invalid.allowed_usage_count = system_usages.size();
    invalid.usage_semantics = system_semantics.data();
    invalid.expected_linux_event_types = system_types.data();
    invalid.expected_linux_codes = system_codes.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_OK);
    exercise_shared_control_state(spec, system_usages[0]);
    mimusb_spec_release(spec);
    spec = nullptr;

    const std::array<std::uint16_t, 4U> phone_usages{0x20U, 0x21U, 0x24U, 0xB0U};
    const std::array<mimusb_usage_semantic, 4U> phone_semantics{
        MIMUSB_USAGE_ON_OFF_MAINTAINED, MIMUSB_USAGE_MOMENTARY, MIMUSB_USAGE_ONE_SHOT,
        MIMUSB_USAGE_SELECTOR_BITMAP};
    const std::array<const char*, 4U> phone_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> phone_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = mimusb::hid::usage::page_telephony;
    invalid.application_usage = mimusb::hid::usage::phone;
    invalid.field_page = mimusb::hid::usage::page_telephony;
    invalid.allowed_usages = phone_usages.data();
    invalid.allowed_usage_count = phone_usages.size();
    invalid.usage_semantics = phone_semantics.data();
    invalid.expected_linux_event_types = phone_types.data();
    invalid.expected_linux_codes = phone_codes.data();
    MIMUSB_CHECK(mimusb_spec_create_toggle(&invalid, &spec) == MIMUSB_OK);
    exercise_shared_control_state(spec, phone_usages[0]);
    mimusb_spec_release(spec);
}

} // namespace

void test_profiles_usage_controls() { test_usage_control_semantics(); }
