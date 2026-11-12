// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * What one HID Node is, assembled from its Spec (the HID function) and its placement (the
 * interface number, the name and the endpoints of its roles).
 */

#include "mimusb.h"

#include <cstdint>

namespace mimusb::usb {

struct HidNodeConfig {
    std::uint8_t interface_number{};
    std::uint8_t subclass{};
    std::uint8_t protocol{};
    std::uint8_t country_code{};
    std::uint16_t bcd_hid{};
    const char* name{};
    mimusb_usb_endpoint ep_in{};
    mimusb_usb_endpoint ep_out{};
};

} // namespace mimusb::usb
