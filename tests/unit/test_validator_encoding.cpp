// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Raw descriptor validation: required control items, collection usages and the strict item
 * encoding rules of HID 1.11. */
#include "hid/validator.hpp"
#include "raw_descriptor_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace mimusb_test;

namespace {

void test_required_control_items_and_collection_usage() {
    const mimusb_hid_report report = idless_report(1U);
    const auto check_rejected = [&report](const std::vector<std::uint8_t>& descriptor) {
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    };

    // HID 1.11 section 6.2.2.6: every Collection has an associated Usage.
    check_rejected({0x05U, 0x01U, 0xA1U, 0x01U, 0xC0U});

    // HID 1.11 section 6.2.2.2: each non-constant control-data Main item
    // needs Usage Page, Usage, Logical Minimum, Logical Maximum, Report Size,
    // and Report Count. The collection preamble consumes its own local Usage.
    for (const std::vector<std::uint8_t>& globals :
         {// Missing Usage for the Input item.
          std::vector<std::uint8_t>{0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U},
          // Missing explicit Logical Minimum.
          std::vector<std::uint8_t>{0x09U, 0x01U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U},
          // Missing explicit Logical Maximum.
          std::vector<std::uint8_t>{0x09U, 0x01U, 0x15U, 0x00U, 0x75U, 0x01U, 0x95U, 0x01U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), globals.begin(), globals.end());
        descriptor.insert(descriptor.end(), {0x81U, 0x02U, 0xC0U});
        check_rejected(descriptor);
    }

    // A four-byte Usage embeds its Usage Page, but HID 1.11 section 6.2.2.2
    // still lists the Usage Page item itself as required descriptor state.
    const std::vector<std::uint8_t> missing_usage_page{
        0x0BU, 0x06U, 0x00U, 0x01U, 0x00U, 0xA1U, 0x01U, 0x0BU, 0x01U, 0x00U, 0x01U, 0x00U,
        0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    check_rejected(missing_usage_page);
}

void test_strict_hid_encoding_rules() {
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // The specified target expands a short Usage range from the raw Local
        // item values, then hid_add_usage() concatenates the Usage Page. The
        // loop is therefore fffe..ffff (two entries), not a 32-bit wrap.
        descriptor.insert(descriptor.end(), {0x06U, 0xFFU, 0xFFU, 0x1AU, 0xFEU, 0xFFU, 0x2AU, 0xFFU,
                                             0xFFU, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        mimusb::hid::DescriptorRequirements requirements{};
        MIMUSB_CHECK(mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U,
                                               nullptr, &requirements)
                         .result == MIMUSB_OK);
        MIMUSB_CHECK(requirements.maximum_usages == 2U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 gives the short Minimum page 1 and the extended Maximum an
        // embedded page 1. The specified target nevertheless expands its raw
        // item_udata range 1..00010002 before adding pages. Record that exact
        // parser requirement so its 12,288-Usage policy can reject it before
        // registration instead of understating it as two entries.
        descriptor.insert(descriptor.end(),
                          {0x19U, 0x01U, 0x2BU, 0x02U, 0x00U, 0x01U, 0x00U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        mimusb::hid::DescriptorRequirements requirements{};
        MIMUSB_CHECK(mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U,
                                               nullptr, &requirements)
                         .result == MIMUSB_OK);
        MIMUSB_CHECK(requirements.maximum_usages == 65538U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {0x1BU, 0x01U, 0x00U, 0x01U, 0x00U, 0x29U, 0x02U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    for (const std::vector<std::uint8_t>& input :
         {std::vector<std::uint8_t>{0x82U, 0x80U, 0x00U},
          std::vector<std::uint8_t>{0x82U, 0x00U, 0x02U}}) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.push_back(0x09U);
        descriptor.push_back(0x01U);
        descriptor.insert(descriptor.end(), input.begin(), input.end());
        descriptor.push_back(0xC0U);
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0x75U, 0x08U, 0x95U, 0x01U,
                                             0x09U, 0x02U, 0x82U, 0x00U, 0x01U, 0xC0U});
        const mimusb_hid_report report = idless_report(2U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 8U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x82U, 0x00U, 0x01U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_OK);
    }
    for (const std::vector<std::uint8_t>& delimited :
         {std::vector<std::uint8_t>{0xA9U, 0x02U, 0x09U, 0x01U, 0xA9U, 0x00U, 0x81U, 0x02U},
          std::vector<std::uint8_t>{0xA9U, 0x01U, 0x09U, 0x01U, 0xA9U, 0x00U, 0x81U, 0x01U}}) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), delimited.begin(), delimited.end());
        descriptor.push_back(0xC0U);
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }

    MIMUSB_CHECK(mimusb::hid::unit_is_valid(0U));
    MIMUSB_CHECK(mimusb::hid::unit_is_valid(0x11U));
    MIMUSB_CHECK(mimusb::hid::unit_is_valid(0x14U));
    MIMUSB_CHECK(mimusb::hid::unit_is_valid(0x0FU));
    MIMUSB_CHECK(!mimusb::hid::unit_is_valid(0x05U));
    MIMUSB_CHECK(!mimusb::hid::unit_is_valid(0x10000011U));
    for (const std::vector<std::uint8_t>& unit :
         {std::vector<std::uint8_t>{0x65U, 0x05U},
          std::vector<std::uint8_t>{0x67U, 0x11U, 0x00U, 0x00U, 0x10U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), unit.begin(), unit.end());
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x00U, 0x65U, 0x0FU,
                                             0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_OK);
    }

    const auto nested_collection = [](const std::vector<std::uint8_t>& collection) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), {0x09U, 0x01U});
        descriptor.insert(descriptor.end(), collection.begin(), collection.end());
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U, 0xC0U});
        return descriptor;
    };
    for (const std::vector<std::uint8_t>& collection :
         {std::vector<std::uint8_t>{0xA1U, 0x07U},
          std::vector<std::uint8_t>{0xA2U, 0x00U, 0x01U}}) {
        const auto descriptor = nested_collection(collection);
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        const auto descriptor = nested_collection({0xA1U, 0x80U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 section 6.2.2.4 reserves Main tags other than Input,
        // Output, Feature, Collection, and End Collection.
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0xD0U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Designator Index (tag 3) and String Index (tag 7) are defined Local
        // items. The selected target ignores both, which does not change the
        // Input report layout.
        descriptor.insert(descriptor.end(),
                          {0x39U, 0x01U, 0x79U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_OK);

        // Local tag 6 is unassigned by HID 1.11 section 6.2.2.8.
        descriptor.insert(descriptor.end() - 1, {0x69U, 0x00U});
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // bType 3 is reserved by HID 1.11 section 6.2.2.2. The audited Linux
        // parser ignores it, but the strict raw subset must not bless it.
        descriptor.insert(descriptor.end(), {0x0DU, 0x00U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimusb_hid_report report = idless_report(1U);
        MIMUSB_CHECK(
            mimusb::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMUSB_ERR_PARAM);
    }
}

} // namespace

void test_validator_encoding() {
    test_required_control_items_and_collection_usage();
    test_strict_hid_encoding_rules();
}
