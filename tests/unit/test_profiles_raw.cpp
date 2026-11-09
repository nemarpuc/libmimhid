// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Raw profile: Report ID order, the report table rules and the factory bounds checked before
 * anything is allocated. */
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
    const mimhid_raw_options options{
        sizeof(mimhid_raw_options), 0U, descriptor.data(), descriptor.size(), &report, 1U, 0U, 0U};
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

} // namespace

void test_profiles_raw() {
    test_raw_report_id_order();
    test_raw_report_table_report_id_rules();
    test_raw_factory_bounds_before_allocation();
}
