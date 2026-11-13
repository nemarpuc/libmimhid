// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * What the factories of the HID Specs share: the checks of the caller's fields, the battery report
 * any generated profile can carry, and build_generated_spec, which runs a profile's recipe twice
 * (counting, then emitting) and checks the result against the HID parser. No USB I/O, and no
 * device-specific state: a Spec is immutable once made.
 */

#include "api/internal.hpp"
#include "classes/hid/hid_class.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace mimusb::spec_build {

using mimusb::detail::set_error;
using mimusb::hid::DescriptorBuilder;
using mimusb::hid::FieldSemantic;

constexpr std::size_t kDescriptorLengthMaximum = 65535U;

inline bool validate_report_id(const mimusb_report_id_option& option, const char* field) noexcept {
    if (const char* reason = mimusb::hid::report_id_option_violation(option)) {
        set_error(MIMUSB_ERR_PARAM, field, reason);
        return false;
    }
    return true;
}

inline bool physical_is_zero(const mimusb_physical_properties& physical) noexcept {
    return physical.enabled == 0U && physical.minimum == 0 && physical.maximum == 0 &&
           physical.unit_exponent == 0 && physical.unit == 0U;
}

inline bool physical_units_match(const mimusb_physical_properties& first,
                                 const mimusb_physical_properties& second) noexcept {
    if (first.enabled != second.enabled) {
        return false;
    }
    return first.enabled == 0U ||
           (first.unit == second.unit && first.unit_exponent == second.unit_exponent);
}

inline bool validate_physical(const mimusb_physical_properties& physical,
                              const char* name) noexcept {
    if (!mimusb::detail::valid_boolean(physical.enabled)) {
        set_error(MIMUSB_ERR_PARAM, name, "physical.enabled must be exactly zero or one.");
        return false;
    }
    if (physical.enabled == 0U) {
        if (!physical_is_zero(physical)) {
            set_error(MIMUSB_ERR_PARAM, name,
                      "Disabled physical properties must be canonical zero.");
            return false;
        }
        return true;
    }
    if (physical.maximum <= physical.minimum || physical.unit_exponent < -8 ||
        physical.unit_exponent > 7 || physical.unit == 0U ||
        !mimusb::hid::unit_is_valid(physical.unit)) {
        set_error(MIMUSB_ERR_PARAM, name,
                  "Enabled physical properties require ordered extents, exponent -8 through 7, and "
                  "a valid nonzero HID Unit.");
        return false;
    }
    return true;
}

inline bool validate_field(const mimusb_integer_field& field, const char* name) noexcept {
    if (!validate_physical(field.physical, name)) {
        return false;
    }
    if (field.bit_width == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, name, "bit_width is required and cannot be zero.");
        return false;
    }
    if (field.bit_width > 32U ||
        !mimusb::hid::value_fits(field.logical_minimum, field.logical_maximum,
                                 static_cast<std::uint8_t>(field.bit_width))) {
        set_error(MIMUSB_ERR_PARAM, name,
                  "The logical range must be ordered and fit the declared width and signedness.");
        return false;
    }
    return true;
}

inline bool field_is_zero(const mimusb_integer_field& field) noexcept {
    return field.logical_minimum == 0 && field.logical_maximum == 0 && field.bit_width == 0U &&
           physical_is_zero(field.physical);
}

struct NullEncodingRange {
    std::int32_t minimum;
    std::int32_t maximum;
    std::uint32_t bit_width;
};

inline bool has_null_encoding(const NullEncodingRange range) noexcept {
    if (range.bit_width == 0U || range.bit_width > 32U) {
        return false;
    }
    std::int64_t representable_minimum = 0;
    std::int64_t representable_maximum = 0;
    if (range.minimum < 0 || range.maximum < 0) {
        representable_minimum = range.bit_width == 32U
                                    ? std::numeric_limits<std::int32_t>::min()
                                    : -(std::int64_t{1} << (range.bit_width - 1U));
        representable_maximum = range.bit_width == 32U
                                    ? std::numeric_limits<std::int32_t>::max()
                                    : (std::int64_t{1} << (range.bit_width - 1U)) - 1;
    } else {
        representable_maximum =
            range.bit_width == 32U
                ? static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())
                : (std::int64_t{1} << range.bit_width) - 1;
    }
    return static_cast<std::int64_t>(range.minimum) > representable_minimum ||
           static_cast<std::int64_t>(range.maximum) < representable_maximum;
}

inline bool validate_option_boolean(const std::uint32_t value, const char* field) noexcept {
    if (!mimusb::detail::valid_boolean(value)) {
        set_error(MIMUSB_ERR_PARAM, field, "The flag must be exactly zero or one.");
        return false;
    }
    return true;
}

inline void pad_report(DescriptorBuilder& builder) {
    const std::size_t padding = (8U - (builder.input_bits() & 7U)) & 7U;
    if (padding != 0U) {
        builder.constant_padding(static_cast<std::uint16_t>(padding));
    }
}

struct GeneratedSpecIdentity {
    mimusb_profile_kind kind;
};

struct BatteryRequest {
    std::uint32_t enabled;
    mimusb_battery_report report;
    mimusb_report_id_option input_report_id;
};

struct BatteryFieldNames {
    const char* enable;
    const char* battery;
    const char* report_id;
    const char* strength;
};

constexpr BatteryFieldNames kKeyboardBatteryNames{"keyboard.enable_battery", "keyboard.battery",
                                                  "keyboard.battery.report_id",
                                                  "keyboard.battery.strength"};
constexpr BatteryFieldNames kMouseBatteryNames{"mouse.enable_battery", "mouse.battery",
                                               "mouse.battery.report_id", "mouse.battery.strength"};
constexpr BatteryFieldNames kToggleBatteryNames{"toggle.enable_battery", "toggle.battery",
                                                "toggle.battery.report_id",
                                                "toggle.battery.strength"};
constexpr BatteryFieldNames kGamepadBatteryNames{"gamepad.enable_battery", "gamepad.battery",
                                                 "gamepad.battery.report_id",
                                                 "gamepad.battery.strength"};
constexpr BatteryFieldNames kTouchscreenBatteryNames{
    "touchscreen.enable_battery", "touchscreen.battery", "touchscreen.battery.report_id",
    "touchscreen.battery.strength"};
constexpr BatteryFieldNames kTouchpadBatteryNames{"touchpad.enable_battery", "touchpad.battery",
                                                  "touchpad.battery.report_id",
                                                  "touchpad.battery.strength"};
constexpr BatteryFieldNames kPenBatteryNames{"pen.enable_battery", "pen.battery",
                                             "pen.battery.report_id", "pen.battery.strength"};
constexpr BatteryFieldNames kDigitizerBatteryNames{"digitizer.enable_battery", "digitizer.battery",
                                                   "digitizer.battery.report_id",
                                                   "digitizer.battery.strength"};

inline bool validate_battery(const BatteryRequest& request,
                             const BatteryFieldNames& names) noexcept {
    if (!validate_option_boolean(request.enabled, names.enable)) {
        return false;
    }
    const mimusb_battery_report& battery = request.report;
    if (request.enabled == 0U) {
        const bool zero =
            battery.report_id.enabled == 0U && battery.report_id.value == 0U &&
            battery.report_id.reserved8[0] == 0U && battery.report_id.reserved8[1] == 0U &&
            battery.report_id.reserved8[2] == 0U && battery.strength.logical_minimum == 0 &&
            battery.strength.logical_maximum == 0 && battery.strength.bit_width == 0U &&
            physical_is_zero(battery.strength.physical);
        if (!zero) {
            set_error(MIMUSB_ERR_PARAM, names.battery,
                      "battery must be all zero while enable_battery is zero.");
        }
        return zero;
    }
    if (!validate_report_id(battery.report_id, names.report_id)) {
        return false;
    }
    if (request.input_report_id.enabled != 1U || battery.report_id.enabled != 1U ||
        battery.report_id.value == request.input_report_id.value) {
        set_error(MIMUSB_ERR_PARAM, names.report_id,
                  "A battery report needs the profile's Report ID and its own, with different "
                  "values.");
        return false;
    }
    if (!validate_field(battery.strength, names.strength) || battery.strength.logical_minimum < 0 ||
        battery.strength.logical_maximum <= battery.strength.logical_minimum) {
        set_error(MIMUSB_ERR_PARAM, names.strength,
                  "Battery Strength requires an ordered nonnegative range that fits its width.");
        return false;
    }
    return true;
}

template <typename Emit>
mimusb_result build_generated_spec(const GeneratedSpecIdentity identity,
                                   mimusb::detail::SpecConfig config, const BatteryRequest& battery,
                                   Emit emit, mimusb_spec** out_spec) {
    if (out_spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_spec", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_spec = nullptr;
    auto spec = std::make_unique<mimusb::detail::HidSpec>();
    const mimusb::hid::BatteryField battery_field{
        battery.report.report_id.value, battery.report.strength.logical_minimum,
        battery.report.strength.logical_maximum,
        static_cast<std::uint8_t>(battery.report.strength.bit_width),
        battery.report.strength.physical};
    const bool has_battery = battery.enabled == 1U;
    DescriptorBuilder counting_builder(nullptr, kDescriptorLengthMaximum, &spec->layout);
    if (has_battery) {
        counting_builder.set_battery(battery_field, &spec->battery_layout);
    }
    const bool counted = emit(counting_builder);
    counting_builder.finish();
    if (!counted || !counting_builder.good()) {
        set_error(MIMUSB_ERR_OVERFLOW, "spec.descriptor",
                  "Descriptor generation failed or exceeded the 16-bit descriptor length field.");
        return MIMUSB_ERR_OVERFLOW;
    }
    spec->requirements = counting_builder.requirements();
    std::vector<std::uint8_t> construction(counting_builder.descriptor_size());
    DescriptorBuilder emitting_builder(construction.data(), construction.size(), nullptr);
    if (has_battery) {
        emitting_builder.set_battery(battery_field, nullptr);
    }
    const bool emitted = emit(emitting_builder);
    emitting_builder.finish();
    if (!emitted || !emitting_builder.good() ||
        emitting_builder.descriptor_size() != construction.size() ||
        emitting_builder.requirements().maximum_fields_per_report !=
            spec->requirements.maximum_fields_per_report ||
        emitting_builder.requirements().maximum_global_stack_depth !=
            spec->requirements.maximum_global_stack_depth ||
        emitting_builder.requirements().maximum_report_size_bits !=
            spec->requirements.maximum_report_size_bits ||
        emitting_builder.requirements().maximum_usages != spec->requirements.maximum_usages ||
        emitting_builder.requirements().maximum_report_data_bits !=
            spec->requirements.maximum_report_data_bits) {
        set_error(MIMUSB_ERR_INTERNAL, "spec.descriptor",
                  "The descriptor counting and emission passes produced different results.");
        return MIMUSB_ERR_INTERNAL;
    }
    const mimusb::hid::ValidationIssue validation =
        mimusb::hid::validate_generated(construction.data(), construction.size(), spec->layout,
                                        has_battery ? &spec->battery_layout : nullptr);
    if (validation.result != MIMUSB_OK) {
        set_error(validation.result, validation.field, validation.reason, 0, 0, 0, 0,
                  static_cast<std::uint32_t>(validation.offset), 0);
        return validation.result;
    }
    spec->kind = identity.kind;
    spec->descriptor = std::move(construction);
    spec->config = std::move(config);
    spec->report_capabilities.push_back(mimusb_report_capability{
        {spec->layout.has_report_id ? 1U : 0U, spec->layout.report_id, {0U, 0U, 0U}},
        static_cast<std::uint32_t>(spec->layout.wire_bytes)});
    if (has_battery) {
        spec->has_battery = true;
        spec->battery = battery.report;
        spec->report_capabilities.push_back(
            mimusb_report_capability{{1U, spec->battery_layout.report_id, {0U, 0U, 0U}},
                                     static_cast<std::uint32_t>(spec->battery_layout.wire_bytes)});
    }
    *out_spec = mimusb::detail::make_hid_spec(std::move(spec));
    mimusb::detail::clear_error();
    return MIMUSB_OK;
}

template <typename Function>
mimusb_result abi_result(const char* field, Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        set_error(MIMUSB_ERR_INTERNAL, field,
                  "Memory allocation failed inside the C ABI boundary.");
        return MIMUSB_ERR_INTERNAL;
    } catch (...) {
        set_error(MIMUSB_ERR_INTERNAL, field,
                  "A C++ exception was contained at the C ABI boundary.");
        return MIMUSB_ERR_INTERNAL;
    }
}

template <typename Function>
mimusb_result abi_spec_factory(mimusb_spec** out_spec, const char* field,
                               Function&& function) noexcept {
    mimusb::detail::clear_error();
    if (out_spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_spec", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_spec = nullptr;
    return abi_result(field, std::forward<Function>(function));
}

// The HID function every HID Spec's options carry (HID 1.11 6.2.1, 4.2, 4.3). Checked before the
// Spec is built; a valid one is stored in the Spec when it was made.
inline mimusb_result check_hid_function(const mimusb_hid_function& f) noexcept {
    if (f.reserved8[0] != 0U || f.reserved8[1] != 0U || f.reserved8[2] != 0U) {
        set_error(MIMUSB_ERR_PARAM, "options.function.reserved8",
                  "The reserved bytes must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if (f.bcd_hid == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "options.function.bcd_hid",
                  "bcd_hid must be explicitly specified (e.g. 0x0111).");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (f.subclass > 1U || f.protocol > 2U || f.country_code > 35U) {
        // HID 1.11 4.2, 4.3 and 6.2.1: subclass 0 or 1; protocol 0 (none), 1 (keyboard) or 2
        // (mouse); country code 0 to 35.
        set_error(MIMUSB_ERR_PARAM, "options.function",
                  "subclass is 0 or 1, protocol 0, 1 or 2 and country_code 0 to 35 (HID 1.11 4.2, "
                  "4.3, 6.2.1).");
        return MIMUSB_ERR_PARAM;
    }
    return MIMUSB_OK;
}

// Makes a HID Spec through `impl` once the options' function checks out, and keeps the function.
template <typename Options, typename Impl>
mimusb_result create_hid_spec(const Options* options, mimusb_spec** out_spec, Impl&& impl) {
    if (options != nullptr && options->struct_size == static_cast<std::uint32_t>(sizeof(Options))) {
        if (const mimusb_result r = check_hid_function(options->function); r != MIMUSB_OK)
            return r;
    }
    const mimusb_result result = impl();
    if (result == MIMUSB_OK && *out_spec != nullptr)
        mimusb::detail::hid_spec(*out_spec)->function = options->function;
    return result;
}

/* mimusb_touchscreen_options and mimusb_touchpad_options share every field below
 * verbatim (same names, same order); only button_count, present solely on
 * mimusb_touchpad_options, is excluded and set separately by each caller. One
 * templated copy keeps the two option structs' shared shape edited in exactly
 * one place instead of two independently hand-maintained functions. A
 * template cannot itself carry C language linkage, so it is defined here,
 * before the extern "C" block below, rather than beside its two callers. */

} // namespace mimusb::spec_build
