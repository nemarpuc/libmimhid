// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * One control rule as the library keeps it: the rule as the board has it (its bytes copied) and,
 * for the rules the library writes for a profile interface, what it is for, so that the events and
 * the idle and protocol answers can be made from them. Pure data: no I/O.
 */

#include "mimhid.h"

#include <cstdint>
#include <vector>

namespace mimhid::usb {

struct BoardRule {
    enum class Role : std::uint8_t {
        user,      // a rule of the caller (a Node's or the Device's)
        hid_other, // a rule the library wrote for a profile interface
        hid_set_report,
        hid_set_idle,
        hid_set_protocol,
        hid_get_idle,    // its answer is the idle rate of the interface
        hid_get_protocol // its answer is the protocol of the interface
    };
    mimhid_control_rule rule{}; // `data` is not used: the bytes are below
    std::vector<std::uint8_t> data;
    std::uint16_t capacity{}; // ANSWER: the length the rule was staged with
    Role role{Role::user};
    std::uint8_t iface{}; // the interface of a library rule
};

} // namespace mimhid::usb
