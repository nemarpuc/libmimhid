// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Builders for hand-written raw report descriptors, shared by the validator tests. Test-only. */
#pragma once

#include "mimusb.h"

#include <cstdint>
#include <vector>

namespace mimusb_test {

inline void append_u32_item(std::vector<std::uint8_t>* descriptor, const std::uint8_t prefix,
                            const std::uint32_t value) {
    descriptor->push_back(prefix);
    descriptor->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

inline std::vector<std::uint8_t> application_preamble() {
    // Usage Page (Generic Desktop), Usage (Keyboard), Application Collection.
    return {0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U};
}

inline void append_globals(std::vector<std::uint8_t>* descriptor, const std::uint8_t report_size,
                           const std::uint32_t report_count) {
    // Logical Minimum 0, Logical Maximum 1, Report Size, Report Count.
    descriptor->insert(descriptor->end(), {0x15U, 0x00U, 0x25U, 0x01U, 0x75U, report_size});
    append_u32_item(descriptor, 0x97U, report_count);
}

inline mimusb_hid_report idless_report(const std::uint32_t wire_length) {
    mimusb_hid_report report{};
    report.wire_length = wire_length;
    return report;
}

} // namespace mimusb_test
