// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The battery report any generated profile can carry: its level.
 */

#include "classes/hid/state_support.hpp"

using namespace mimusb::state_build;

extern "C" {

static mimusb_result mimusb_battery_update_impl(mimusb::detail::HidNode* node,
                                                const std::int32_t strength) {
    mimusb::detail::clear_error();
    if (node == nullptr || node->owner->closed || node->spec == nullptr ||
        !node->spec->has_battery) {
        set_error(MIMUSB_ERR_PARAM, "battery.update", "The node has no battery report.");
        return MIMUSB_ERR_PARAM;
    }
    const mimusb_result link = link_available(node, "battery.update");
    if (link != MIMUSB_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, "battery.update", "The node has one report in flight.");
        return MIMUSB_ERR_BUSY;
    }
    if (!in_range(strength, node->spec->battery.strength)) {
        set_error(MIMUSB_ERR_PARAM, "battery.strength", "The value is outside the declared range.");
        return MIMUSB_ERR_PARAM;
    }
    // The first value is always a change, even when it equals the zero the
    // state starts with.
    const bool changed = !node->battery.has_value || node->battery.strength != strength;
    node->battery.has_value = true;
    node->battery.strength = strength;
    node->battery_dirty = node->battery_dirty || changed;
    return MIMUSB_OK;
}

mimusb_result MIMUSB_CALL mimusb_battery_update(mimusb_node* node, const std::int32_t strength) {
    return abi_state_result(node, "battery.update", [&] {
        return mimusb_battery_update_impl(mimusb::detail::hid_node(node), strength);
    });
}

} // extern "C"
