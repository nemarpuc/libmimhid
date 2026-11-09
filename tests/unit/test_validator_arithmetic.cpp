// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Raw descriptor validation: signed and unsigned item values, report size arithmetic and the
 * failure paths of the parser. */
#include "hid/validator.hpp"
#include "raw_descriptor_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace mimhid_test;

namespace {

void test_raw_arithmetic_and_failure_paths() {
    {
        // With nonnegative Logical Minimum, HID 1.11 section 6.2.2.7 and
        // hid_parser_global() interpret the Maximum item as unsigned even
        // though the target stores its bit pattern in an s32.
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(),
                          {0x15U, 0x00U, 0x27U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x75U, 0x20U, 0x95U,
                           0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(4U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_OK);

        descriptor[14U] = 0x1FU; // Report Size 31 cannot represent 0xffffffff.
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(),
                          {0x15U, 0xFFU, 0x27U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x75U, 0x20U, 0x95U,
                           0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(4U);
        // Minimum -1 makes the four-byte Maximum signed, so 0xffffffff is -1.
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_OK);
        // A signed 0x80000000 Maximum is below -1.
        descriptor[9U] = 0x00U;
        descriptor[10U] = 0x00U;
        descriptor[11U] = 0x00U;
        descriptor[12U] = 0x80U;
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 33U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x01U, 0xC0U});
        const mimhid_raw_report report = idless_report(5U);
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
        // The second 31-bit value begins at bit 31 and spans five bytes.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 31U, 2U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(8U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 32U, std::numeric_limits<std::uint32_t>::max());
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        // On a 64-bit host the computed wire length exceeds the public u32
        // table field; on a 32-bit host the Size*Count product overflows
        // size_t first. Both paths are an arithmetic overflow, not a generic
        // table mismatch.
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_OVERFLOW);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x91U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_UNSUPPORTED);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // In the selected target's fetch_item(), every tag-15 prefix denotes
        // a long item, not only the canonical FE prefix.
        descriptor.insert(descriptor.end(), {0xF0U, 0x00U, 0x00U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_UNSUPPORTED);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // hid_open_report() rejects a Local delimiter left open at EOF.
        descriptor.insert(descriptor.end(), {0xA9U, 0x01U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // hid_add_field() checks logical ordering before it discards a
        // Constant padding item that has no local Usage.
        descriptor.insert(descriptor.end(), {0x15U, 0x02U, 0x25U, 0x01U, 0x75U, 0x08U, 0x95U, 0x01U,
                                             0x81U, 0x01U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // HID 1.11 section 6.2.2.7 reserves Global tags 12 through 15;
        // C5 00 is a one-byte tag-12 Global item. hid_parser_global() in the
        // exact audited target revision rejects unknown Global tags.
        descriptor.insert(descriptor.end(), {0xC5U, 0x00U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // HID 1.11 section 6.2.2.7 defines tags 3 through 6 as Physical
        // Minimum, Physical Maximum, Unit Exponent, and Unit. The exact target
        // hid_parser_global() accepts each of them.
        descriptor.insert(descriptor.end(),
                          {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x0CU, 0x65U, 0x11U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        const mimhid::hid::ValidationIssue result =
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        MIMHID_CHECK(result.result == MIMHID_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Unit None before the Main item removes the HUT 1.7 section 3.3
        // metadata condition; a transient earlier Unit does not qualify the
        // later Input after Global state is explicitly reset to zero.
        descriptor.insert(descriptor.end(),
                          {0x65U, 0x11U, 0x65U, 0x00U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HUT 1.7 section 3.3 requires Physical Minimum, Physical Maximum,
        // and Unit Exponent when a non-None Unit qualifies any data Main item,
        // including Constant padding with no local Usage.
        descriptor.insert(descriptor.end(), {0x65U, 0x11U, 0x81U, 0x01U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    for (std::uint32_t omitted = 0U; omitted < 3U; ++omitted) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        if (omitted != 0U) {
            descriptor.insert(descriptor.end(), {0x35U, 0x00U});
        }
        if (omitted != 1U) {
            descriptor.insert(descriptor.end(), {0x45U, 0x01U});
        }
        if (omitted != 2U) {
            descriptor.insert(descriptor.end(), {0x55U, 0x0CU});
        }
        descriptor.insert(descriptor.end(), {0x65U, 0x11U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x0CU, 0x65U, 0x11U, 0xA4U, 0x65U,
                           0x00U, 0xB4U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_OK);
    }
    {
        auto descriptor = application_preamble();
        // The target tolerates legacy full-byte sign extension, but HID 1.11
        // section 6.2.2.7 defines the Unit Exponent as a four-bit code.
        descriptor.insert(descriptor.end(), {0x55U, 0xFCU});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const mimhid_raw_report report = idless_report(1U);
        MIMHID_CHECK(
            mimhid::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            MIMHID_ERR_PARAM);
    }
}

} // namespace

void test_validator_arithmetic() { test_raw_arithmetic_and_failure_paths(); }
