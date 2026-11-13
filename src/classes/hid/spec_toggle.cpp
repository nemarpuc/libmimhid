// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The toggle profile: Consumer, System, Camera or Telephony keys selected by their pages and
 * usages (HID Usage Tables), as one array or bitmap of usage controls. Its Spec factory.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

namespace {

struct UsageSemanticFlags {
    bool accepted;
    bool relative;
    bool no_preferred;
};

UsageSemanticFlags usage_semantic_flags(const mimusb_usage_semantic semantic) noexcept {
    switch (semantic) {
    case MIMUSB_USAGE_SELECTOR_BITMAP:
    case MIMUSB_USAGE_MOMENTARY:
    case MIMUSB_USAGE_RETRIGGER:
        return UsageSemanticFlags{true, false, false};
    case MIMUSB_USAGE_ON_OFF_TOGGLE:
    case MIMUSB_USAGE_ONE_SHOT:
        return UsageSemanticFlags{true, true, false};
    case MIMUSB_USAGE_ON_OFF_MAINTAINED:
        return UsageSemanticFlags{true, false, true};
    case MIMUSB_USAGE_ON_OFF_PAIR:
    case MIMUSB_USAGE_LINEAR:
    case MIMUSB_USAGE_DYNAMIC_VALUE:
    case MIMUSB_USAGE_NAMED_ARRAY:
    default:
        return UsageSemanticFlags{false, false, false};
    }
}

bool emit_usage_controls(DescriptorBuilder& builder, const mimusb_toggle_options& options) {
    if (!builder.begin_application(options.application_page, options.application_usage) ||
        !builder.set_report_id(options.report_id.enabled == 1U, options.report_id.value)) {
        return false;
    }
    for (std::size_t index = 0U; index < options.allowed_usage_count; ++index) {
        const UsageSemanticFlags flags = usage_semantic_flags(options.usage_semantics[index]);
        if (!flags.accepted ||
            !builder.variable(options.field_page, options.allowed_usages[index], 0, 1, 1U,
                              flags.relative, false, FieldSemantic::consumer_usage,
                              static_cast<std::uint16_t>(index), flags.no_preferred)) {
            return false;
        }
    }
    pad_report(builder);
    return builder.end_collection();
}

mimusb_result create_usage_control_spec(const mimusb_toggle_options* options,
                                        mimusb_spec** out_spec) {
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "toggle_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U || options->reserved16 != 0U ||
        !validate_report_id(options->report_id, "toggle.report_id")) {
        set_error(MIMUSB_ERR_PARAM, "toggle.reserved",
                  "reserved fields must be zero and the Report ID declaration must be valid.");
        return MIMUSB_ERR_PARAM;
    }
    const bool is_camera = options->field_page == mimusb::hid::usage::page_camera_control;
    if (options->application_page == 0U || options->application_usage == 0U ||
        options->field_page == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "toggle.application",
                  "application_page, application_usage, and field_page are required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (options->allowed_usages == nullptr || options->allowed_usage_count == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "toggle.allowed_usages",
                  "At least one caller-selected Usage is required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (options->usage_semantics == nullptr) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "toggle.usage_semantics",
                  "Every allow-listed Usage needs an explicit HUT semantic.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (options->expected_linux_event_types == nullptr ||
        options->expected_linux_codes == nullptr) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "toggle.expected_linux_events",
                  "Every allow-listed Usage needs explicit target-event evidence.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (options->allowed_usage_count > std::numeric_limits<std::uint16_t>::max()) {
        set_error(MIMUSB_ERR_OVERFLOW, "toggle.allowed_usage_count",
                  "The generated layout cannot index this many one-bit controls.");
        return MIMUSB_ERR_OVERFLOW;
    }
    for (std::size_t index = 0; index < options->allowed_usage_count; ++index) {
        if (options->allowed_usages[index] == 0U ||
            options->expected_linux_event_types[index] == nullptr ||
            options->expected_linux_codes[index] == nullptr ||
            options->expected_linux_event_types[index][0] == '\0' ||
            options->expected_linux_codes[index][0] == '\0') {
            set_error(MIMUSB_ERR_PARAM, "toggle.allowed_usages",
                      "Every nonzero Usage requires explicit expected Linux event type and code "
                      "evidence.");
            return MIMUSB_ERR_PARAM;
        }
        if (std::find(options->allowed_usages, options->allowed_usages + index,
                      options->allowed_usages[index]) != options->allowed_usages + index) {
            set_error(MIMUSB_ERR_PARAM, "toggle.allowed_usages",
                      "Each Usage may appear only once in the allow-list.");
            return MIMUSB_ERR_PARAM;
        }
        if (options->application_page == options->field_page &&
            options->allowed_usages[index] == options->application_usage) {
            set_error(MIMUSB_ERR_PARAM, "toggle.allowed_usages",
                      "The Application Collection Usage is not an Input control Usage.");
            return MIMUSB_ERR_PARAM;
        }
        const mimusb_usage_semantic semantic = options->usage_semantics[index];
        if (!usage_semantic_flags(semantic).accepted) {
            if (semantic == MIMUSB_USAGE_ON_OFF_PAIR || semantic == MIMUSB_USAGE_LINEAR ||
                semantic == MIMUSB_USAGE_DYNAMIC_VALUE || semantic == MIMUSB_USAGE_NAMED_ARRAY) {
                set_error(MIMUSB_ERR_UNSUPPORTED, "toggle.usage_semantics",
                          "This HUT semantic requires a value, direction, or collection API; "
                          "it cannot be represented by press/release.");
                return MIMUSB_ERR_UNSUPPORTED;
            }
            set_error(MIMUSB_ERR_PARAM, "toggle.usage_semantics",
                      "Every Usage must select one defined mimusb_usage_semantic value.");
            return MIMUSB_ERR_PARAM;
        }
        if (is_camera && options->allowed_usages[index] != mimusb::hid::usage::camera_auto_focus &&
            options->allowed_usages[index] != mimusb::hid::usage::camera_shutter) {
            set_error(MIMUSB_ERR_PARAM, "toggle.allowed_usages",
                      "HUT 1.7 Camera Control defines Auto-focus 0x20 and Shutter "
                      "0x21; no other Usage exists on that page.");
            return MIMUSB_ERR_PARAM;
        }
        if (is_camera && semantic != MIMUSB_USAGE_ONE_SHOT) {
            set_error(MIMUSB_ERR_UNSUPPORTED, "toggle.usage_semantics",
                      "HUT 1.7 Table 35.1 defines Auto-focus and Shutter as One Shot Controls.");
            return MIMUSB_ERR_UNSUPPORTED;
        }
    }
    mimusb::detail::ConsumerConfig config{};
    config.options = *options;
    config.allowed_usages.assign(options->allowed_usages,
                                 options->allowed_usages + options->allowed_usage_count);
    config.usage_semantics.assign(options->usage_semantics,
                                  options->usage_semantics + options->allowed_usage_count);
    for (std::size_t index = 0U; index < options->allowed_usage_count; ++index) {
        config.expected_linux_event_types.emplace_back(options->expected_linux_event_types[index]);
        config.expected_linux_codes.emplace_back(options->expected_linux_codes[index]);
    }
    config.options.allowed_usages = nullptr;
    config.options.allowed_usage_count = config.allowed_usages.size();
    config.options.usage_semantics = nullptr;
    config.options.expected_linux_event_types = nullptr;
    config.options.expected_linux_codes = nullptr;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kToggleBatteryNames)) {
        return MIMUSB_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{MIMUSB_PROFILE_TOGGLE}, std::move(config), battery,
        [=](DescriptorBuilder& builder) { return emit_usage_controls(builder, *options); },
        out_spec);
}

} // namespace

extern "C" {

static mimusb_result mimusb_spec_create_toggle_impl(const mimusb_toggle_options* options,
                                                    mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    return create_usage_control_spec(options, out_spec);
}

mimusb_result MIMUSB_CALL mimusb_spec_create_toggle(const mimusb_toggle_options* options,
                                                    mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_toggle", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_toggle_impl(options, out_spec); });
    });
}

} // extern "C"
