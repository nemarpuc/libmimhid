// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Raw profile: Report ID order, the report table rules and the factory bounds checked before
 * anything is allocated. */
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

void test_raw_report_id_order() {
    const std::array<std::uint8_t, 17> valid{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                             0x85U, 0x01U, 0x15U, 0x00U, 0x25U, 0x01U,
                                             0x75U, 0x01U, 0x95U, 0x01U, 0x81U};
    // Complete the Input item data and collection in a separate exact array.
    const std::array<std::uint8_t, 21> accepted{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    (void)valid;
    const mimusb_raw_report report{{1U, 1U, {}}, 2U};
    MIMUSB_CHECK(mimusb::hid::validate_raw(accepted.data(), accepted.size(), &report, 1U).result ==
                 MIMUSB_OK);
    const std::array<std::uint8_t, 19> rejected{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x15U,
                                                0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U,
                                                0x81U, 0x02U, 0x85U, 0x01U, 0xC0U};
    MIMUSB_CHECK(mimusb::hid::validate_raw(rejected.data(), rejected.size(), &report, 1U).result !=
                 MIMUSB_OK);

    const mimusb_raw_report no_id_report{{0U, 0U, {}}, 1U};
    const std::array<std::uint8_t, 17> output_item{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                   0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                   0x95U, 0x01U, 0x91U, 0x02U, 0xC0U};
    MIMUSB_CHECK(
        mimusb::hid::validate_raw(output_item.data(), output_item.size(), &no_id_report, 1U)
            .result == MIMUSB_ERR_UNSUPPORTED);

    const std::array<std::uint8_t, 19> restored_id_zero{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0xA4U, 0x85U, 0x01U, 0x75U,
        0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xB4U, 0x81U, 0x02U, 0xC0U};
    MIMUSB_CHECK(
        mimusb::hid::validate_raw(restored_id_zero.data(), restored_id_zero.size(), &report, 1U)
            .result == MIMUSB_ERR_PARAM);

    const std::array<std::uint8_t, 16> excessive_count{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                       0x75U, 0x01U, 0x97U, 0xFFU, 0xFFU, 0xFFU,
                                                       0xFFU, 0x81U, 0x02U, 0xC0U};
    MIMUSB_CHECK(
        mimusb::hid::validate_raw(excessive_count.data(), excessive_count.size(), &no_id_report, 1U)
            .result == MIMUSB_ERR_PARAM);

    const std::array<std::uint8_t, 19> range_exceeds_size{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U,
        0x25U, 0x03U, 0x75U, 0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    MIMUSB_CHECK(
        mimusb::hid::validate_raw(range_exceeds_size.data(), range_exceeds_size.size(), &report, 1U)
            .result == MIMUSB_ERR_PARAM);
}

void test_raw_report_table_report_id_rules() {
    const std::array<std::uint8_t, 21> descriptor{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                  0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                  0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const auto result_for = [&](const mimusb_raw_report& report) {
        return mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result;
    };
    MIMUSB_CHECK(result_for({{1U, 1U, {}}, 2U}) == MIMUSB_OK);
    // The flag is exactly zero or one.
    MIMUSB_CHECK(result_for({{2U, 1U, {}}, 2U}) == MIMUSB_ERR_PARAM);
    // A declared ID is 1 through 255, and a disabled ID is zero.
    MIMUSB_CHECK(result_for({{1U, 0U, {}}, 2U}) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(result_for({{0U, 1U, {}}, 2U}) == MIMUSB_ERR_PARAM);
    // The reserved bytes are zero.
    MIMUSB_CHECK(result_for({{1U, 1U, {1U, 0U, 0U}}, 2U}) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(result_for({{1U, 1U, {0U, 0U, 1U}}, 2U}) == MIMUSB_ERR_PARAM);
    // The wire length is not zero.
    MIMUSB_CHECK(result_for({{1U, 1U, {}}, 0U}) == MIMUSB_ERR_PARAM);

    // The same ID twice is one report identity listed twice.
    const std::array<mimusb_raw_report, 2> duplicate{mimusb_raw_report{{1U, 1U, {}}, 2U},
                                                     mimusb_raw_report{{1U, 1U, {}}, 2U}};
    MIMUSB_CHECK(mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), duplicate.data(),
                                           duplicate.size())
                     .result == MIMUSB_ERR_PARAM);

    // The raw spec reports the table back through the manifest unchanged.
    const mimusb_raw_report report{{1U, 1U, {}}, 2U};
    const mimusb_raw_options options{
        sizeof(mimusb_raw_options), 0U, descriptor.data(), descriptor.size(), &report, 1U, 0U, 0U,
        {0x0111U, 0U, 0U, 0U, {}}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_raw(&options, &spec) == MIMUSB_OK);
    if (spec != nullptr) {
        mimusb_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        MIMUSB_CHECK(mimusb_spec_manifest(spec, &manifest) == MIMUSB_OK);
        MIMUSB_CHECK(manifest.report_count == 1U);
        if (manifest.report_count == 1U) {
            MIMUSB_CHECK(manifest.reports[0].report_id.enabled == 1U);
            MIMUSB_CHECK(manifest.reports[0].report_id.value == 1U);
            MIMUSB_CHECK(manifest.reports[0].wire_length == 2U);
        }
        mimusb_spec_release(spec);
    }
}

void test_raw_factory_bounds_before_allocation() {
    const std::array<std::uint8_t, 1U> descriptor{0xC0U};
    const mimusb_raw_report report{};
    mimusb_raw_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.function.bcd_hid = 0x0111;
    options.descriptor = descriptor.data();
    options.descriptor_length = descriptor.size();
    options.reports = &report;
    options.report_count = std::numeric_limits<std::size_t>::max();
    mimusb_spec* spec = reinterpret_cast<mimusb_spec*>(std::uintptr_t{1U});
    MIMUSB_CHECK(mimusb_spec_create_raw(&options, &spec) == MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(spec == nullptr);

    options.reports = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_raw(&options, &spec) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(spec == nullptr);
}

} // namespace

void test_profiles_raw() {
    test_raw_report_id_order();
    test_raw_report_table_report_id_rules();
    test_raw_factory_bounds_before_allocation();
}
