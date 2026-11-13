// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * The one shape every function class has. A class (raw interfaces, CDC-ACM, HID, ...) turns a Spec
 * and a placement into interfaces and control rules when a Node is opened, lays the interfaces of
 * its Nodes out when the Device is composed, and hears the requests its rules take and the packets
 * that arrive on its endpoints. The device layer (src/api) knows no class: it reaches a class only
 * through FunctionClass, and a Node's runtime only through NodeClass.
 *
 * A new class is one file under src/classes/ with a FunctionClass of its own and a
 * mimusb_spec_create_* factory that makes Specs of it.
 */

#include "mimusb.h"
#include "usb/board_rule.hpp"
#include "usb/descriptor_composer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct mimusb_device;
struct mimusb_node;
struct mimusb_spec;

namespace mimusb::detail {

// What a Spec of one class keeps. Each class derives its own and reads it back with a static_cast
// once FunctionClass says the Spec is of that class (the library is built without RTTI).
struct SpecData {
    SpecData() = default;
    SpecData(const SpecData&) = delete;
    SpecData& operator=(const SpecData&) = delete;
    virtual ~SpecData() = default;
};

// Interfaces in the public form (mimusb_node_config) with every pointer into storage this object
// owns. The containers never move their elements, so `view` points into them for as long as the
// object lives. A Spec of mimusb_spec_create_interfaces is one; every class builds one for a Node.
struct InterfacesSpec : SpecData {
    std::vector<mimusb_usb_interface> interfaces;
    std::deque<std::vector<mimusb_usb_setting>> settings;
    std::deque<std::vector<mimusb_usb_item>> items;
    std::deque<std::vector<std::uint8_t>> bytes; // descriptors, rule data
    std::deque<std::string> names;
    std::vector<std::uint8_t> association;
    std::vector<mimusb_control_rule> rules;
    mimusb_node_config view{};

    // Copies `length` bytes into storage the object owns and returns where they are.
    const std::uint8_t* keep(const std::uint8_t* data, std::size_t length) {
        bytes.emplace_back(data, data + length);
        return bytes.back().data();
    }
    // Points `view` at the containers; called once they are filled.
    void finish() {
        view = mimusb_node_config{};
        view.struct_size = static_cast<std::uint32_t>(sizeof(mimusb_node_config));
        view.interfaces = interfaces.empty() ? nullptr : interfaces.data();
        view.interface_count = interfaces.size();
        view.association = association.empty() ? nullptr : association.data();
        view.association_length = association.size();
        view.rules = rules.empty() ? nullptr : rules.data();
        view.rule_count = rules.size();
    }
};

// A control rule a Spec names (mimusb_rule_slot) and its place among the rules the Spec brings.
using RuleSlots = std::vector<std::pair<mimusb_rule_slot, std::size_t>>;

// The runtime a Node of a class that keeps a state has (a HID Node's report state). A class
// without a state gives its Nodes none.
class NodeClass {
  public:
    NodeClass() = default;
    NodeClass(const NodeClass&) = delete;
    NodeClass& operator=(const NodeClass&) = delete;
    virtual ~NodeClass() = default;
    mimusb_node* owner{}; // the Node this is the runtime of, set when the Node is made
    // What the target must see before the Node goes (a HID Node releases what it holds pressed).
    virtual mimusb_result before_close(std::uint32_t timeout_ms) {
        (void)timeout_ms;
        return MIMUSB_OK;
    }
};

// What a class makes of a Spec at a placement when a Node is opened.
struct NodeBuild {
    // The Node's interfaces, association and own rules: `config`, which points into the Spec or
    // into `storage` when the class made them for this placement.
    const mimusb_node_config* config{};
    InterfacesSpec storage;
    RuleSlots slots; // slot -> index in config->rules
    std::unique_ptr<NodeClass> runtime;
};

// The bus layout of the live Nodes of one class, made at each commit: the interface groups for the
// composer, and the rules the class writes besides the Nodes' own.
struct Composition {
    std::vector<usb::ComposerGroup> groups;
    std::vector<usb::BoardRule> rules;
};

// A data stage the device layer waits for on behalf of a class: the rule that took the request.
struct PendingData {
    std::uint8_t token{};
    const FunctionClass* owner{};
    std::uint8_t tag{};
    std::uint8_t iface{};
    std::uint16_t value{};
};

// What a class does with a request one of its rules took.
enum class Taken : std::uint8_t {
    pass,     // nothing: the program hears of it as the control transfer it is
    deliver,  // the class filled the event
    wait_data // the class waits for the data stage (PendingData), and nothing is delivered yet
};

class FunctionClass {
  public:
    FunctionClass() = default;
    FunctionClass(const FunctionClass&) = delete;
    FunctionClass& operator=(const FunctionClass&) = delete;
    virtual ~FunctionClass() = default;

    // mimusb_spec_interface_count, mimusb_spec_roles and mimusb_spec_rule_cost.
    virtual std::size_t interface_count(const mimusb_spec& spec) const = 0;
    virtual std::size_t roles(const mimusb_spec& spec, mimusb_role_info* out,
                              std::size_t capacity) const = 0;
    virtual void rule_cost(const mimusb_spec& spec, std::size_t* rules,
                           std::size_t* answer_bytes) const = 0;

    // The report descriptor and the reports of a Spec (mimusb_spec_descriptor, _manifest); none
    // for a class without reports.
    virtual void reports(const mimusb_spec& spec, const std::vector<std::uint8_t>** descriptor,
                         const std::vector<mimusb_report_capability>** reports) const {
        (void)spec;
        *descriptor = nullptr;
        *reports = nullptr;
    }

    // A Node of `spec` at `placement` on `device`: its interfaces and rules, and its runtime.
    virtual mimusb_result build(mimusb_device* device, mimusb_spec* spec,
                                const mimusb_node_placement& placement, NodeBuild* out) const = 0;

    // The layout of the live Nodes of this class, in node order. Each Node's interfaces are one
    // group, and the class writes no rule; a class whose Nodes share interfaces joins them here.
    virtual mimusb_result compose(const std::vector<mimusb_node*>& nodes, Composition* out,
                                  usb::ComposerError* error) const;

    // The reader thread: a request an ACCEPT rule of this class took (rule.tag names it), and the
    // data stage of one it waits for. `setup` is the 8-byte setup packet.
    virtual Taken on_accepted(mimusb_device* device, const usb::BoardRule& rule,
                              const std::uint8_t* setup, mimusb_event* event) const {
        (void)device, (void)rule, (void)setup, (void)event;
        return Taken::pass;
    }
    virtual bool on_control_data(mimusb_device* device, const PendingData& pending,
                                 const std::uint8_t* data, std::size_t length,
                                 mimusb_event* event) const {
        (void)device, (void)pending, (void)data, (void)length, (void)event;
        return false;
    }
    // The reader thread: a packet on an OUT endpoint of a Node of this class, on interface
    // `iface`. True when the class filled the event; otherwise the program hears of it as
    // MIMUSB_EVENT_ENDPOINT_DATA.
    virtual bool on_endpoint_data(mimusb_device* device, std::uint8_t iface, std::uint8_t address,
                                  const std::uint8_t* data, std::size_t length,
                                  mimusb_event* event) const {
        (void)device, (void)iface, (void)address, (void)data, (void)length, (void)event;
        return false;
    }
};

// The classes this library has.
const FunctionClass& interfaces_class() noexcept;
const FunctionClass& cdc_acm_class() noexcept;
const FunctionClass& hid_class() noexcept;

// The group a Node's own interfaces make, as the composer takes it.
usb::ComposerGroup group_of_node(const mimusb_node& node);

} // namespace mimusb::detail
