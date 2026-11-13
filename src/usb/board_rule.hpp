// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * One control rule as the library keeps it: the rule as the board has it (its bytes copied) and,
 * for a rule a function class wrote, which class wrote it and what the class calls it, so that the
 * requests it takes go back to that class. Pure data: no I/O, and no class is known here.
 */

#include "mimusb.h"

#include <cstdint>
#include <vector>

namespace mimusb::detail {
class FunctionClass;
}

namespace mimusb::usb {

struct BoardRule {
    mimusb_control_rule rule{}; // `data` is not used: the bytes are below
    std::vector<std::uint8_t> data;
    std::uint16_t capacity{}; // ANSWER: the length the rule was staged with
    // The class that wrote the rule (null: a rule of the caller, a Node's or the Device's) and the
    // class's own name for it (0 for a rule the class does not need to recognise).
    const mimusb::detail::FunctionClass* owner{};
    std::uint8_t tag{};
    std::uint8_t iface{}; // the interface of a class rule
    // An ANSWER the class changes as the target sets what it answers: its bytes are kept across a
    // new rule table, and go back to `initial` each time the target configures the device.
    bool live{};
    std::vector<std::uint8_t> initial;
};

} // namespace mimusb::usb
