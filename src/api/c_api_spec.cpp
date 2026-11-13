// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: what every Spec answers, whatever its class (its reference count, its interfaces,
 * roles and rules, its report descriptor and manifest). Each answer comes from the Spec's
 * FunctionClass; the factories that make Specs live with their classes in src/classes/.
 */

#include "api/support.hpp"

#include <limits>

using mimusb::detail::set_error;

namespace mimusb::detail {

bool retain_spec(mimusb_spec* spec) noexcept {
    if (spec == nullptr) {
        return false;
    }
    std::uint32_t references = spec->references.load(std::memory_order_relaxed);
    while (references != std::numeric_limits<std::uint32_t>::max()) {
        if (spec->references.compare_exchange_weak(references, references + 1U,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool release_spec(mimusb_spec* spec) noexcept {
    if (spec == nullptr) {
        return false;
    }
    std::uint32_t references = spec->references.load(std::memory_order_acquire);
    while (references != 0U) {
        if (spec->references.compare_exchange_weak(references, references - 1U,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            if (references == 1U) {
                delete spec;
            }
            return true;
        }
    }
    return false;
}

usb::ComposerGroup group_of_node(const mimusb_node& node) {
    usb::ComposerGroup group;
    group.node_id = public_node_id(&node);
    group.association = node.association;
    for (const GenericInterface& itf : node.interfaces) {
        usb::ComposerInterface made;
        made.number = itf.number;
        made.class_code = itf.class_code;
        made.subclass = itf.subclass;
        made.protocol = itf.protocol;
        utf16_from_utf8(itf.name.c_str(), &made.name);
        made.settings = itf.settings;
        group.interfaces.push_back(std::move(made));
    }
    return group;
}

mimusb_result FunctionClass::compose(const std::vector<mimusb_node*>& nodes, Composition* out,
                                     usb::ComposerError* error) const {
    (void)error;
    for (const mimusb_node* node : nodes)
        out->groups.push_back(group_of_node(*node));
    return MIMUSB_OK;
}

} // namespace mimusb::detail

extern "C" {

void MIMUSB_CALL mimusb_spec_retain(mimusb_spec* spec) try {
    mimusb::detail::clear_error();
    if (!mimusb::detail::retain_spec(spec)) {
        set_error(spec == nullptr ? MIMUSB_ERR_PARAM : MIMUSB_ERR_OVERFLOW, "spec.retain",
                  spec == nullptr ? "The spec handle is null."
                                  : "The spec reference count cannot be incremented further.");
    }
}
MIMUSB_C_VOID_CATCH("spec.retain")

void MIMUSB_CALL mimusb_spec_release(mimusb_spec* spec) try {
    mimusb::detail::clear_error();
    if (!mimusb::detail::release_spec(spec)) {
        set_error(MIMUSB_ERR_PARAM, "spec.release",
                  "The spec handle is null or has no owned reference to release.");
    }
}
MIMUSB_C_VOID_CATCH("spec.release")

mimusb_result MIMUSB_CALL mimusb_spec_descriptor(const mimusb_spec* spec,
                                                 const std::uint8_t** bytes,
                                                 std::size_t* length) try {
    mimusb::detail::clear_error();
    if (bytes != nullptr)
        *bytes = nullptr;
    if (length != nullptr)
        *length = 0U;
    if (spec == nullptr || bytes == nullptr || length == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "spec", "A required pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    const std::vector<std::uint8_t>* descriptor = nullptr;
    const std::vector<mimusb_report_capability>* reports = nullptr;
    spec->function_class->reports(*spec, &descriptor, &reports);
    if (descriptor != nullptr) {
        *bytes = descriptor->data();
        *length = descriptor->size();
    }
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.descriptor")

mimusb_result MIMUSB_CALL mimusb_spec_manifest(const mimusb_spec* spec,
                                               mimusb_capability_manifest* manifest) try {
    mimusb::detail::clear_error();
    if (spec == nullptr || manifest == nullptr || manifest->struct_size != sizeof(*manifest) ||
        manifest->reserved != 0U) {
        set_error(MIMUSB_ERR_PARAM, "manifest",
                  "The manifest pointer, struct_size, or reserved field is invalid.");
        return MIMUSB_ERR_PARAM;
    }
    const std::vector<std::uint8_t>* descriptor = nullptr;
    const std::vector<mimusb_report_capability>* reports = nullptr;
    spec->function_class->reports(*spec, &descriptor, &reports);
    manifest->input_supported = 1U;
    // The library delivers the target's Output and Feature requests as events (they are taken by
    // ACCEPT rules it writes) and answers Feature reads from the responses registered with
    // mimusb_node_set_feature_response().
    manifest->output_supported = 1U;
    manifest->feature_transport_supported = 1U;
    manifest->profile_kind = spec->kind;
    manifest->reports = reports != nullptr ? reports->data() : nullptr;
    manifest->report_count = reports != nullptr ? reports->size() : 0U;
    manifest->descriptor_length = descriptor != nullptr ? descriptor->size() : 0U;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.manifest")

mimusb_result MIMUSB_CALL mimusb_spec_roles(const mimusb_spec* spec, mimusb_role_info* out_roles,
                                            const std::size_t capacity,
                                            std::size_t* out_count) try {
    mimusb::detail::clear_error();
    if (spec == nullptr || out_count == nullptr || (out_roles == nullptr && capacity != 0U)) {
        set_error(MIMUSB_ERR_PARAM, "spec.roles",
                  "spec and out_count are required, and out_roles with a capacity.");
        return MIMUSB_ERR_PARAM;
    }
    *out_count = spec->function_class->roles(*spec, out_roles, capacity);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.roles")

mimusb_result MIMUSB_CALL mimusb_spec_interface_count(const mimusb_spec* spec,
                                                      size_t* out_count) try {
    mimusb::detail::clear_error();
    if (spec == nullptr || out_count == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "spec", "spec and out_count are required.");
        return MIMUSB_ERR_PARAM;
    }
    *out_count = spec->function_class->interface_count(*spec);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.interface_count")

mimusb_result MIMUSB_CALL mimusb_spec_rule_cost(const mimusb_spec* spec, size_t* out_rules,
                                                size_t* out_answer_bytes) try {
    mimusb::detail::clear_error();
    if (spec == nullptr || out_rules == nullptr || out_answer_bytes == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "spec", "spec and both outputs are required.");
        return MIMUSB_ERR_PARAM;
    }
    *out_rules = 0U;
    *out_answer_bytes = 0U;
    spec->function_class->rule_cost(*spec, out_rules, out_answer_bytes);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.rule_cost")

} // extern "C"
