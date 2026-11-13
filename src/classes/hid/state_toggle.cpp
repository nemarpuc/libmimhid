// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The toggle profile's state: which of its usages are pressed.
 */

#include "classes/hid/state_support.hpp"

using namespace mimusb::state_build;

extern "C" {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static mimusb_result mimusb_toggle_impl(mimusb::detail::HidNode* node, const std::uint16_t usage,
                                        const std::uint32_t down) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_boolean(down)) {
        set_error(MIMUSB_ERR_PARAM, "toggle", "down must be exactly zero or one.");
        return MIMUSB_ERR_PARAM;
    }
    if (node == nullptr || node->spec == nullptr || node->owner->closed ||
        node->spec->kind != MIMUSB_PROFILE_TOGGLE) {
        set_error(MIMUSB_ERR_PARAM, "toggle", "The operation requires a live Toggle node.");
        return MIMUSB_ERR_PARAM;
    }
    const mimusb_result link = link_available(node, "toggle");
    if (link != MIMUSB_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, "toggle", "The node has one report in flight.");
        return MIMUSB_ERR_BUSY;
    }
    auto* consumer = state<mimusb::detail::ConsumerState>(node);
    const auto* spec = config<mimusb::detail::ConsumerConfig>(node);
    const auto found = std::find(spec->allowed_usages.begin(), spec->allowed_usages.end(), usage);
    if (found == spec->allowed_usages.end()) {
        set_error(MIMUSB_ERR_PARAM, "toggle.usage", "The Usage is not in the caller allow-list.");
        return MIMUSB_ERR_PARAM;
    }
    if (!transition_storage_matches(consumer->pressed, consumer->transitions, "toggle")) {
        return MIMUSB_ERR_INTERNAL;
    }
    const auto index = static_cast<std::size_t>(found - spec->allowed_usages.begin());
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = consumer->pressed[index] != value;
    if (changed && consumer->transitions[index] != 0U) {
        return pending_transition("toggle");
    }
    if (changed) {
        consumer->pressed[index] = value;
        consumer->transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return MIMUSB_OK;
}

mimusb_result MIMUSB_CALL mimusb_toggle_usage(mimusb_node* node, const std::uint16_t usage,
                                              const std::uint32_t down) {
    return abi_state_result(node, "toggle", [&] {
        return mimusb_toggle_impl(mimusb::detail::hid_node(node), usage, down);
    });
}

} // extern "C"
