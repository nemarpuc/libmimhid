// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Keyboard profile: factory option checks, NKRO with concurrent keys, the key bitmap and the
 * press/release transitions a report must carry. */
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimusb.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

using namespace mimusb_test;

namespace {

void test_keyboard_nkro_concurrent_keys() {
    mimusb_keyboard_options options = keyboard_options();
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;
    mimusb_node node{};
    node.spec = spec;
    mimusb::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0xE0U, 1U) == MIMUSB_OK);
    node.dirty = false;
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0xE0U, 1U) == MIMUSB_OK);
    MIMUSB_CHECK(!node.dirty);

    // Twenty simultaneously held non-modifier keys: well beyond the legacy
    // six-slot Array limit. Full NKRO gives every Usage its own bitmap bit,
    // so there is no ErrorRollOver overflow to hit.
    constexpr std::uint16_t k_held_keys = 20U;
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage)
        MIMUSB_CHECK(mimusb_keyboard_key(&node, usage, 1U) == MIMUSB_OK);

    std::array<std::uint8_t, 64> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(report[0] == 0x01U);
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimusb::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMUSB_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);

    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x04U, 0U) == MIMUSB_OK);
    report.fill(0U);
    MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMUSB_OK);
    const auto* released = field(spec, mimusb::hid::FieldSemantic::key_bitmap, 0U);
    MIMUSB_CHECK(released != nullptr && extract(report.data(), *released) == 0U);
    for (std::uint16_t usage = 0x05U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimusb::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMUSB_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x66U, 0U) == MIMUSB_ERR_PARAM);
    mimusb_spec_release(spec);
}

void test_keyboard_bitmap_and_lifecycle_transitions() {
    mimusb_keyboard_options options = keyboard_options();
    options.usage_maximum = 0x0BU;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&options, &spec) == MIMUSB_OK);
    if (spec == nullptr)
        return;

    mimusb_node node{};
    node.spec = spec;
    mimusb::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);

    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 1U) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 0U) == MIMUSB_ERR_BUSY);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMUSB_OK);
    const auto* key = field(spec, mimusb::hid::FieldSemantic::key_bitmap, 2U);
    MIMUSB_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);

    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 0U) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 1U) == MIMUSB_ERR_BUSY);
    report.fill(0xFFU);
    MIMUSB_CHECK(mimusb::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(key != nullptr && extract(report.data(), *key) == 0U);
    mimusb::detail::transfer_complete(&node, MIMUSB_ERR_IO, 1);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 1U) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 0U) == MIMUSB_ERR_BUSY);
    mimusb::detail::transfer_complete(&node, MIMUSB_OK, 0);
    MIMUSB_CHECK(mimusb_keyboard_key(&node, 0x06U, 0U) == MIMUSB_OK);
    mimusb_spec_release(spec);
}

} // namespace

void test_profiles_keyboard() {
    mimusb_keyboard_options zero{};
    auto* spec = reinterpret_cast<mimusb_spec*>(static_cast<std::uintptr_t>(1U));
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&zero, &spec) != MIMUSB_OK);
    MIMUSB_CHECK(spec == nullptr);
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&zero, nullptr) == MIMUSB_ERR_PARAM);

    mimusb_keyboard_options invalid_keyboard = keyboard_options();
    invalid_keyboard.report_id.reserved8[0] = 1U;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&invalid_keyboard, &spec) == MIMUSB_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_minimum = 1U;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&invalid_keyboard, &spec) == MIMUSB_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum =
        static_cast<std::uint16_t>(invalid_keyboard.usage_minimum - 1U);
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&invalid_keyboard, &spec) == MIMUSB_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum = 0xE0U;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&invalid_keyboard, &spec) == MIMUSB_ERR_PARAM);

    test_keyboard_nkro_concurrent_keys();
    test_keyboard_bitmap_and_lifecycle_transitions();
}
