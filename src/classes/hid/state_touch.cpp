// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The touchscreen and touchpad profiles' state: contacts (in multi-packet frames when the Spec
 * has them) and the touchpad buttons.
 */

#include "classes/hid/state_support.hpp"

using namespace mimusb::state_build;

extern "C" {

struct TouchExtra {
    std::int32_t pressure;
    std::int32_t width;
    std::int32_t height;
    std::int32_t azimuth;
};

// The contact_id/down/x/y order is the fixed mimusb_touchscreen_contact and
// mimusb_touchpad_contact ABI order and cannot be changed to avoid this warning.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static mimusb_result touch_contact_impl(mimusb::detail::HidNode* node,
                                        const mimusb_profile_kind kind,
                                        const std::uint32_t contact_id, const std::uint32_t down,
                                        const std::int32_t x, const std::int32_t y,
                                        const TouchExtra* extra) {
    mimusb::detail::clear_error();
    const bool right_kind = node != nullptr && node->spec != nullptr && node->spec->kind == kind;
    if (!right_kind || node->owner->closed || !mimusb::detail::valid_boolean(down)) {
        set_error(MIMUSB_ERR_PARAM, "touch", "A live touch node and boolean down are required.");
        return MIMUSB_ERR_PARAM;
    }
    const mimusb_result link = link_available(node, "touch");
    if (link != MIMUSB_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, "touch", "A touch report is in flight.");
        return MIMUSB_ERR_BUSY;
    }
    const auto* spec = config<mimusb::detail::TouchConfig>(node);
    auto* touch = state<mimusb::detail::TouchState>(node);
    if (touch->packet_cursor != 0U) {
        set_error(MIMUSB_ERR_BUSY, "touch",
                  "The current multi-packet frame must finish before state changes.");
        return MIMUSB_ERR_BUSY;
    }
    auto active =
        std::find_if(touch->contacts.begin(), touch->contacts.end(), [=](const auto& entry) {
            return entry.phase == ContactPhase::down && entry.sample.contact_id == contact_id;
        });
    if (down == 0U) {
        if (active == touch->contacts.end()) {
            set_error(MIMUSB_ERR_PARAM,
                      kind == MIMUSB_PROFILE_TOUCHSCREEN ? "touchscreen.contact_id"
                                                         : "touchpad.contact_id",
                      "Lift requires an active Contact ID.");
            return MIMUSB_ERR_PARAM;
        }
        if (!in_range(x, spec->fields.x) || !in_range(y, spec->fields.y)) {
            set_error(MIMUSB_ERR_PARAM, "touch", "The final coordinates are outside the spec.");
            return MIMUSB_ERR_PARAM;
        }
        if (active->lifecycle_transition_pending) {
            return pending_transition("touch");
        }
        active->phase = ContactPhase::up;
        active->sample.x = x;
        active->sample.y = y;
        active->sample.pressure = 0;
        active->lifecycle_transition_pending = true;
        touch->packet_cursor = 0U;
        node->dirty = true;
        return MIMUSB_OK;
    }
    mimusb::detail::TouchContact contact{};
    contact.contact_id = contact_id;
    contact.x = x;
    contact.y = y;
    if (extra != nullptr) {
        contact.pressure = extra->pressure;
        contact.width = extra->width;
        contact.height = extra->height;
        contact.azimuth = extra->azimuth;
    }
    if (!validate_contact(*spec, contact, "touch"))
        return MIMUSB_ERR_PARAM;
    if (active != touch->contacts.end()) {
        // The Contact ID is already down: this call moves it, matching the
        // prior touch_move behavior exactly.
        const bool changed =
            active->sample.x != contact.x || active->sample.y != contact.y ||
            active->sample.pressure != contact.pressure || active->sample.width != contact.width ||
            active->sample.height != contact.height || active->sample.azimuth != contact.azimuth;
        active->sample = contact;
        touch->packet_cursor = 0U;
        node->dirty = node->dirty || changed;
        return MIMUSB_OK;
    }
    for (const auto& entry : touch->contacts) {
        if (entry.phase != ContactPhase::none && entry.sample.contact_id == contact_id) {
            set_error(MIMUSB_ERR_BUSY,
                      kind == MIMUSB_PROFILE_TOUCHSCREEN ? "touchscreen.contact_id"
                                                         : "touchpad.contact_id",
                      "The Contact ID is already active.");
            return MIMUSB_ERR_BUSY;
        }
    }
    auto found = std::find_if(touch->contacts.begin(), touch->contacts.end(),
                              [](const auto& entry) { return entry.phase == ContactPhase::none; });
    if (found == touch->contacts.end() ||
        std::count_if(touch->contacts.begin(), touch->contacts.end(), [](const auto& entry) {
            return entry.phase != ContactPhase::none;
        }) >= static_cast<std::ptrdiff_t>(spec->fields.maximum_contacts)) {
        set_error(MIMUSB_ERR_OVERFLOW,
                  kind == MIMUSB_PROFILE_TOUCHSCREEN ? "touchscreen.maximum_contacts"
                                                     : "touchpad.maximum_contacts",
                  "No declared contact slot remains.");
        return MIMUSB_ERR_OVERFLOW;
    }
    found->phase = ContactPhase::down;
    found->sample = contact;
    found->lifecycle_transition_pending = true;
    touch->packet_cursor = 0U;
    node->dirty = true;
    return MIMUSB_OK;
}

static mimusb_result mimusb_touchpad_button_impl(mimusb::detail::HidNode* node,
                                                 const std::uint32_t button,
                                                 const std::uint32_t down) {
    mimusb::detail::clear_error();
    const mimusb_result check = usable(node, MIMUSB_PROFILE_TOUCHPAD, "touchpad.button");
    if (check != MIMUSB_OK)
        return check;
    auto* touch = state<mimusb::detail::TouchState>(node);
    if (touch->packet_cursor != 0U) {
        set_error(MIMUSB_ERR_BUSY, "touchpad.button",
                  "The current multi-packet frame must finish before state changes.");
        return MIMUSB_ERR_BUSY;
    }
    if (button == 0U || button > touch->buttons.size() || !mimusb::detail::valid_boolean(down)) {
        set_error(MIMUSB_ERR_PARAM, "touchpad.button",
                  "Button is one-based and down must be zero or one.");
        return MIMUSB_ERR_PARAM;
    }
    if (!transition_storage_matches(touch->buttons, touch->button_transitions, "touchpad.button")) {
        return MIMUSB_ERR_INTERNAL;
    }
    const std::size_t index = button - 1U;
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = touch->buttons[index] != value;
    if (changed && touch->button_transitions[index] != 0U) {
        return pending_transition("touchpad.button");
    }
    if (changed) {
        touch->buttons[index] = value;
        touch->button_transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return MIMUSB_OK;
}

mimusb_result MIMUSB_CALL mimusb_touchscreen_contact(mimusb_node* node,
                                                     const std::uint32_t contact_id,
                                                     const std::uint32_t down, const std::int32_t x,
                                                     const std::int32_t y,
                                                     const mimusb_touchscreen_extra* extra) {
    return abi_state_result(node, "touchscreen.contact", [&] {
        const TouchExtra values = extra == nullptr ? TouchExtra{}
                                                   : TouchExtra{extra->pressure, extra->width,
                                                                extra->height, extra->azimuth};
        return touch_contact_impl(mimusb::detail::hid_node(node), MIMUSB_PROFILE_TOUCHSCREEN,
                                  contact_id, down, x, y, extra == nullptr ? nullptr : &values);
    });
}

mimusb_result MIMUSB_CALL mimusb_touchpad_contact(mimusb_node* node, const std::uint32_t contact_id,
                                                  const std::uint32_t down, const std::int32_t x,
                                                  const std::int32_t y,
                                                  const mimusb_touchpad_extra* extra) {
    return abi_state_result(node, "touchpad.contact", [&] {
        const TouchExtra values = extra == nullptr ? TouchExtra{}
                                                   : TouchExtra{extra->pressure, extra->width,
                                                                extra->height, extra->azimuth};
        return touch_contact_impl(mimusb::detail::hid_node(node), MIMUSB_PROFILE_TOUCHPAD,
                                  contact_id, down, x, y, extra == nullptr ? nullptr : &values);
    });
}

mimusb_result MIMUSB_CALL mimusb_touchpad_button(mimusb_node* node, const std::uint32_t button,
                                                 const std::uint32_t down) {
    return abi_state_result(node, "touchpad.button", [&] {
        return mimusb_touchpad_button_impl(mimusb::detail::hid_node(node), button, down);
    });
}

} // extern "C"
