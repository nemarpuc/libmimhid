// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Interfaces of any class, as the caller describes them (mimusb_spec_create_interfaces): the class
 * that reads no class. Its Spec carries the interface numbers, names, endpoints and rules, so the
 * placement gives nothing but extra_rules.
 */

#include "api/support.hpp"

#include <memory>

using mimusb::detail::InterfacesSpec;
using mimusb::detail::set_error;

namespace {

const InterfacesSpec& data_of(const mimusb_spec& spec) {
    return *static_cast<const InterfacesSpec*>(spec.data.get());
}

class InterfacesClass final : public mimusb::detail::FunctionClass {
  public:
    std::size_t interface_count(const mimusb_spec& spec) const override {
        return data_of(spec).view.interface_count;
    }
    std::size_t roles(const mimusb_spec&, mimusb_role_info*, std::size_t) const override {
        return 0U;
    }
    void rule_cost(const mimusb_spec& spec, std::size_t* rules,
                   std::size_t* answer_bytes) const override {
        const mimusb_node_config& view = data_of(spec).view;
        for (std::size_t i = 0U; i < view.rule_count; ++i) {
            ++*rules;
            if (view.rules[i].action == MIMUSB_CONTROL_ANSWER)
                *answer_bytes += view.rules[i].length;
        }
    }
    mimusb_result build(mimusb_device*, mimusb_spec* spec, const mimusb_node_placement& placement,
                        mimusb::detail::NodeBuild* out) const override {
        if (placement.first_interface != 0U || placement.name != nullptr ||
            placement.endpoint_count != 0U) {
            set_error(MIMUSB_ERR_PARAM, "placement",
                      "A Spec of mimusb_spec_create_interfaces carries its own interface numbers, "
                      "names and endpoints: first_interface, name and endpoints are zero.");
            return MIMUSB_ERR_PARAM;
        }
        out->config = &data_of(*spec).view;
        return MIMUSB_OK;
    }
};

const InterfacesClass kInterfaces{};

} // namespace

namespace mimusb::detail {

const FunctionClass& interfaces_class() noexcept { return kInterfaces; }

} // namespace mimusb::detail

extern "C" {

mimusb_result MIMUSB_CALL mimusb_spec_create_interfaces(const mimusb_node_config* config,
                                                        mimusb_spec** out_spec) try {
    mimusb::detail::clear_error();
    if (out_spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_spec", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_spec = nullptr;
    if (!mimusb::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_node_config)),
                                      "node_config")) {
        return MIMUSB_ERR_PARAM;
    }
    if (config->reserved != 0U) {
        set_error(MIMUSB_ERR_PARAM, "node_config.reserved", "The reserved field must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if ((config->interfaces == nullptr) != (config->interface_count == 0U) ||
        (config->association == nullptr) != (config->association_length == 0U) ||
        (config->rules == nullptr) != (config->rule_count == 0U)) {
        set_error(MIMUSB_ERR_PARAM, "node_config",
                  "A pointer is null with a count, or a count is zero with a pointer.");
        return MIMUSB_ERR_PARAM;
    }
    if (config->interface_count > 255U || config->rule_count > 65535U ||
        config->association_length > 65535U) {
        set_error(MIMUSB_ERR_OVERFLOW, "node_config",
                  "At most 255 interfaces, 65535 rules and an association of 65535 bytes.");
        return MIMUSB_ERR_OVERFLOW;
    }
    auto owned = std::make_unique<InterfacesSpec>();
    owned->interfaces.reserve(config->interface_count);
    for (std::size_t i = 0U; i < config->interface_count; ++i) {
        mimusb_usb_interface itf = config->interfaces[i];
        if ((itf.settings == nullptr) != (itf.setting_count == 0U) || itf.setting_count > 255U) {
            set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings",
                      "An interface has 1 to 255 settings, and settings is not null.", 0, 0, 0, 0,
                      itf.number, static_cast<std::uint32_t>(itf.setting_count));
            return MIMUSB_ERR_PARAM;
        }
        if (itf.name != nullptr) {
            owned->names.emplace_back(itf.name);
            itf.name = owned->names.back().c_str();
        }
        std::vector<mimusb_usb_setting> settings;
        settings.reserve(itf.setting_count);
        for (std::size_t k = 0U; k < itf.setting_count; ++k) {
            mimusb_usb_setting setting = itf.settings[k];
            if ((setting.items == nullptr) != (setting.item_count == 0U) ||
                setting.item_count > 255U) {
                set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.items",
                          "A setting has 0 to 255 items, and items is not null with a count.", 0, 0,
                          0, 0, itf.number, static_cast<std::uint32_t>(k));
                return MIMUSB_ERR_PARAM;
            }
            std::vector<mimusb_usb_item> items;
            items.reserve(setting.item_count);
            for (std::size_t j = 0U; j < setting.item_count; ++j) {
                mimusb_usb_item item = setting.items[j];
                if (item.descriptor != nullptr || item.descriptor_length != 0U) {
                    if (item.descriptor == nullptr || item.descriptor_length == 0U ||
                        item.descriptor_length > 65535U) {
                        set_error(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.items.descriptor",
                                  "A descriptor is not null and 1 to 65535 bytes, or both are "
                                  "zero.",
                                  0, 0, 0, 0, itf.number, static_cast<std::uint32_t>(j));
                        return MIMUSB_ERR_PARAM;
                    }
                    item.descriptor = owned->keep(item.descriptor, item.descriptor_length);
                }
                items.push_back(item);
            }
            owned->items.push_back(std::move(items));
            setting.items = owned->items.back().empty() ? nullptr : owned->items.back().data();
            settings.push_back(setting);
        }
        owned->settings.push_back(std::move(settings));
        itf.settings = owned->settings.back().data();
        owned->interfaces.push_back(itf);
    }
    if (config->association != nullptr)
        owned->association.assign(config->association,
                                  config->association + config->association_length);
    owned->rules.reserve(config->rule_count);
    for (std::size_t i = 0U; i < config->rule_count; ++i) {
        mimusb_control_rule rule = config->rules[i];
        if (rule.data != nullptr && rule.length != 0U)
            rule.data = owned->keep(rule.data, rule.length);
        owned->rules.push_back(rule);
    }
    owned->finish();
    // The caller's view, as it was given, pointing at the copies.
    const mimusb_node_config made = owned->view;
    owned->view = *config;
    owned->view.interfaces = made.interfaces;
    owned->view.association = made.association;
    owned->view.rules = made.rules;
    auto spec = std::make_unique<mimusb_spec>();
    spec->kind = MIMUSB_PROFILE_INTERFACES;
    spec->function_class = &kInterfaces;
    spec->data = std::move(owned);
    *out_spec = spec.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.create_interfaces")

} // extern "C"
