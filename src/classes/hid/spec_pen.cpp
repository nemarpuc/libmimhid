// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The pen and digitizer profiles (HID Usage Tables: Digitizers, Pen and Digitizer): their Spec
 * factories, one recipe for both.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

extern "C" {

struct StylusFieldNames {
    const char* barrel_usages;
    const char* disabled_fields;
    const char* enable_eraser;
    const char* enable_hover;
    const char* enable_pressure;
    const char* enable_tilt;
    const char* enable_twist_target_specific;
    const char* fields;
    const char* neutral_fields;
    const char* pressure;
    const char* report_id;
    const char* tilt_x;
    const char* tilt_y;
    const char* twist;
    const char* x;
    const char* y;
    const char* options;
};

constexpr StylusFieldNames kPenFieldNames{
    "pen.barrel_usages",
    "pen.disabled_fields",
    "pen.enable_eraser",
    "pen.enable_hover",
    "pen.enable_pressure",
    "pen.enable_tilt",
    "pen.enable_twist_target_specific",
    "pen.fields",
    "pen.neutral_fields",
    "pen.pressure",
    "pen.report_id",
    "pen.tilt_x",
    "pen.tilt_y",
    "pen.twist",
    "pen.x",
    "pen.y",
    "pen_options",
};

constexpr StylusFieldNames kDigitizerFieldNames{
    "digitizer.barrel_usages",
    "digitizer.disabled_fields",
    "digitizer.enable_eraser",
    "digitizer.enable_hover",
    "digitizer.enable_pressure",
    "digitizer.enable_tilt",
    "digitizer.enable_twist_target_specific",
    "digitizer.fields",
    "digitizer.neutral_fields",
    "digitizer.pressure",
    "digitizer.report_id",
    "digitizer.tilt_x",
    "digitizer.tilt_y",
    "digitizer.twist",
    "digitizer.x",
    "digitizer.y",
    "digitizer_options",
};

// Pen and Digitizer share one report shape; only the Application Usage differs.
static mimusb_result create_stylus_spec(const mimusb_pen_options* options,
                                        const mimusb_profile_kind kind, mimusb_spec** out_spec) {
    const StylusFieldNames& names =
        kind == MIMUSB_PROFILE_PEN ? kPenFieldNames : kDigitizerFieldNames;
    if (options->reserved != 0U || !validate_report_id(options->report_id, names.report_id) ||
        !validate_field(options->x, names.x) || !validate_field(options->y, names.y) ||
        options->x.logical_minimum > 0 || options->x.logical_maximum < 0 ||
        options->y.logical_minimum > 0 || options->y.logical_maximum < 0 ||
        !validate_option_boolean(options->enable_pressure, names.enable_pressure) ||
        !validate_option_boolean(options->enable_tilt, names.enable_tilt) ||
        !validate_option_boolean(options->enable_twist_target_specific,
                                 names.enable_twist_target_specific) ||
        !validate_option_boolean(options->enable_eraser, names.enable_eraser) ||
        !validate_option_boolean(options->enable_hover, names.enable_hover) ||
        options->barrel_usage_count > 32U ||
        (options->barrel_usage_count != 0U && options->barrel_usages == nullptr)) {
        return mimusb::detail::finish_result(
            MIMUSB_ERR_PARAM, names.options,
            "An enabled pen field has an invalid range or bit width.");
    }
    if ((options->enable_pressure == 1U &&
         (!validate_field(options->pressure, names.pressure) ||
          options->pressure.logical_minimum != 0 || options->pressure.logical_maximum < 1)) ||
        (options->enable_tilt == 1U && (!validate_field(options->tilt_x, names.tilt_x) ||
                                        !validate_field(options->tilt_y, names.tilt_y))) ||
        (options->enable_twist_target_specific == 1U &&
         !validate_field(options->twist, names.twist))) {
        return mimusb::detail::finish_result(
            MIMUSB_ERR_PARAM, names.fields,
            "An enabled pen field has an invalid range or bit width.");
    }
    if ((options->enable_pressure == 0U && !field_is_zero(options->pressure)) ||
        (options->enable_tilt == 0U &&
         (!field_is_zero(options->tilt_x) || !field_is_zero(options->tilt_y))) ||
        (options->enable_twist_target_specific == 0U && !field_is_zero(options->twist)) ||
        (options->barrel_usage_count == 0U && options->barrel_usages != nullptr)) {
        set_error(MIMUSB_ERR_PARAM, names.disabled_fields,
                  "Disabled optional pen declarations and absent barrel storage must be canonical "
                  "zero/null.");
        return MIMUSB_ERR_PARAM;
    }
    if ((options->enable_tilt == 1U &&
         (options->tilt_x.logical_minimum > 0 || options->tilt_x.logical_maximum < 0 ||
          options->tilt_y.logical_minimum > 0 || options->tilt_y.logical_maximum < 0)) ||
        (options->enable_twist_target_specific == 1U &&
         (options->twist.logical_minimum > 0 || options->twist.logical_maximum < 0))) {
        set_error(
            MIMUSB_ERR_PARAM, names.neutral_fields,
            "Enabled tilt and twist fields must represent zero for hover and departure reports.");
        return MIMUSB_ERR_PARAM;
    }
    mimusb::detail::PenConfig config{};
    config.options = *options;
    if (options->barrel_usage_count != 0U) {
        config.barrel_usages.assign(options->barrel_usages,
                                    options->barrel_usages + options->barrel_usage_count);
    }
    for (std::size_t index = 0U; index < config.barrel_usages.size(); ++index) {
        if ((config.barrel_usages[index] != mimusb::hid::usage::barrel_switch &&
             config.barrel_usages[index] != mimusb::hid::usage::secondary_barrel_switch) ||
            std::find(config.barrel_usages.begin(),
                      config.barrel_usages.begin() + static_cast<std::ptrdiff_t>(index),
                      config.barrel_usages[index]) !=
                config.barrel_usages.begin() + static_cast<std::ptrdiff_t>(index)) {
            set_error(MIMUSB_ERR_PARAM, names.barrel_usages,
                      "Each barrel control must be a unique HUT 1.7 Barrel Switch (0x44) or "
                      "Secondary Barrel Switch (0x5A).");
            return MIMUSB_ERR_PARAM;
        }
    }
    config.options.barrel_usages = nullptr;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery,
                          kind == MIMUSB_PROFILE_PEN ? kPenBatteryNames : kDigitizerBatteryNames)) {
        return MIMUSB_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{kind}, config, battery,
        [=](DescriptorBuilder& builder) {
            const std::uint16_t application = kind == MIMUSB_PROFILE_PEN
                                                  ? mimusb::hid::usage::pen
                                                  : mimusb::hid::usage::digitizer;
            if (!builder.begin_application(mimusb::hid::usage::page_digitizers, application) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.begin_physical(mimusb::hid::usage::page_digitizers,
                                        mimusb::hid::usage::stylus)) {
                return false;
            }
            // The Linux HID core processes Variable fields in descriptor order. In
            // the audited legacy input path, In Range chooses PEN or RUBBER
            // from the Invert state already seen in that report, so Invert is
            // placed before Tip and In Range.
            if (options->enable_eraser == 1U &&
                !builder.variable(mimusb::hid::usage::page_digitizers, mimusb::hid::usage::invert,
                                  0, 1, 1U, false, false, FieldSemantic::eraser, 0U)) {
                return false;
            }
            if (!builder.variable(mimusb::hid::usage::page_digitizers,
                                  mimusb::hid::usage::tip_switch, 0, 1, 1U, false, false,
                                  FieldSemantic::tip, 0U) ||
                !builder.variable(mimusb::hid::usage::page_digitizers, mimusb::hid::usage::in_range,
                                  0, 1, 1U, false, false, FieldSemantic::in_range, 0U)) {
                return false;
            }
            for (std::size_t index = 0U; index < options->barrel_usage_count; ++index) {
                if (!builder.variable(mimusb::hid::usage::page_digitizers,
                                      options->barrel_usages[index], 0, 1, 1U, false, false,
                                      FieldSemantic::buttons, static_cast<std::uint16_t>(index))) {
                    return false;
                }
            }
            pad_report(builder);
            if (!builder.variable(mimusb::hid::usage::page_generic_desktop, mimusb::hid::usage::x,
                                  options->x.logical_minimum, options->x.logical_maximum,
                                  static_cast<std::uint8_t>(options->x.bit_width), false, false,
                                  FieldSemantic::x, 0U, false, &options->x.physical) ||
                !builder.variable(mimusb::hid::usage::page_generic_desktop, mimusb::hid::usage::y,
                                  options->y.logical_minimum, options->y.logical_maximum,
                                  static_cast<std::uint8_t>(options->y.bit_width), false, false,
                                  FieldSemantic::y, 0U, false, &options->y.physical)) {
                return false;
            }
            // Each of X, Y, Pressure, Tilt X/Y, and Twist below is an
            // independently caller-declared 1-32 bit field (see LIMITS.md);
            // realigning between them keeps any one of them from starting at
            // a nonzero bit offset and crossing a fifth byte (HID 1.11 8.4).
            pad_report(builder);
            if (options->enable_pressure == 1U &&
                !builder.variable(
                    mimusb::hid::usage::page_digitizers, mimusb::hid::usage::tip_pressure,
                    options->pressure.logical_minimum, options->pressure.logical_maximum,
                    static_cast<std::uint8_t>(options->pressure.bit_width), false, false,
                    FieldSemantic::pressure, 0U, false, &options->pressure.physical)) {
                return false;
            }
            pad_report(builder);
            if (options->enable_tilt == 1U &&
                (!builder.variable(mimusb::hid::usage::page_digitizers, mimusb::hid::usage::x_tilt,
                                   options->tilt_x.logical_minimum, options->tilt_x.logical_maximum,
                                   static_cast<std::uint8_t>(options->tilt_x.bit_width), false,
                                   false, FieldSemantic::tilt_x, 0U, false,
                                   &options->tilt_x.physical) ||
                 !builder.variable(mimusb::hid::usage::page_digitizers, mimusb::hid::usage::y_tilt,
                                   options->tilt_y.logical_minimum, options->tilt_y.logical_maximum,
                                   static_cast<std::uint8_t>(options->tilt_y.bit_width), false,
                                   false, FieldSemantic::tilt_y, 0U, false,
                                   &options->tilt_y.physical))) {
                return false;
            }
            pad_report(builder);
            if (options->enable_twist_target_specific == 1U &&
                !builder.variable(mimusb::hid::usage::page_digitizers, mimusb::hid::usage::twist,
                                  options->twist.logical_minimum, options->twist.logical_maximum,
                                  static_cast<std::uint8_t>(options->twist.bit_width), false, false,
                                  FieldSemantic::twist, 0U, false, &options->twist.physical)) {
                return false;
            }
            pad_report(builder);
            return builder.end_collection() && builder.end_collection();
        },
        out_spec);
}

static mimusb_result mimusb_spec_create_pen_impl(const mimusb_pen_options* options,
                                                 mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "pen_options")) {
        return mimusb::detail::finish_result(MIMUSB_ERR_PARAM, "pen_options",
                                             "The pen options contain an invalid required field.");
    }
    return create_stylus_spec(options, MIMUSB_PROFILE_PEN, out_spec);
}

static mimusb_result mimusb_spec_create_digitizer_impl(const mimusb_digitizer_options* options,
                                                       mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "digitizer_options")) {
        return mimusb::detail::finish_result(
            MIMUSB_ERR_PARAM, "digitizer_options",
            "The digitizer options contain an invalid required field.");
    }
    mimusb_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.reserved = options->reserved;
    pen.report_id = options->report_id;
    pen.x = options->x;
    pen.y = options->y;
    pen.enable_pressure = options->enable_pressure;
    pen.pressure = options->pressure;
    pen.enable_tilt = options->enable_tilt;
    pen.tilt_x = options->tilt_x;
    pen.tilt_y = options->tilt_y;
    pen.enable_twist_target_specific = options->enable_twist_target_specific;
    pen.twist = options->twist;
    pen.barrel_usages = options->barrel_usages;
    pen.barrel_usage_count = options->barrel_usage_count;
    pen.enable_eraser = options->enable_eraser;
    pen.enable_hover = options->enable_hover;
    pen.enable_battery = options->enable_battery;
    pen.battery = options->battery;
    return create_stylus_spec(&pen, MIMUSB_PROFILE_DIGITIZER, out_spec);
}

mimusb_result MIMUSB_CALL mimusb_spec_create_pen(const mimusb_pen_options* options,
                                                 mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_pen", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_pen_impl(options, out_spec); });
    });
}

mimusb_result MIMUSB_CALL mimusb_spec_create_digitizer(const mimusb_digitizer_options* options,
                                                       mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_digitizer", [&] {
        return create_hid_spec(options, out_spec, [&] {
            return mimusb_spec_create_digitizer_impl(options, out_spec);
        });
    });
}

} // extern "C"
