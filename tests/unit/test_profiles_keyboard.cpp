// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Keyboard profile: factory option checks, NKRO with concurrent keys, the key bitmap and the
 * press/release transitions a report must carry. */
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimhid.h"
#include "profile_support.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

using namespace mimhid_test;

namespace {

void test_keyboard_nkro_concurrent_keys() {
    mimhid_keyboard_options options = keyboard_options();
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;
    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0xE0U, 1U) == MIMHID_OK);
    node.dirty = false;
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0xE0U, 1U) == MIMHID_OK);
    MIMHID_CHECK(!node.dirty);

    // Twenty simultaneously held non-modifier keys: well beyond the legacy
    // six-slot Array limit. Full NKRO gives every Usage its own bitmap bit,
    // so there is no ErrorRollOver overflow to hit.
    constexpr std::uint16_t k_held_keys = 20U;
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage)
        MIMHID_CHECK(mimhid_keyboard_key(&node, usage, 1U) == MIMHID_OK);

    std::array<std::uint8_t, 64> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(report[0] == 0x01U);
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x04U, 0U) == MIMHID_OK);
    report.fill(0U);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* released = field(spec, mimhid::hid::FieldSemantic::key_bitmap, 0U);
    MIMHID_CHECK(released != nullptr && extract(report.data(), *released) == 0U);
    for (std::uint16_t usage = 0x05U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x66U, 0U) == MIMHID_ERR_PARAM);
    mimhid_spec_release(spec);
}

void test_keyboard_bitmap_and_lifecycle_transitions() {
    mimhid_keyboard_options options = keyboard_options();
    options.usage_maximum = 0x0BU;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&options, &spec) == MIMHID_OK);
    if (spec == nullptr)
        return;

    mimhid_node node{};
    node.spec = spec;
    mimhid::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_ERR_BUSY);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    const auto* key = field(spec, mimhid::hid::FieldSemantic::key_bitmap, 2U);
    MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);

    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_ERR_BUSY);
    report.fill(0xFFU);
    MIMHID_CHECK(mimhid::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 MIMHID_OK);
    MIMHID_CHECK(key != nullptr && extract(report.data(), *key) == 0U);
    mimhid::detail::transfer_complete(&node, MIMHID_ERR_IO, 1);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 1U) == MIMHID_OK);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_ERR_BUSY);
    mimhid::detail::transfer_complete(&node, MIMHID_OK, 0);
    MIMHID_CHECK(mimhid_keyboard_key(&node, 0x06U, 0U) == MIMHID_OK);
    mimhid_spec_release(spec);
}

} // namespace

void test_profiles_keyboard() {
    mimhid_keyboard_options zero{};
    auto* spec = reinterpret_cast<mimhid_spec*>(static_cast<std::uintptr_t>(1U));
    MIMHID_CHECK(mimhid_spec_create_keyboard(&zero, &spec) != MIMHID_OK);
    MIMHID_CHECK(spec == nullptr);
    MIMHID_CHECK(mimhid_spec_create_keyboard(&zero, nullptr) == MIMHID_ERR_PARAM);

    mimhid_keyboard_options invalid_keyboard = keyboard_options();
    invalid_keyboard.report_id.reserved8[0] = 1U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_minimum = 1U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum =
        static_cast<std::uint16_t>(invalid_keyboard.usage_minimum - 1U);
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum = 0xE0U;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&invalid_keyboard, &spec) == MIMHID_ERR_PARAM);

    test_keyboard_nkro_concurrent_keys();
    test_keyboard_bitmap_and_lifecycle_transitions();
}
