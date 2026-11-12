// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The descriptor builder of the generated profiles: capacity failures and collection balance. */
#include "hid/descriptor_builder.hpp"
#include "hid/validator.hpp"
#include "raw_descriptor_support.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace mimusb_test;

namespace {

void test_generated_collection_balance() {
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(!builder.begin_logical(0x01U, 0x01U));
        MIMUSB_CHECK(!builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(!builder.end_collection());
        MIMUSB_CHECK(!builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        builder.finish();
        MIMUSB_CHECK(!builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(builder.end_collection());
        builder.finish();
        MIMUSB_CHECK(builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(builder.set_report_id(true, 1U));
        MIMUSB_CHECK(!builder.set_report_id(false, 0U));
        MIMUSB_CHECK(!builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(builder.set_report_id(true, 1U));
        MIMUSB_CHECK(!builder.set_report_id(true, 2U));
        MIMUSB_CHECK(!builder.good());
    }
    {
        mimusb::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(!builder.constant_padding(8U));
        MIMUSB_CHECK(!builder.good());
    }
    {
        std::array<std::uint8_t, 64U> descriptor{};
        mimusb::hid::ReportLayout layout{};
        mimusb::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(builder.set_report_id(true, 1U));
        MIMUSB_CHECK(builder.constant_padding(8U));
        MIMUSB_CHECK(builder.end_collection());
        builder.finish();
        MIMUSB_CHECK(builder.good());
        MIMUSB_CHECK(
            mimusb::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMUSB_OK);
        descriptor[7U] = 2U;
        MIMUSB_CHECK(
            mimusb::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMUSB_ERR_INTERNAL);
    }
    {
        std::array<std::uint8_t, 128U> descriptor{};
        mimusb::hid::ReportLayout layout{};
        mimusb::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        MIMUSB_CHECK(builder.begin_application(0x01U, 0x06U));
        MIMUSB_CHECK(builder.set_report_id(false, 0U));
        MIMUSB_CHECK(builder.feature_static_value(0x01U, 0x01U, 0, 1, 1U));
        MIMUSB_CHECK(builder.feature_static_value(0x01U, 0x02U, 0, 1, 32U));
        MIMUSB_CHECK(builder.end_collection());
        builder.finish();
        MIMUSB_CHECK(builder.good());
        // The 32-bit Feature begins at bit one and therefore spans five
        // bytes, which HID 1.11 section 8.4 forbids just as it does for Input.
        MIMUSB_CHECK(
            mimusb::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == MIMUSB_ERR_INTERNAL);
    }
}

} // namespace

void test_descriptor_builder() { test_generated_collection_balance(); }
