// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The keyboard profile (HID Usage Tables: Keyboard/Keypad page): its Spec factory.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

extern "C" {

static mimusb_result mimusb_spec_create_keyboard_impl(const mimusb_keyboard_options* options,
                                                      mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "keyboard_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U || !validate_report_id(options->report_id, "keyboard.report_id")) {
        set_error(MIMUSB_ERR_PARAM, "keyboard.reserved", "reserved must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if (options->usage_minimum < 0x04U || options->usage_maximum < options->usage_minimum) {
        set_error(MIMUSB_ERR_PARAM, "keyboard.usage_range",
                  "The declared non-modifier Usage range must be ordered and exclude the reserved "
                  "keyboard error selectors 0x00 through 0x03.");
        return MIMUSB_ERR_PARAM;
    }
    if (options->usage_minimum <= mimusb::hid::usage::keyboard_right_gui &&
        options->usage_maximum >= mimusb::hid::usage::keyboard_left_control) {
        set_error(MIMUSB_ERR_PARAM, "keyboard.usage_range",
                  "The non-modifier bitmap cannot duplicate modifier Usages 0xE0 through 0xE7.");
        return MIMUSB_ERR_PARAM;
    }

    mimusb::detail::KeyboardConfig config{};
    config.options = *options;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kKeyboardBatteryNames)) {
        return MIMUSB_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{MIMUSB_PROFILE_KEYBOARD}, config, battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(mimusb::hid::usage::page_generic_desktop,
                                           mimusb::hid::usage::keyboard) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.variable_range(
                    mimusb::hid::usage::page_keyboard, mimusb::hid::usage::keyboard_left_control,
                    mimusb::hid::usage::keyboard_right_gui, 1U, FieldSemantic::modifier) ||
                !builder.variable_range(mimusb::hid::usage::page_keyboard, options->usage_minimum,
                                        options->usage_maximum, 1U, FieldSemantic::key_bitmap)) {
                return false;
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

mimusb_result MIMUSB_CALL mimusb_spec_create_keyboard(const mimusb_keyboard_options* options,
                                                      mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_keyboard", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_keyboard_impl(options, out_spec); });
    });
}

} // extern "C"
