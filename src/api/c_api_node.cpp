// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: opening, configuring and closing Nodes. A Node is what its Spec's class builds at
 * the placement (FunctionClass::build); opening, committing and closing it is the same for every
 * class.
 */

#include "api/support.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <utility>

using mimusb::detail::commit_device;
using mimusb::detail::CommitWaits;
using mimusb::detail::public_node_id;
using mimusb::detail::read_commit_timeouts;
using mimusb::detail::require_timeout;
using mimusb::detail::reserved_is_zero;
using mimusb::detail::set_error;
using mimusb::detail::valid_usb_string;

extern "C" {

// ---- nodes -----------------------------------------------------------------------------

// The rules a Node of a placement has besides those its Spec carries: the placement's extra_rules.
struct ExtraRules {
    const mimusb_control_rule* rules{};
    std::size_t count{};
};

// A Node of `spec` as its class built it at the placement: its interfaces, its rules (the
// placement's extra rules first) and its runtime, checked alone, then committed with the others.
static mimusb_result open_built(mimusb_device* device, mimusb_spec* spec,
                                mimusb::detail::NodeBuild* build, const ExtraRules& extra,
                                const mimusb_commit_timeouts* timeouts, mimusb_node** out_node) {
    const mimusb_node_config* config = build->config;
    const mimusb::detail::RuleSlots& slots = build->slots;
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (!mimusb::detail::valid_struct(config, config == nullptr ? 0U : config->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_node_config)),
                                      "node_config")) {
        return MIMUSB_ERR_PARAM;
    }
    if (!reserved_is_zero(config->reserved, "node_config.reserved"))
        return MIMUSB_ERR_PARAM;
    if (config->interfaces == nullptr || config->interface_count == 0U ||
        config->interface_count > device->board.max_interfaces_any ||
        (config->association == nullptr) != (config->association_length == 0U)) {
        set_error(MIMUSB_ERR_PARAM, "node_config.interfaces",
                  "A Node has one or more interfaces (at most the board's max_interfaces_any); "
                  "association and association_length go together.");
        return MIMUSB_ERR_PARAM;
    }
    auto node = std::unique_ptr<mimusb_node>(new (std::nothrow) mimusb_node());
    if (node == nullptr) {
        set_error(MIMUSB_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return MIMUSB_ERR_INTERNAL;
    }
    node->device = device;
    {
        // The caller's rules come first: a request is answered by the first rule that matches.
        std::vector<mimusb_control_rule> combined;
        combined.reserve(extra.count + config->rule_count);
        if (extra.count != 0U)
            combined.insert(combined.end(), extra.rules, extra.rules + extra.count);
        if (config->rule_count != 0U)
            combined.insert(combined.end(), config->rules, config->rules + config->rule_count);
        if (const mimusb_result r = mimusb::detail::copy_control_rules(
                device, combined.data(), combined.size(), "node_config.rules", &node->rules);
            r != MIMUSB_OK)
            return r;
        for (const auto& [slot, index] : slots)
            node->rule_slots.emplace_back(slot, extra.count + index);
    }
    if (config->association != nullptr)
        node->association.assign(config->association,
                                 config->association + config->association_length);
    for (std::size_t i = 0U; i < config->interface_count; ++i) {
        const mimusb_usb_interface& in = config->interfaces[i];
        // The class is the caller's: the library reads it only for this refusal. Class 0 is
        // "defined at the device level" (USB 2.0 9.6.5 / the class codes of the USB-IF), never an
        // interface the caller forgot.
        if (in.class_code == 0U) {
            set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.class_code",
                      "class_code is required.", 0, 0, 0, 0, in.number, 0);
            return MIMUSB_ERR_UNSET_FIELD;
        }
        if (in.settings == nullptr || in.setting_count == 0U || in.setting_count > 255U) {
            set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings",
                      "An interface has 1 to 255 settings (bAlternateSetting 0, 1, 2, ... in "
                      "order); settings must not be null.",
                      0, 0, 0, 0, in.number, static_cast<std::uint32_t>(in.setting_count));
            return MIMUSB_ERR_PARAM;
        }
        if (!valid_usb_string(in.name)) {
            set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.name",
                      "The name must be UTF-8 without control characters, at most 126 UTF-16 code "
                      "units.");
            return MIMUSB_ERR_PARAM;
        }
        mimusb::detail::GenericInterface copy;
        copy.number = in.number;
        copy.class_code = in.class_code;
        copy.subclass = in.subclass;
        copy.protocol = in.protocol;
        copy.name = in.name != nullptr ? in.name : "";
        for (std::size_t s = 0U; s < in.setting_count; ++s) {
            const mimusb_usb_setting& setting = in.settings[s];
            if (setting.items == nullptr && setting.item_count != 0U) {
                set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.items",
                          "items must not be null with a nonzero item_count.", 0, 0, 0, 0,
                          in.number, static_cast<std::uint32_t>(s));
                return MIMUSB_ERR_PARAM;
            }
            mimusb::usb::ComposerSetting made_setting;
            for (std::size_t k = 0U; k < setting.item_count; ++k) {
                const mimusb_usb_item& item = setting.items[k];
                mimusb::usb::ComposerItem made_item;
                if (item.reserved != 0U) {
                    set_error(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.items.reserved",
                              "reserved must be zero.", 0, 0, 0, 0, in.number,
                              static_cast<std::uint32_t>(k));
                    return MIMUSB_ERR_PARAM;
                }
                if (item.kind == MIMUSB_ITEM_ENDPOINT) {
                    if (item.descriptor != nullptr || item.descriptor_length != 0U) {
                        set_error(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.items.descriptor",
                                  "An endpoint item leaves descriptor and descriptor_length "
                                  "zero.",
                                  0, 0, 0, 0, in.number, static_cast<std::uint32_t>(k));
                        return MIMUSB_ERR_PARAM;
                    }
                    made_item.is_endpoint = true;
                    made_item.endpoint = item.endpoint;
                } else if (item.kind == MIMUSB_ITEM_DESCRIPTOR) {
                    mimusb_usb_endpoint zero{};
                    if (item.descriptor == nullptr ||
                        std::memcmp(&item.endpoint, &zero, sizeof(zero)) != 0) {
                        set_error(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.items.descriptor",
                                  "A descriptor item has a descriptor and leaves endpoint all "
                                  "zero.",
                                  0, 0, 0, 0, in.number, static_cast<std::uint32_t>(k));
                        return MIMUSB_ERR_PARAM;
                    }
                    made_item.descriptor.assign(item.descriptor,
                                                item.descriptor + item.descriptor_length);
                } else {
                    set_error(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.settings.items.kind",
                              "kind is MIMUSB_ITEM_ENDPOINT or MIMUSB_ITEM_DESCRIPTOR.", 0, 0, 0, 0,
                              in.number, static_cast<std::uint32_t>(k));
                    return MIMUSB_ERR_UNSET_FIELD;
                }
                made_setting.items.push_back(std::move(made_item));
            }
            copy.settings.push_back(std::move(made_setting));
        }
        // One runtime per IN address, over all settings: an address that comes again in another
        // setting is the same endpoint (the composer checks they agree). The largest packet is the
        // bound of a write, and an isochronous endpoint takes one packet per write.
        copy.for_each_endpoint([&](const mimusb_usb_endpoint& ep) {
            if ((ep.address & 0x80U) == 0U || ep.policy.on_full == 0U || ep.policy.depth == 0U)
                return;
            for (mimusb::detail::EndpointRuntime& known : node->in_endpoints) {
                if (known.address == ep.address) {
                    known.max_packet_size = std::max(known.max_packet_size, ep.max_packet_size);
                    return;
                }
            }
            mimusb::detail::EndpointRuntime runtime;
            runtime.address = ep.address;
            runtime.max_packet_size = ep.max_packet_size;
            runtime.isochronous = ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS;
            runtime.policy = ep.policy;
            node->in_endpoints.push_back(runtime);
        });
        node->interface_numbers.push_back(copy.number);
        copy.for_each_endpoint([&](const mimusb_usb_endpoint& ep) {
            if (std::find(node->endpoint_addresses.begin(), node->endpoint_addresses.end(),
                          ep.address) == node->endpoint_addresses.end())
                node->endpoint_addresses.push_back(ep.address);
        });
        node->interfaces.push_back(std::move(copy));
    }
    // The interface checks of the composer on this Node alone; the ones across Nodes (numbers
    // without a gap, a repeated address) are made at the commit, before anything is sent.
    {
        std::vector<mimusb::usb::ComposerGroup> groups(1);
        groups[0].association = node->association;
        for (const mimusb::detail::GenericInterface& itf : node->interfaces) {
            mimusb::usb::ComposerInterface made;
            made.number = itf.number;
            made.class_code = itf.class_code;
            made.subclass = itf.subclass;
            made.protocol = itf.protocol;
            mimusb::detail::utf16_from_utf8(itf.name.c_str(), &made.name);
            made.settings = itf.settings;
            groups[0].interfaces.push_back(std::move(made));
        }
        if (const mimusb_result r = mimusb::detail::precheck_groups(device, std::move(groups));
            r != MIMUSB_OK)
            return r;
    }
    std::uint16_t node_id = 0U;
    if (!mimusb::detail::free_node_id(device, &node_id)) {
        set_error(MIMUSB_ERR_OVERFLOW, "node.open", "The Device holds no more Nodes.");
        return MIMUSB_ERR_OVERFLOW;
    }
    node->node_id = node_id;
    node->iface = node->interfaces.front().number;
    node->runtime = std::move(build->runtime);
    if (node->runtime != nullptr)
        node->runtime->owner = node.get();
    node->spec = spec;
    mimusb::detail::retain_spec(spec);

    mimusb_node* published = node.get();
    mimusb::detail::publish_node(device, published);
    device->dirty = true;
    if (device->batch_open) {
        published->staged_only = true;
        *out_node = node.release();
        return MIMUSB_OK;
    }
    const mimusb_result committed = commit_device(device, 0U, waits);
    if (committed != MIMUSB_OK) {
        // The board's active configuration is unchanged or unknown; drop this Node again.
        mimusb::detail::unpublish_node(device, published);
        mimusb::detail::release_spec(spec);
        node->spec = nullptr;
        if (device->configured)
            commit_device(device, 0U, waits);
        return committed;
    }
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "node opened");
    *out_node = node.release();
    return MIMUSB_OK;
}

mimusb_result MIMUSB_CALL mimusb_node_open(mimusb_device* device, mimusb_spec* spec,
                                           const mimusb_node_placement* placement,
                                           const mimusb_commit_timeouts* timeouts,
                                           mimusb_node** out_node) try {
    mimusb::detail::clear_error();
    if (out_node == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_node", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_node = nullptr;
    if (device == nullptr || spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node.open", "A live Device and a Spec are required.");
        return MIMUSB_ERR_PARAM;
    }
    {
        CommitWaits waits;
        if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
            return r;
    }
    if (!mimusb::detail::valid_struct(placement, placement == nullptr ? 0U : placement->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_node_placement)),
                                      "placement")) {
        return MIMUSB_ERR_PARAM;
    }
    bool reserved_zero = placement->reserved == 0U;
    for (const std::uint8_t byte : placement->reserved8)
        reserved_zero = reserved_zero && byte == 0U;
    if (!reserved_zero) {
        set_error(MIMUSB_ERR_PARAM, "placement.reserved", "The reserved fields must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if ((placement->endpoints == nullptr) != (placement->endpoint_count == 0U) ||
        placement->endpoint_count > 64U) {
        set_error(MIMUSB_ERR_PARAM, "placement.endpoints",
                  "endpoints is null exactly when endpoint_count is 0, and there are at most 64.");
        return MIMUSB_ERR_PARAM;
    }
    if ((placement->extra_rules == nullptr) != (placement->extra_rule_count == 0U) ||
        placement->extra_rule_count > 65535U) {
        set_error(MIMUSB_ERR_PARAM, "placement.extra_rules",
                  "extra_rules is null exactly when extra_rule_count is 0, and there are at most "
                  "65535.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "The Device handle was consumed by close.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    if (device->descriptors_set) {
        set_error(MIMUSB_ERR_BUSY, "node.open",
                  "The Device is given as bytes (mimusb_device_set_descriptors); clear them "
                  "before opening a Node.");
        return MIMUSB_ERR_BUSY;
    }
    // The class makes the Node's interfaces, rules and runtime; the rest is the same for all.
    mimusb::detail::NodeBuild build;
    if (const mimusb_result r = spec->function_class->build(device, spec, *placement, &build);
        r != MIMUSB_OK)
        return r;
    const ExtraRules extra{placement->extra_rules, placement->extra_rule_count};
    return open_built(device, spec, &build, extra, timeouts, out_node);
}
MIMUSB_C_RESULT_CATCH("node.open")

mimusb_result MIMUSB_CALL mimusb_node_set_rule_data(mimusb_node* node, const size_t rule_index,
                                                    const uint8_t* data, const size_t length,
                                                    const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    return mimusb::detail::change_rule_data(node->device, node, rule_index, data, length,
                                            control_timeout_ms);
}
MIMUSB_C_RESULT_CATCH("node.set_rule_data")

mimusb_result MIMUSB_CALL mimusb_node_find_rule(const mimusb_node* node,
                                                const mimusb_rule_slot slot,
                                                size_t* out_rule_index) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || out_rule_index == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node and out_rule_index are required.");
        return MIMUSB_ERR_PARAM;
    }
    *out_rule_index = 0U;
    for (const auto& [named, index] : node->rule_slots) {
        if (named == slot) {
            *out_rule_index = index;
            return MIMUSB_OK;
        }
    }
    set_error(MIMUSB_ERR_UNSUPPORTED, "slot", "The Node's Spec has no rule of this slot.", 0, 0, 0,
              0, 0, static_cast<std::uint32_t>(slot));
    return MIMUSB_ERR_UNSUPPORTED;
}
MIMUSB_C_RESULT_CATCH("node.find_rule")

uint16_t MIMUSB_CALL mimusb_node_id(const mimusb_node* node) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return 0U;
    }
    return public_node_id(node);
}
MIMUSB_C_VALUE_CATCH("node.id", 0U)

mimusb_result MIMUSB_CALL mimusb_node_manifest(const mimusb_node* node,
                                               mimusb_capability_manifest* manifest) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "node", "A live Node is required.");
        return MIMUSB_ERR_PARAM;
    }
    return mimusb_spec_manifest(node->spec, manifest);
}
MIMUSB_C_RESULT_CATCH("node.manifest")

mimusb_result MIMUSB_CALL mimusb_node_close(mimusb_node* node,
                                            const mimusb_commit_timeouts* timeouts,
                                            const uint32_t drain_timeout_ms) try {
    mimusb::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "node.close",
                  "The Node handle is null, closed, detached, or owned by a closing Device.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (const mimusb_result t = require_timeout(drain_timeout_ms, "drain_timeout_ms");
        t != MIMUSB_OK)
        return t;
    mimusb_device* device = node->device;
    // From here the Node goes whatever fails; the first failure is what the caller hears about.
    const mimusb_result neutral = mimusb::detail::before_close(node, drain_timeout_ms);
    const mimusb_error_detail neutral_detail = *mimusb_last_error();
    mimusb::detail::unpublish_node(device, node);
    device->dirty = true;
    mimusb_result committed = MIMUSB_OK;
    if (!device->batch_open && !node->staged_only)
        committed = commit_device(device, 0U, waits);
    node->closed = true;
    mimusb::detail::release_spec(node->spec);
    node->spec = nullptr;
    delete node;
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "node closed");
    if (neutral != MIMUSB_OK) {
        mimusb::detail::restore_error(neutral_detail);
        return neutral;
    }
    if (committed != MIMUSB_OK)
        return committed;
    mimusb::detail::clear_error();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("node.close")

} // extern "C"
