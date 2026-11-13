// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * A HID Spec from a report descriptor the caller wrote (mimusb_spec_create_hid): the descriptor
 * is checked by the HID parser against the table of reports the caller says it sends.
 */

#include "classes/hid/spec_support.hpp"

using namespace mimusb::spec_build;

extern "C" {

static mimusb_result mimusb_spec_create_hid_impl(const mimusb_hid_options* options,
                                                 mimusb_spec** out_spec) {
    mimusb::detail::clear_error();
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*options)),
                                      "hid_options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (out_spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_spec", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_spec = nullptr;
    if (options->reserved != 0U || !mimusb::detail::valid_boolean(options->requires_output) ||
        !mimusb::detail::valid_boolean(options->requires_feature_response)) {
        set_error(MIMUSB_ERR_PARAM, "hid.options",
                  "The reserved field must be zero and every flag exactly zero or one.");
        return MIMUSB_ERR_PARAM;
    }
    // validate_raw permits exactly the 256 possible Report IDs. Bound every
    // count-derived allocation before using the caller's count so an invalid
    // raw table cannot turn a deterministic validation error into allocation
    // pressure or std::length_error.
    if (options->reports == nullptr || options->report_count == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "hid.reports",
                  "Raw descriptors require an explicit accepted-report table.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (options->report_count > 256U) {
        set_error(MIMUSB_ERR_OVERFLOW, "hid.reports",
                  "The accepted-report table exceeds the 256 possible Report IDs.");
        return MIMUSB_ERR_OVERFLOW;
    }
    std::vector<std::uint8_t> trailing_valid_masks(options->report_count, std::uint8_t{0xFFU});
    mimusb::hid::DescriptorRequirements requirements{};
    const mimusb::hid::ValidationIssue validation = mimusb::hid::validate_raw(
        options->descriptor, options->descriptor_length, options->reports, options->report_count,
        trailing_valid_masks.data(), &requirements, options->requires_output == 1U,
        options->requires_feature_response == 1U);
    if (validation.result != MIMUSB_OK) {
        set_error(validation.result, validation.field, validation.reason, 0, 0, 0, 0,
                  static_cast<std::uint32_t>(validation.offset), 0);
        return validation.result;
    }
    auto spec = std::make_unique<mimusb::detail::HidSpec>();
    spec->kind = MIMUSB_PROFILE_HID;
    spec->requirements = requirements;
    spec->descriptor.assign(options->descriptor, options->descriptor + options->descriptor_length);
    mimusb::detail::RawConfig config{};
    config.reports.assign(options->reports, options->reports + options->report_count);
    config.trailing_valid_masks = std::move(trailing_valid_masks);
    for (const mimusb_hid_report& report : config.reports) {
        spec->report_capabilities.push_back(
            mimusb_report_capability{report.report_id, report.wire_length});
    }
    spec->config = std::move(config);
    *out_spec = mimusb::detail::make_hid_spec(std::move(spec));
    mimusb::detail::clear_error();
    return MIMUSB_OK;
}

mimusb_result MIMUSB_CALL mimusb_spec_create_hid(const mimusb_hid_options* options,
                                                 mimusb_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_hid", [&] {
        return create_hid_spec(options, out_spec,
                               [&] { return mimusb_spec_create_hid_impl(options, out_spec); });
    });
}

} // extern "C"
