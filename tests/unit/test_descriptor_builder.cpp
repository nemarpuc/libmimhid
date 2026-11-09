// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The descriptor builder of the generated profiles: capacity failures and collection balance. */
#include "hid/descriptor_builder.hpp"
#include "hid/validator.hpp"
#include "raw_descriptor_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace mimhid_test;

namespace {

void test_generated_collection_balance() {
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(!builder.begin_logical(0x01U, 0x01U));
        MIMHID_CHECK(!builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(!builder.end_collection());
        MIMHID_CHECK(!builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        builder.finish();
        MIMHID_CHECK(!builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(builder.end_collection());
        builder.finish();
        MIMHID_CHECK(builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(builder.set_report_id(true, 1U));
        MIMHID_CHECK(!builder.set_report_id(false, 0U));
        MIMHID_CHECK(!builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(builder.set_report_id(true, 1U));
        MIMHID_CHECK(!builder.set_report_id(true, 2U));
        MIMHID_CHECK(!builder.good());
    }
    {
        mimhid::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(!builder.constant_padding(8U));
        MIMHID_CHECK(!builder.good());
    }
    {
        std::array<std::uint8_t, 64U> descriptor{};
        mimhid::hid::ReportLayout layout{};
        mimhid::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(builder.set_report_id(true, 1U));
        MIMHID_CHECK(builder.constant_padding(8U));
        MIMHID_CHECK(builder.end_collection());
        builder.finish();
        MIMHID_CHECK(builder.good());
        MIMHID_CHECK(
            mimhid::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMHID_OK);
        descriptor[7U] = 2U;
        MIMHID_CHECK(
            mimhid::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMHID_ERR_INTERNAL);
    }
    {
        std::array<std::uint8_t, 128U> descriptor{};
        mimhid::hid::ReportLayout layout{};
        mimhid::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        MIMHID_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMHID_CHECK(builder.set_report_id(false, 0U));
        MIMHID_CHECK(builder.feature_static_value(0x01U, 0x01U, 0, 1, 1U));
        MIMHID_CHECK(builder.feature_static_value(0x01U, 0x02U, 0, 1, 32U));
        MIMHID_CHECK(builder.end_collection());
        builder.finish();
        MIMHID_CHECK(builder.good());
        // The 32-bit Feature begins at bit one and therefore spans five
        // bytes, which HID 1.11 section 8.4 forbids just as it does for Input.
        MIMHID_CHECK(
            mimhid::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMHID_ERR_INTERNAL);
    }
}

} // namespace

void test_descriptor_builder() { test_generated_collection_balance(); }
