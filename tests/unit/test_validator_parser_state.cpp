// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Raw descriptor validation against the target parser's state machine: what a Main item needs
 * from the Global and Local state, local state reset, and delimiters. */
#include "hid/validator.hpp"
#include "raw_descriptor_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace mimhid_test;

namespace {

void test_target_neutral_raw_requirements() {
    // The state transitions under test are from the exact target source:
    // https://android.googlesource.com/kernel/common/+/35556bed836f8dc07ac55f69c8d17dce3e7f0e25/drivers/hid/hid-core.c
    // hid_parser_global, hid_parser_local, hid_add_field, hid_register_field.
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Usage Minimum 0 through Usage Maximum 20000 is 20001 Local Usages.
        descriptor.insert(descriptor.end(),
                          {0x1AU, 0x00U, 0x00U, 0x2AU, 0x20U, 0x4EU, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 1U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 20001U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // A nearly full 32-bit Usage range requires a 64-bit count and must
        // not be expanded into individual entries by validation.
        descriptor.insert(descriptor.end(), {0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x2BU, 0xFEU, 0xFFU,
                                             0xFFU, 0xFFU, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 1U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages ==
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
        MIMHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        // 524280 one-bit values occupy exactly 65535 bytes. Validation must
        // use checked arithmetic, not a Report Count-sized loop.
        append_globals(&descriptor, 1U, 524280U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(65535U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 1U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 524280U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 524280U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        for (std::uint32_t field = 0U; field < 257U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.push_back(0xC0U);
        const mimhid_raw_report report = idless_report(33U);
        std::uint8_t trailing_mask = 0U;
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, &trailing_mask, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 257U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 1U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 257U);
        MIMHID_CHECK(trailing_mask == 0x01U);
    }
    {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), {0x85U, 0x01U});
        append_globals(&descriptor, 1U, 1U);
        for (std::uint32_t field = 0U; field < 2U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.insert(descriptor.end(), {0x85U, 0x02U});
        for (std::uint32_t field = 0U; field < 3U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.push_back(0xC0U);
        const std::array<mimhid_raw_report, 2U> reports{mimhid_raw_report{{1U, 1U, {}}, 2U},
                                                        mimhid_raw_report{{1U, 2U, {}}, 2U}};
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), reports.data(),
                                      reports.size(), nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 3U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 1U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 3U);
    }
    {
        auto descriptor = application_preamble();
        // Global Push/Pop are zero-byte short items. Five balanced levels are
        // valid descriptor state for a target-neutral Spec, but exceed the
        // selected target revision's HID_GLOBAL_STACK_SIZE policy of four.
        descriptor.insert(descriptor.end(),
                          {0xA4U, 0xA4U, 0xA4U, 0xA4U, 0xA4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 1U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 5U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 1U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        // The exact target rejects a Report Size declaration above 256 even
        // if a later Global item overwrites it before Input. A target-neutral
        // Spec records the declaration so Node registration can apply policy.
        descriptor.insert(descriptor.end(), {0x77U, 0x01U, 0x01U, 0x00U, 0x00U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 257U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // Local Usage items are parsed even after the final Collection. The
        // exact target rejects the 12,289th entry; requirements must not only
        // sample Local state when a Main item appears.
        descriptor.insert(descriptor.end(), 12289U, 0x08U);
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_usages == 12289U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // The audited target's inclusive u32 loop can wrap for some Local
        // states when the raw Maximum is UINT32_MAX. The validated raw subset
        // conservatively rejects every such Maximum as project policy.
        descriptor.insert(descriptor.end(), {0x1BU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x2BU, 0xFFU, 0xFFU,
                                             0xFFU, 0xFFU, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_UNSUPPORTED);
    }
}

void test_local_state_and_delimiters() {
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 section 6.2.2.8 does not state that Usage Maximum without
        // Usage Minimum is invalid or define a default minimum. The exact
        // target zero-initializes local state, so a lone Maximum of 3 expands
        // the raw range 0 through 3. Preserve that implementation observation
        // instead of inventing a stricter conformance rule.
        descriptor.insert(descriptor.end(), {0x29U, 0x03U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_usages == 4U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {// Branch zero Usage.
                           0x09U, 0x01U,
                           // First delimiter branch: five more Usages.
                           0xA9U, 0x01U, 0x19U, 0x0AU, 0x29U, 0x0EU, 0xA9U, 0x00U,
                           // Second branch is ignored by the audited parser.
                           0xA9U, 0x01U, 0x09U, 0x7FU, 0xA9U, 0x00U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 1U);
        MIMHID_CHECK(requirements.maximum_usages == 6U);
    }
    {
        // The five-Usage range belongs to the Collection Main item and is
        // reset afterwards. The following constant Input is padding, not a
        // registered field.
        std::vector<std::uint8_t> descriptor{0x05U, 0x01U, 0x19U, 0x01U,
                                             0x29U, 0x05U, 0xA1U, 0x01U};
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x81U, 0x01U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_OK);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 0U);
        MIMHID_CHECK(requirements.maximum_usages == 5U);
    }
    for (const std::vector<std::uint8_t>& suffix :
         {std::vector<std::uint8_t>{0xA9U, 0x01U, 0xA9U, 0x01U},
          std::vector<std::uint8_t>{0xA9U, 0x00U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), suffix.begin(), suffix.end());
        descriptor.push_back(0xC0U);
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{99U, 99U, 99U, 99U, 99U};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 0U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 0U);
        MIMHID_CHECK(requirements.maximum_usages == 0U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 0U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x19U, 0x05U, 0x29U, 0x04U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        mimhid::hid::DescriptorRequirements requirements{99U, 99U, 99U, 99U, 99U};
        const mimhid::hid::ValidationIssue result = mimhid::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
        MIMHID_CHECK(requirements.maximum_fields_per_report == 0U);
        MIMHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        MIMHID_CHECK(requirements.maximum_report_size_bits == 0U);
        MIMHID_CHECK(requirements.maximum_usages == 0U);
        MIMHID_CHECK(requirements.maximum_report_data_bits == 0U);
    }
    {
        // HID 1.11 section 6.2.2.8 forbids delimiter aliases for an
        // Application Collection Usage.
        const std::vector<std::uint8_t> descriptor{0x05U, 0x01U, 0xA9U, 0x01U, 0x09U, 0x06U,
                                                   0xA9U, 0x00U, 0xA1U, 0x01U, 0xC0U};
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    {
        // The same section forbids delimiter aliases for Array items.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 8U, 1U);
        descriptor.insert(descriptor.end(),
                          {0xA9U, 0x01U, 0x09U, 0x04U, 0xA9U, 0x00U, 0x81U, 0x00U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    {
        // A delimiter set brackets alternatives and must close before the
        // consuming Main item.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0xA9U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
}

} // namespace

void test_validator_parser_state() {
    test_target_neutral_raw_requirements();
    test_local_state_and_delimiters();
}
