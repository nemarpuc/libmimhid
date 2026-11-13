// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The gamepad profile (HID Usage Tables: Generic Desktop Game Pad): buttons, axes and a hat
 * switch. Its Spec factory.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

namespace {

bool controller_axis_usage(const mimusb_axis_role role, std::uint16_t* usage_page,
                           std::uint16_t* usage) noexcept {
    if (usage_page == nullptr || usage == nullptr) {
        return false;
    }
    *usage_page = mimusb::hid::usage::page_generic_desktop;
    switch (role) {
    case MIMUSB_AXIS_X:
        *usage = mimusb::hid::usage::x;
        return true;
    case MIMUSB_AXIS_Y:
        *usage = mimusb::hid::usage::y;
        return true;
    case MIMUSB_AXIS_Z:
        *usage = mimusb::hid::usage::z;
        return true;
    case MIMUSB_AXIS_RX:
        *usage = mimusb::hid::usage::rx;
        return true;
    case MIMUSB_AXIS_RY:
        *usage = mimusb::hid::usage::ry;
        return true;
    case MIMUSB_AXIS_RZ:
        *usage = mimusb::hid::usage::rz;
        return true;
    case MIMUSB_AXIS_SLIDER:
        *usage = mimusb::hid::usage::slider;
        return true;
    case MIMUSB_AXIS_DIAL:
        *usage = mimusb::hid::usage::dial;
        return true;
    case MIMUSB_AXIS_WHEEL:
        *usage = mimusb::hid::usage::wheel;
        return true;
    case MIMUSB_AXIS_SIMULATION_RUDDER:
        *usage_page = mimusb::hid::usage::page_simulation;
        *usage = mimusb::hid::usage::simulation_rudder;
        return true;
    case MIMUSB_AXIS_SIMULATION_THROTTLE:
        *usage_page = mimusb::hid::usage::page_simulation;
        *usage = mimusb::hid::usage::simulation_throttle;
        return true;
    case MIMUSB_AXIS_SIMULATION_ACCELERATOR:
        *usage_page = mimusb::hid::usage::page_simulation;
        *usage = mimusb::hid::usage::simulation_accelerator;
        return true;
    case MIMUSB_AXIS_SIMULATION_BRAKE:
        *usage_page = mimusb::hid::usage::page_simulation;
        *usage = mimusb::hid::usage::simulation_brake;
        return true;
    case MIMUSB_AXIS_SIMULATION_STEERING:
        *usage_page = mimusb::hid::usage::page_simulation;
        *usage = mimusb::hid::usage::simulation_steering;
        return true;
    default:
        return false;
    }
}

bool controller_axis_domain_matches_usage(const mimusb_gamepad_axis_option& axis) noexcept {
    const auto& value = axis.value;
    switch (axis.role) {
    case MIMUSB_AXIS_SIMULATION_ACCELERATOR:
    case MIMUSB_AXIS_SIMULATION_BRAKE:
    case MIMUSB_AXIS_SIMULATION_THROTTLE:
        return value.logical_minimum == 0 && value.logical_maximum > 0 && axis.neutral_value == 0;
    case MIMUSB_AXIS_SIMULATION_STEERING:
    case MIMUSB_AXIS_SIMULATION_RUDDER:
        return value.logical_minimum < 0 && value.logical_maximum > 0 && axis.neutral_value == 0;
    default:
        return true;
    }
}

} // namespace

extern "C" {

constexpr mimusb_physical_properties kHatDegrees{1U, 0, 315, 0, 0x14U};

static mimusb_result create_controller_spec(const mimusb_gamepad_options* options,
                                            mimusb_spec** out_spec) {
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "gamepad_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U || !validate_report_id(options->report_id, "gamepad.report_id") ||
        options->axes == nullptr || options->axis_count < 2U || options->button_count == 0U ||
        options->button_count > 65535U || options->button_usage_minimum == 0U ||
        (options->dpad_representation != MIMUSB_DPAD_NONE &&
         options->dpad_representation != MIMUSB_DPAD_HAT &&
         options->dpad_representation != MIMUSB_DPAD_BUTTONS)) {
        set_error(MIMUSB_ERR_PARAM, "gamepad",
                  "A gamepad requires explicit axes, at least one button, and valid flags.");
        return MIMUSB_ERR_PARAM;
    }
    if (options->axis_count > std::numeric_limits<std::uint16_t>::max()) {
        set_error(MIMUSB_ERR_OVERFLOW, "gamepad.axis_count",
                  "The generated layout cannot index more than 65535 controller axes.");
        return MIMUSB_ERR_OVERFLOW;
    }
    if (static_cast<std::uint32_t>(options->button_usage_minimum) + options->button_count - 1U >
        std::numeric_limits<std::uint16_t>::max()) {
        set_error(MIMUSB_ERR_OVERFLOW, "gamepad.button_usage_minimum",
                  "The declared button Usage range exceeds the 16-bit Usage value space.");
        return MIMUSB_ERR_OVERFLOW;
    }
    if (options->dpad_representation == MIMUSB_DPAD_BUTTONS &&
        options->button_count >
            static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) - 3U) {
        set_error(MIMUSB_ERR_OVERFLOW, "gamepad.button_count",
                  "Four D-pad fields must fit the layout's 16-bit field-instance space.");
        return MIMUSB_ERR_OVERFLOW;
    }
    bool has_x = false;
    bool has_y = false;
    mimusb::detail::GamepadConfig config{};
    config.options = *options;
    for (std::size_t index = 0; index < options->axis_count; ++index) {
        std::uint16_t expected_page = 0U;
        std::uint16_t expected_usage = 0U;
        if (!controller_axis_usage(options->axes[index].role, &expected_page, &expected_usage) ||
            options->axes[index].usage_page != expected_page ||
            options->axes[index].usage != expected_usage) {
            set_error(MIMUSB_ERR_PARAM, "gamepad.axes.role",
                      "Each axis role must match its audited HUT 1.7 Usage Page and Usage.");
            return MIMUSB_ERR_PARAM;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (options->axes[previous].role == options->axes[index].role) {
                set_error(MIMUSB_ERR_PARAM, "gamepad.axes.role",
                          "Each controller axis role may be declared only once.");
                return MIMUSB_ERR_PARAM;
            }
        }
        if (!validate_field(options->axes[index].value, "gamepad.axes.value") ||
            options->axes[index].neutral_value < options->axes[index].value.logical_minimum ||
            options->axes[index].neutral_value > options->axes[index].value.logical_maximum ||
            options->axes[index].usage_page == 0U || options->axes[index].usage == 0U ||
            options->axes[index].expected_linux_code == nullptr ||
            options->axes[index].expected_linux_code[0] == '\0') {
            set_error(MIMUSB_ERR_UNSET_FIELD, "gamepad.axes",
                      "Every axis requires Usage, range, width, and Linux code.");
            return MIMUSB_ERR_UNSET_FIELD;
        }
        if (!controller_axis_domain_matches_usage(options->axes[index])) {
            set_error(MIMUSB_ERR_PARAM, "gamepad.axes.value",
                      "Simulation Accelerator, Brake, and Throttle require a zero-to-positive "
                      "range with zero neutral; Steering and Rudder require a signed range "
                      "around zero with zero neutral.");
            return MIMUSB_ERR_PARAM;
        }
        has_x = has_x || options->axes[index].role == MIMUSB_AXIS_X;
        has_y = has_y || options->axes[index].role == MIMUSB_AXIS_Y;
        mimusb::detail::GamepadAxisConfig axis{};
        axis.axis = options->axes[index];
        axis.expected_linux_code = options->axes[index].expected_linux_code;
        axis.axis.expected_linux_code = nullptr;
        config.axes.push_back(std::move(axis));
    }
    if (!has_x || !has_y) {
        set_error(MIMUSB_ERR_PARAM, "gamepad.axes",
                  "The gamepad requires explicitly declared X and Y axes.");
        return MIMUSB_ERR_PARAM;
    }
    if (options->dpad_representation == MIMUSB_DPAD_HAT) {
        if (options->hat_logical_minimum != 0 || options->hat_logical_maximum != 7 ||
            options->hat_bit_width != 4U ||
            !has_null_encoding(NullEncodingRange{options->hat_logical_minimum,
                                                 options->hat_logical_maximum,
                                                 options->hat_bit_width})) {
            set_error(MIMUSB_ERR_PARAM, "gamepad.hat",
                      "The Hat must explicitly select logical 0..7 "
                      "in four bits and leave an encoding for Null State.");
            return MIMUSB_ERR_PARAM;
        }
    } else if (options->hat_logical_minimum != 0 || options->hat_logical_maximum != 0 ||
               options->hat_bit_width != 0U) {
        set_error(MIMUSB_ERR_PARAM, "gamepad.hat",
                  "Hat range fields must be canonical zero unless the D-pad is a Hat Switch.");
        return MIMUSB_ERR_PARAM;
    }
    config.options.axes = nullptr;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kGamepadBatteryNames)) {
        return MIMUSB_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{MIMUSB_PROFILE_GAMEPAD}, std::move(config), battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(mimusb::hid::usage::page_generic_desktop,
                                           mimusb::hid::usage::gamepad) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.variable_range(mimusb::hid::usage::page_button,
                                        options->button_usage_minimum,
                                        static_cast<std::uint16_t>(options->button_usage_minimum +
                                                                   options->button_count - 1U),
                                        1U, FieldSemantic::buttons)) {
                return false;
            }
            pad_report(builder);
            if (options->dpad_representation == MIMUSB_DPAD_HAT &&
                !builder.variable(mimusb::hid::usage::page_generic_desktop,
                                  mimusb::hid::usage::hat_switch, options->hat_logical_minimum,
                                  options->hat_logical_maximum,
                                  static_cast<std::uint8_t>(options->hat_bit_width), false, true,
                                  FieldSemantic::hat, 0U, false, &kHatDegrees)) {
                return false;
            }
            if (options->dpad_representation == MIMUSB_DPAD_BUTTONS) {
                constexpr std::uint16_t usages[] = {
                    mimusb::hid::usage::dpad_up, mimusb::hid::usage::dpad_down,
                    mimusb::hid::usage::dpad_right, mimusb::hid::usage::dpad_left};
                for (std::size_t index = 0U; index < 4U; ++index) {
                    if (!builder.variable(mimusb::hid::usage::page_generic_desktop, usages[index],
                                          0, 1, 1U, false, false, FieldSemantic::buttons,
                                          static_cast<std::uint16_t>(options->button_count + index),
                                          true)) {
                        return false;
                    }
                }
            }
            // The Hat Switch and the raw D-pad OOC bits above are 4 bits wide
            // and are not byte-aligned on their own; without realigning here,
            // an axis width in HUT 1.7's full 1-32 bit range (see LIMITS.md)
            // could start at a nonzero bit offset and cross a fifth byte,
            // which HID 1.11 section 8.4 forbids for a single field.
            pad_report(builder);
            for (std::size_t index = 0; index < options->axis_count; ++index) {
                const mimusb_gamepad_axis_option& axis = options->axes[index];
                if (!builder.variable(
                        axis.usage_page, axis.usage, axis.value.logical_minimum,
                        axis.value.logical_maximum, static_cast<std::uint8_t>(axis.value.bit_width),
                        false, false, FieldSemantic::gamepad_axis,
                        static_cast<std::uint16_t>(index), false, &axis.value.physical)) {
                    return false;
                }
                // Each axis has its own caller-declared width (1-32 bits, see LIMITS.md). A
                // width that is not a multiple of eight leaves the next axis at a nonzero bit
                // offset; realigning after each axis keeps that drift from ever compounding into
                // a fifth-byte span two or more axes in, regardless of the declared width.
                pad_report(builder);
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

static mimusb_result mimusb_spec_create_gamepad_impl(const mimusb_gamepad_options* options,
                                                     mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    return create_controller_spec(options, out_spec);
}

// set_error() keeps the field-name pointer without copying it (see
// error_detail.cpp), so every name is a string literal.

mimusb_result MIMUSB_CALL mimusb_spec_create_gamepad(const mimusb_gamepad_options* options,
                                                     mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_gamepad", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_gamepad_impl(options, out_spec); });
    });
}

} // extern "C"
