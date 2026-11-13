// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The touchscreen and touchpad profiles (HID Usage Tables: Digitizers, Touch Screen and Touch
 * Pad; Microsoft precision touchpad fields): their Spec factories, one recipe for both.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

namespace {

template <typename Options>
void touch_fields_from(const Options& options, mimusb::detail::TouchFields* out) noexcept {
    out->report_id = options.report_id;
    out->maximum_contacts = options.maximum_contacts;
    out->contacts_per_report = options.contacts_per_report;
    out->contact_id = options.contact_id;
    out->x = options.x;
    out->y = options.y;
    out->contact_count = options.contact_count;
    out->enable_pressure = options.enable_pressure;
    out->pressure = options.pressure;
    out->enable_width = options.enable_width;
    out->width = options.width;
    out->enable_height = options.enable_height;
    out->height = options.height;
    out->enable_azimuth = options.enable_azimuth;
    out->azimuth = options.azimuth;
    out->enable_scan_time = options.enable_scan_time;
    out->scan_time = options.scan_time;
    out->scan_time_unit_100us = options.scan_time_unit_100us;
    out->enable_contact_count_maximum_feature_declaration =
        options.enable_contact_count_maximum_feature_declaration;
    out->enable_multi_packet_frames = options.enable_multi_packet_frames;
}

} // namespace

extern "C" {

struct TouchFieldNames {
    const char* report_id;
    const char* x;
    const char* y;
    const char* enable_pressure;
    const char* enable_width;
    const char* enable_height;
    const char* enable_azimuth;
    const char* enable_scan_time;
    const char* scan_time_unit_100us;
    const char* enable_contact_count_maximum_feature_declaration;
    const char* enable_multi_packet_frames;
    const char* fields;
    const char* contact_id;
    const char* contact_count;
    const char* contact_fields;
    const char* contacts_per_report;
    const char* pressure;
    const char* disabled_fields;
    const char* azimuth;
    const char* width;
    const char* height;
    const char* inactive_geometry;
    const char* geometry_units;
    const char* scan_time;
    const char* button_count;
};

constexpr TouchFieldNames kTouchscreenFieldNames{
    "touchscreen.report_id",
    "touchscreen.x",
    "touchscreen.y",
    "touchscreen.enable_pressure",
    "touchscreen.enable_width",
    "touchscreen.enable_height",
    "touchscreen.enable_azimuth",
    "touchscreen.enable_scan_time",
    "touchscreen.scan_time_unit_100us",
    "touchscreen.enable_contact_count_maximum_feature_declaration",
    "touchscreen.enable_multi_packet_frames",
    "touchscreen.fields",
    "touchscreen.contact_id",
    "touchscreen.contact_count",
    "touchscreen.contact_fields",
    "touchscreen.contacts_per_report",
    "touchscreen.pressure",
    "touchscreen.disabled_fields",
    "touchscreen.azimuth",
    "touchscreen.width",
    "touchscreen.height",
    "touchscreen.inactive_geometry",
    "touchscreen.geometry_units",
    "touchscreen.scan_time",
    "touchscreen.button_count",
};

constexpr TouchFieldNames kTouchpadFieldNames{
    "touchpad.report_id",
    "touchpad.x",
    "touchpad.y",
    "touchpad.enable_pressure",
    "touchpad.enable_width",
    "touchpad.enable_height",
    "touchpad.enable_azimuth",
    "touchpad.enable_scan_time",
    "touchpad.scan_time_unit_100us",
    "touchpad.enable_contact_count_maximum_feature_declaration",
    "touchpad.enable_multi_packet_frames",
    "touchpad.fields",
    "touchpad.contact_id",
    "touchpad.contact_count",
    "touchpad.contact_fields",
    "touchpad.contacts_per_report",
    "touchpad.pressure",
    "touchpad.disabled_fields",
    "touchpad.azimuth",
    "touchpad.width",
    "touchpad.height",
    "touchpad.inactive_geometry",
    "touchpad.geometry_units",
    "touchpad.scan_time",
    "touchpad.button_count",
};

// profile_kind and application_usage are only ever passed
// as fixed literals from this function's two call sites (the Touchscreen and
// Touchpad wrappers), never from caller-supplied runtime values, so a
// transposition would be a compile-time-local mistake caught immediately by
// every existing test rather than a live bug.
static mimusb_result create_touch_spec_from_fields(
    const mimusb::detail::TouchFields& fields, const TouchFieldNames& names,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const mimusb_profile_kind profile_kind, const std::uint16_t application_usage,
    const BatteryRequest& battery, mimusb_spec** out_spec) {
    if (!validate_report_id(fields.report_id, names.report_id) ||
        !validate_field(fields.x, names.x) || !validate_field(fields.y, names.y) ||
        fields.x.logical_minimum > 0 || fields.x.logical_maximum < 0 ||
        fields.y.logical_minimum > 0 || fields.y.logical_maximum < 0 ||
        fields.maximum_contacts == 0U || fields.maximum_contacts > 16U ||
        fields.contacts_per_report == 0U || fields.contacts_per_report > fields.maximum_contacts ||
        !validate_option_boolean(fields.enable_pressure, names.enable_pressure) ||
        !validate_option_boolean(fields.enable_width, names.enable_width) ||
        !validate_option_boolean(fields.enable_height, names.enable_height) ||
        !validate_option_boolean(fields.enable_azimuth, names.enable_azimuth) ||
        !validate_option_boolean(fields.enable_scan_time, names.enable_scan_time) ||
        !validate_option_boolean(fields.scan_time_unit_100us, names.scan_time_unit_100us) ||
        !validate_option_boolean(fields.enable_contact_count_maximum_feature_declaration,
                                 names.enable_contact_count_maximum_feature_declaration) ||
        !validate_option_boolean(fields.enable_multi_packet_frames,
                                 names.enable_multi_packet_frames)) {
        set_error(MIMUSB_ERR_PARAM, names.fields,
                  "The touch options contain an invalid required field, range, count, or flag.");
        return MIMUSB_ERR_PARAM;
    }
    if (!validate_field(fields.contact_id, names.contact_id) ||
        !validate_field(fields.contact_count, names.contact_count)) {
        set_error(MIMUSB_ERR_PARAM, names.contact_fields,
                  "Fixed-slot Multi-Touch requires explicit Contact Identifier and Contact Count "
                  "fields.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.contact_count.logical_minimum > 0 ||
        fields.contact_count.logical_maximum < static_cast<std::int32_t>(fields.maximum_contacts)) {
        set_error(MIMUSB_ERR_PARAM, names.contact_count,
                  "Contact Count must represent zero through maximum_contacts even though "
                  "fixed-slot output never emits an isolated zero frame.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.contact_id.logical_minimum > 0 || fields.contact_id.logical_maximum < 0 ||
        static_cast<std::uint64_t>(fields.contact_id.logical_maximum) + 1U <
            fields.maximum_contacts) {
        set_error(MIMUSB_ERR_PARAM, names.contact_id,
                  "The Contact Identifier range must represent zero for inactive slots and at "
                  "least maximum_contacts distinct nonnegative API IDs.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_multi_packet_frames == 0U &&
        fields.contacts_per_report != fields.maximum_contacts) {
        set_error(MIMUSB_ERR_PARAM, names.contacts_per_report,
                  "Without multi-packet frames every declared contact must fit one report.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_pressure == 1U &&
        (!validate_field(fields.pressure, names.pressure) || fields.pressure.logical_minimum != 0 ||
         fields.pressure.logical_maximum < 1)) {
        set_error(MIMUSB_ERR_PARAM, names.pressure,
                  "Enabled touch pressure must explicitly represent zero and at least one positive "
                  "contact value.");
        return MIMUSB_ERR_PARAM;
    }
    if ((fields.enable_pressure == 0U && !field_is_zero(fields.pressure)) ||
        (fields.enable_width == 0U && !field_is_zero(fields.width)) ||
        (fields.enable_height == 0U && !field_is_zero(fields.height)) ||
        (fields.enable_azimuth == 0U && !field_is_zero(fields.azimuth)) ||
        (fields.enable_scan_time == 0U &&
         (!field_is_zero(fields.scan_time) || fields.scan_time_unit_100us != 0U))) {
        set_error(
            MIMUSB_ERR_PARAM, names.disabled_fields,
            "Disabled pressure, geometry, and Scan Time declarations must be canonical zero.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_azimuth == 1U &&
        (!validate_field(fields.azimuth, names.azimuth) || fields.azimuth.logical_minimum != 0 ||
         fields.azimuth.logical_maximum <= 0)) {
        set_error(MIMUSB_ERR_PARAM, names.azimuth,
                  "Enabled Azimuth requires logical minimum zero and a positive full-turn logical "
                  "maximum.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_width == 1U && !validate_field(fields.width, names.width)) {
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_height == 1U && !validate_field(fields.height, names.height)) {
        return MIMUSB_ERR_PARAM;
    }
    if ((fields.enable_width == 1U &&
         (fields.width.logical_minimum > 0 || fields.width.logical_maximum < 0)) ||
        (fields.enable_height == 1U &&
         (fields.height.logical_minimum > 0 || fields.height.logical_maximum < 0))) {
        set_error(MIMUSB_ERR_PARAM, names.inactive_geometry,
                  "Enabled width and height fields must represent zero for inactive fixed slots.");
        return MIMUSB_ERR_PARAM;
    }
    if ((fields.enable_width == 1U &&
         !physical_units_match(fields.width.physical, fields.x.physical)) ||
        (fields.enable_height == 1U &&
         !physical_units_match(fields.height.physical, fields.y.physical))) {
        set_error(MIMUSB_ERR_PARAM, names.geometry_units,
                  "HUT Width must use the same HID Unit and Unit Exponent as X, and Height must "
                  "use the same Unit and Unit Exponent as Y.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.enable_scan_time == 1U &&
        (!validate_field(fields.scan_time, names.scan_time) || fields.scan_time_unit_100us != 1U ||
         fields.scan_time.logical_minimum != 0 || fields.scan_time.logical_maximum < 1 ||
         !physical_is_zero(fields.scan_time.physical))) {
        set_error(
            MIMUSB_ERR_PARAM, names.scan_time,
            "The portable Linux profile requires an explicit 100-microsecond Scan Time counter.");
        return MIMUSB_ERR_PARAM;
    }
    if (fields.button_count > 65535U) {
        set_error(MIMUSB_ERR_OVERFLOW, names.button_count,
                  "The Button Usage range cannot exceed the 16-bit Usage value space.");
        return MIMUSB_ERR_OVERFLOW;
    }
    mimusb::detail::TouchConfig config{};
    config.fields = fields;
    if (!validate_battery(battery, profile_kind == MIMUSB_PROFILE_TOUCHSCREEN
                                       ? kTouchscreenBatteryNames
                                       : kTouchpadBatteryNames)) {
        return MIMUSB_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{profile_kind}, config, battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(mimusb::hid::usage::page_digitizers,
                                           application_usage) ||
                !builder.set_report_id(fields.report_id.enabled == 1U, fields.report_id.value)) {
                return false;
            }
            if (fields.enable_contact_count_maximum_feature_declaration == 1U &&
                !builder.feature_static_value(
                    mimusb::hid::usage::page_digitizers, mimusb::hid::usage::contact_count_maximum,
                    0, static_cast<std::int32_t>(fields.maximum_contacts),
                    static_cast<std::uint8_t>(fields.contact_count.bit_width))) {
                return false;
            }
            for (std::uint32_t contact = 0U; contact < fields.contacts_per_report; ++contact) {
                if (!builder.begin_logical(mimusb::hid::usage::page_digitizers,
                                           mimusb::hid::usage::finger) ||
                    !builder.variable(mimusb::hid::usage::page_digitizers,
                                      mimusb::hid::usage::tip_switch, 0, 1, 1U, false, false,
                                      FieldSemantic::tip, static_cast<std::uint16_t>(contact))) {
                    return false;
                }
                pad_report(builder);
                if (!builder.variable(
                        mimusb::hid::usage::page_digitizers, mimusb::hid::usage::contact_identifier,
                        fields.contact_id.logical_minimum, fields.contact_id.logical_maximum,
                        static_cast<std::uint8_t>(fields.contact_id.bit_width), false, false,
                        FieldSemantic::contact_id, static_cast<std::uint16_t>(contact), false,
                        &fields.contact_id.physical) ||
                    !builder.variable(mimusb::hid::usage::page_generic_desktop,
                                      mimusb::hid::usage::x, fields.x.logical_minimum,
                                      fields.x.logical_maximum,
                                      static_cast<std::uint8_t>(fields.x.bit_width), false, false,
                                      FieldSemantic::x, static_cast<std::uint16_t>(contact), false,
                                      &fields.x.physical) ||
                    !builder.variable(mimusb::hid::usage::page_generic_desktop,
                                      mimusb::hid::usage::y, fields.y.logical_minimum,
                                      fields.y.logical_maximum,
                                      static_cast<std::uint8_t>(fields.y.bit_width), false, false,
                                      FieldSemantic::y, static_cast<std::uint16_t>(contact), false,
                                      &fields.y.physical)) {
                    return false;
                }
                // Contact Identifier, X, Y, Pressure, Width, Height, and
                // Azimuth are each an independently caller-declared field
                // (see LIMITS.md); realigning after Y and between the
                // optional fields keeps any one of them from starting at a
                // nonzero bit offset and crossing a fifth byte (HID 1.11 8.4).
                pad_report(builder);
                if (fields.enable_pressure == 1U &&
                    !builder.variable(
                        mimusb::hid::usage::page_digitizers, mimusb::hid::usage::tip_pressure,
                        fields.pressure.logical_minimum, fields.pressure.logical_maximum,
                        static_cast<std::uint8_t>(fields.pressure.bit_width), false, false,
                        FieldSemantic::pressure, static_cast<std::uint16_t>(contact), false,
                        &fields.pressure.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_width == 1U &&
                    !builder.variable(
                        mimusb::hid::usage::page_digitizers, mimusb::hid::usage::width,
                        fields.width.logical_minimum, fields.width.logical_maximum,
                        static_cast<std::uint8_t>(fields.width.bit_width), false, false,
                        FieldSemantic::width, static_cast<std::uint16_t>(contact), false,
                        &fields.width.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_height == 1U &&
                    !builder.variable(
                        mimusb::hid::usage::page_digitizers, mimusb::hid::usage::height,
                        fields.height.logical_minimum, fields.height.logical_maximum,
                        static_cast<std::uint8_t>(fields.height.bit_width), false, false,
                        FieldSemantic::height, static_cast<std::uint16_t>(contact), false,
                        &fields.height.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_azimuth == 1U &&
                    !builder.variable(
                        mimusb::hid::usage::page_digitizers, mimusb::hid::usage::azimuth,
                        fields.azimuth.logical_minimum, fields.azimuth.logical_maximum,
                        static_cast<std::uint8_t>(fields.azimuth.bit_width), false, false,
                        FieldSemantic::azimuth, static_cast<std::uint16_t>(contact), false,
                        &fields.azimuth.physical)) {
                    return false;
                }
                if (!builder.end_collection()) {
                    return false;
                }
            }
            // Scan Time and Contact Count are each an independently
            // caller-declared 1-32 bit field (see LIMITS.md), following the
            // last enabled per-contact field above; realigning between them
            // keeps any one of them from starting at a nonzero bit offset
            // and crossing a fifth byte (HID 1.11 8.4).
            pad_report(builder);
            if (fields.enable_scan_time == 1U &&
                !builder.variable(
                    mimusb::hid::usage::page_digitizers, mimusb::hid::usage::scan_time,
                    fields.scan_time.logical_minimum, fields.scan_time.logical_maximum,
                    static_cast<std::uint8_t>(fields.scan_time.bit_width), false, false,
                    FieldSemantic::scan_time, 0U, false, &fields.scan_time.physical)) {
                return false;
            }
            pad_report(builder);
            if (!builder.variable(
                    mimusb::hid::usage::page_digitizers, mimusb::hid::usage::contact_count,
                    fields.contact_count.logical_minimum, fields.contact_count.logical_maximum,
                    static_cast<std::uint8_t>(fields.contact_count.bit_width), false, false,
                    FieldSemantic::contact_count, 0U, false, &fields.contact_count.physical)) {
                return false;
            }
            if (fields.button_count > 0U) {
                if (!builder.variable_range(mimusb::hid::usage::page_button, 1U,
                                            static_cast<std::uint16_t>(fields.button_count), 1U,
                                            FieldSemantic::buttons)) {
                    return false;
                }
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

static mimusb_result mimusb_spec_create_touchscreen_impl(const mimusb_touchscreen_options* options,
                                                         mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "touchscreen_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U) {
        set_error(MIMUSB_ERR_PARAM, "touchscreen_options",
                  "The touch options contain an invalid required field, range, count, or flag.");
        return MIMUSB_ERR_PARAM;
    }
    mimusb::detail::TouchFields fields{};
    touch_fields_from(*options, &fields);
    fields.button_count = 0U;
    return create_touch_spec_from_fields(
        fields, kTouchscreenFieldNames, MIMUSB_PROFILE_TOUCHSCREEN,
        mimusb::hid::usage::touch_screen,
        BatteryRequest{options->enable_battery, options->battery, options->report_id}, out_spec);
}

static mimusb_result mimusb_spec_create_touchpad_impl(const mimusb_touchpad_options* options,
                                                      mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "touchpad_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U) {
        set_error(MIMUSB_ERR_PARAM, "touchpad_options",
                  "The touchpad options contain an invalid required field, range, count, or flag.");
        return MIMUSB_ERR_PARAM;
    }
    mimusb::detail::TouchFields fields{};
    touch_fields_from(*options, &fields);
    fields.button_count = options->button_count;
    return create_touch_spec_from_fields(
        fields, kTouchpadFieldNames, MIMUSB_PROFILE_TOUCHPAD, mimusb::hid::usage::touch_pad,
        BatteryRequest{options->enable_battery, options->battery, options->report_id}, out_spec);
}

// Pen and Digitizer share their validation; each reports its own field names.

mimusb_result MIMUSB_CALL mimusb_spec_create_touchscreen(const mimusb_touchscreen_options* options,
                                                         mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_touchscreen", [&] {
        return create_hid_spec(options, out_spec, [&] {
            return mimusb_spec_create_touchscreen_impl(options, out_spec);
        });
    });
}

mimusb_result MIMUSB_CALL mimusb_spec_create_touchpad(const mimusb_touchpad_options* options,
                                                      mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_touchpad", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_touchpad_impl(options, out_spec); });
    });
}

} // extern "C"
