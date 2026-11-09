// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Helpers shared by the profile tests: field lookup and extraction in a serialized report,
 * descriptor searches, and the option sets several profile files start from. Test-only. */
#pragma once

#include "api/internal.hpp"
#include "mimhid.h"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace mimhid_test {

inline std::uint64_t extract(const std::uint8_t* report, const mimhid::hid::FieldLayout& field) {
    std::uint64_t value = 0U;
    for (std::uint8_t bit = 0U; bit < field.bit_width; ++bit) {
        const std::size_t position = field.bit_offset + bit;
        value |= static_cast<std::uint64_t>((report[position / 8U] >> (position & 7U)) & 1U) << bit;
    }
    return value;
}

inline const mimhid::hid::FieldLayout* field(const mimhid_spec* spec,
                                             const mimhid::hid::FieldSemantic semantic,
                                             const std::uint16_t instance = 0U) {
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == semantic && candidate.instance == instance)
            return &candidate;
    }
    return nullptr;
}

inline std::size_t count_item(const std::vector<std::uint8_t>& descriptor,
                              const std::uint8_t prefix, const std::uint8_t value) {
    std::size_t count = 0U;
    for (std::size_t index = 1U; index < descriptor.size(); ++index) {
        if (descriptor[index - 1U] == prefix && descriptor[index] == value)
            ++count;
    }
    return count;
}

inline bool has_sequence(const std::vector<std::uint8_t>& descriptor,
                         const std::initializer_list<std::uint8_t> sequence) {
    return std::search(descriptor.begin(), descriptor.end(), sequence.begin(), sequence.end()) !=
           descriptor.end();
}

inline mimhid_keyboard_options keyboard_options() {
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

inline mimhid_touchscreen_options touchscreen_options() {
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

inline mimhid_touchpad_options touchpad_options() {
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

} // namespace mimhid_test
