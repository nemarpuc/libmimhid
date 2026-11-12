// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* HID short item encoding: the smallest size that holds a value, signedness, and a writer that
 * runs out of room leaves the buffer as it was. */
#include "hid/descriptor_builder.hpp"
#include "hid/item_writer.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>

void test_item_writer() {
    {
        std::array<std::uint8_t, 8> bytes{};
        mimusb::hid::ItemWriter writer(bytes.data(), bytes.size());
        MIMUSB_CHECK(writer.logical_minimum(128));
        MIMUSB_CHECK(writer.size() == 3U);
        MIMUSB_CHECK(bytes[0] == 0x16U && bytes[1] == 0x80U && bytes[2] == 0x00U);
    }
    {
        std::array<std::uint8_t, 8> bytes{};
        mimusb::hid::ItemWriter writer(bytes.data(), bytes.size());
        MIMUSB_CHECK(writer.logical_minimum(0));
        MIMUSB_CHECK(writer.logical_maximum(65535, 0));
        MIMUSB_CHECK(writer.size() == 5U);
        MIMUSB_CHECK(bytes[0] == 0x15U && bytes[1] == 0x00U);
        MIMUSB_CHECK(bytes[2] == 0x26U && bytes[3] == 0xFFU && bytes[4] == 0xFFU);
    }
    {
        std::array<std::uint8_t, 4> bytes{};
        mimusb::hid::ItemWriter writer(bytes.data(), bytes.size());
        MIMUSB_CHECK(writer.unit_exponent(-4));
        MIMUSB_CHECK(writer.size() == 2U);
        MIMUSB_CHECK(bytes[0] == 0x55U && bytes[1] == 0x0CU);
    }
    {
        std::array<std::uint8_t, 1U> bytes{0xA5U};
        mimusb::hid::ItemWriter writer(bytes.data(), bytes.size());
        MIMUSB_CHECK(!writer.usage(0x100U));
        MIMUSB_CHECK(writer.size() == 0U);
        MIMUSB_CHECK(bytes[0] == 0xA5U);
        MIMUSB_CHECK(!writer.push());
        MIMUSB_CHECK(writer.size() == 0U);
        MIMUSB_CHECK(bytes[0] == 0xA5U);
    }
    MIMUSB_CHECK(mimusb::hid::value_fits(0, 7, 4U));
    MIMUSB_CHECK(!mimusb::hid::value_fits(-9, 7, 4U));
    // The generated-profile API carries signed int32 extrema; raw HID item
    // signedness is tested separately through validate_raw().
    MIMUSB_CHECK(!mimusb::hid::value_fits(0, -1, 32U));
}
