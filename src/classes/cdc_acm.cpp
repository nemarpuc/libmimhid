// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The CDC-ACM function (USB CDC 1.2 5.2.3; PSTN 1.2 5.3, 6.3) as a Spec: a class that keeps no
 * state, so it is only made of interfaces and control rules at the placement of its Node.
 */

#include "api/support.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

using mimusb::detail::set_error;

namespace {

constexpr std::uint8_t kCsInterface = 0x24U; // CDC 1.2 5.2.3: CS_INTERFACE
constexpr std::uint8_t kInterfaceAssociation = 0x0BU;
constexpr std::uint8_t kClassCdcControl = 0x02U;
constexpr std::uint8_t kSubclassAcm = 0x02U;
constexpr std::uint8_t kClassCdcData = 0x0AU;

// PSTN 1.2 6.3 class requests. bmRequestType 0x21: host to device, class, interface; 0xA1: device
// to host.
constexpr std::uint8_t kSetLineCoding = 0x20U;
constexpr std::uint8_t kGetLineCoding = 0x21U;
constexpr std::uint8_t kSetControlLineState = 0x22U;

// A Spec made with mimusb_spec_create_cdc_acm: its options, with the data interface's name kept.
// The interfaces, descriptors and rules are made when a Node is opened, at its placement.
struct CdcAcmSpec : mimusb::detail::SpecData {
    mimusb_cdc_acm_options options{};
    std::string data_name;
};

const CdcAcmSpec& data_of(const mimusb_spec& spec) {
    return *static_cast<const CdcAcmSpec*>(spec.data.get());
}

mimusb_usb_item descriptor_item(mimusb::detail::InterfacesSpec* out, const std::uint8_t* data,
                                const std::size_t length) {
    mimusb_usb_item item{};
    item.kind = MIMUSB_ITEM_DESCRIPTOR;
    item.descriptor = out->keep(data, length);
    item.descriptor_length = length;
    return item;
}

mimusb_usb_item endpoint_item(const mimusb_usb_endpoint& endpoint) {
    mimusb_usb_item item{};
    item.kind = MIMUSB_ITEM_ENDPOINT;
    item.endpoint = endpoint;
    return item;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimusb_control_rule class_rule(const std::uint8_t type, const std::uint8_t request,
                               const std::uint8_t interface_number,
                               const mimusb_control_rule_action action) {
    mimusb_control_rule r{};
    r.request_type_mask = 0xFFU;
    r.request_type_value = type;
    r.request_mask = 0xFFU;
    r.request_value = request;
    r.index_mask = 0xFFFFU;
    r.index_value = interface_number;
    r.action = action;
    return r;
}

// What a CDC-ACM Spec is made of at a placement: the two interfaces with their class descriptors,
// the Interface Association descriptor and the three control rules, in `out`, and the slot of the
// line coding rule. Checks the placement's roles.
mimusb_result build_cdc_acm(const mimusb_spec& spec, const mimusb_node_placement& placement,
                            mimusb::detail::InterfacesSpec* out, mimusb::detail::RuleSlots* slots) {
    const CdcAcmSpec& cdc = data_of(spec);
    if (placement.first_interface == 0xFFU) {
        set_error(MIMUSB_ERR_PARAM, "placement.first_interface",
                  "A CDC-ACM Spec takes two interface numbers: first_interface and the next.");
        return MIMUSB_ERR_PARAM;
    }
    // The roles: all three are required, each once, with the transfer type and direction the
    // class has (CDC 1.2 3.4: the notification endpoint is interrupt IN, the data endpoints bulk).
    struct Role {
        mimusb_endpoint_role role;
        std::uint8_t transfer_type;
        bool in;
        mimusb_usb_endpoint endpoint;
        bool seen;
    };
    std::array<Role, 3> roles{{
        {MIMUSB_ROLE_CDC_NOTIFY, MIMUSB_TRANSFER_INTERRUPT, true, {}, false},
        {MIMUSB_ROLE_CDC_DATA_IN, MIMUSB_TRANSFER_BULK, true, {}, false},
        {MIMUSB_ROLE_CDC_DATA_OUT, MIMUSB_TRANSFER_BULK, false, {}, false},
    }};
    for (std::size_t i = 0U; i < placement.endpoint_count; ++i) {
        const mimusb_placed_endpoint& placed = placement.endpoints[i];
        const auto index = static_cast<std::uint32_t>(i);
        if (placed.reserved != 0U) {
            set_error(MIMUSB_ERR_PARAM, "placement.endpoints.reserved",
                      "The reserved field must be zero.", 0, 0, 0, 0, index, 0);
            return MIMUSB_ERR_PARAM;
        }
        Role* role = nullptr;
        for (Role& candidate : roles) {
            if (candidate.role == placed.role)
                role = &candidate;
        }
        if (role == nullptr || role->seen) {
            set_error(MIMUSB_ERR_PARAM, "placement.endpoints.role",
                      role == nullptr ? "The Spec has no such role (mimusb_spec_roles)."
                                      : "A role is given once.",
                      0, 0, 0, 0, index, static_cast<std::uint32_t>(placed.role));
            return MIMUSB_ERR_PARAM;
        }
        if (placed.endpoint.transfer_type != role->transfer_type) {
            set_error(MIMUSB_ERR_UNSUPPORTED, "placement.endpoints.transfer_type",
                      "The endpoint of a CDC-ACM role is interrupt (NOTIFY) or bulk (DATA).", 0, 0,
                      0, 0, index, placed.endpoint.transfer_type);
            return MIMUSB_ERR_UNSUPPORTED;
        }
        if (((placed.endpoint.address & 0x80U) != 0U) != role->in) {
            set_error(MIMUSB_ERR_PARAM, "placement.endpoints.address",
                      "The address of an IN role has bit 7 set and that of an OUT role has not.", 0,
                      0, 0, 0, index, placed.endpoint.address);
            return MIMUSB_ERR_PARAM;
        }
        role->endpoint = placed.endpoint;
        role->seen = true;
    }
    for (const Role& role : roles) {
        if (!role.seen) {
            set_error(MIMUSB_ERR_UNSET_FIELD, "placement.endpoints",
                      "MIMUSB_ROLE_CDC_NOTIFY, _CDC_DATA_IN and _CDC_DATA_OUT are all required.", 0,
                      0, 0, 0, 0, static_cast<std::uint32_t>(role.role));
            return MIMUSB_ERR_UNSET_FIELD;
        }
    }

    const mimusb_cdc_acm_options& o = cdc.options;
    const std::uint8_t control = placement.first_interface;
    const auto data = static_cast<std::uint8_t>(control + 1U);
    const auto bcd_low = static_cast<std::uint8_t>(o.bcd_cdc & 0xFFU);
    const auto bcd_high = static_cast<std::uint8_t>(o.bcd_cdc >> 8U);
    // CDC 1.2 5.2.3: bLength, bDescriptorType, bDescriptorSubtype, then the data of the subtype.
    const std::uint8_t header[5] = {5U, kCsInterface, 0x00U, bcd_low, bcd_high};
    const std::uint8_t call[5] = {5U, kCsInterface, 0x01U, o.call_management_capabilities, data};
    const std::uint8_t acm[4] = {4U, kCsInterface, 0x02U, o.acm_capabilities};
    const std::uint8_t onion[5] = {5U, kCsInterface, 0x06U, control, data};
    // USB IAD ECN: first interface, two interfaces, the function's class, subclass and protocol,
    // no string.
    const std::uint8_t association[8] = {
        8U, kInterfaceAssociation, control, 2U, kClassCdcControl, kSubclassAcm, o.protocol, 0U};

    std::vector<mimusb_usb_item> comm_items;
    comm_items.push_back(descriptor_item(out, header, sizeof header));
    comm_items.push_back(descriptor_item(out, call, sizeof call));
    comm_items.push_back(descriptor_item(out, acm, sizeof acm));
    comm_items.push_back(descriptor_item(out, onion, sizeof onion));
    comm_items.push_back(endpoint_item(roles[0].endpoint));
    std::vector<mimusb_usb_item> data_items;
    data_items.push_back(endpoint_item(roles[1].endpoint));
    data_items.push_back(endpoint_item(roles[2].endpoint));
    out->items.push_back(std::move(comm_items));
    out->items.push_back(std::move(data_items));

    std::vector<mimusb_usb_setting> comm_setting{{out->items[0].data(), out->items[0].size()}};
    std::vector<mimusb_usb_setting> data_setting{{out->items[1].data(), out->items[1].size()}};
    out->settings.push_back(std::move(comm_setting));
    out->settings.push_back(std::move(data_setting));

    mimusb_usb_interface comm{};
    comm.number = control;
    comm.class_code = kClassCdcControl;
    comm.subclass = kSubclassAcm;
    comm.protocol = o.protocol;
    if (placement.name != nullptr) {
        out->names.emplace_back(placement.name);
        comm.name = out->names.back().c_str();
    }
    comm.settings = out->settings[0].data();
    comm.setting_count = 1U;
    mimusb_usb_interface data_interface{};
    data_interface.number = data;
    data_interface.class_code = kClassCdcData;
    if (!cdc.data_name.empty()) {
        out->names.emplace_back(cdc.data_name);
        data_interface.name = out->names.back().c_str();
    }
    data_interface.settings = out->settings[1].data();
    data_interface.setting_count = 1U;
    out->interfaces.push_back(comm);
    out->interfaces.push_back(data_interface);

    out->association.assign(association, association + sizeof association);

    // The rules (PSTN 1.2 6.3), in the order of the slots below.
    out->rules.push_back(class_rule(0x21U, kSetLineCoding, control, MIMUSB_CONTROL_ACCEPT));
    mimusb_control_rule get = class_rule(0xA1U, kGetLineCoding, control, MIMUSB_CONTROL_ANSWER);
    get.data = out->keep(o.line_coding, sizeof o.line_coding);
    get.length = sizeof o.line_coding;
    out->rules.push_back(get);
    out->rules.push_back(class_rule(0x21U, kSetControlLineState, control, MIMUSB_CONTROL_ACCEPT));
    slots->emplace_back(MIMUSB_RULE_SLOT_CDC_LINE_CODING, 1U);

    out->finish();
    return MIMUSB_OK;
}

class CdcAcmClass final : public mimusb::detail::FunctionClass {
  public:
    std::size_t interface_count(const mimusb_spec&) const override { return 2U; }
    std::size_t roles(const mimusb_spec&, mimusb_role_info* out,
                      const std::size_t capacity) const override {
        const mimusb_role_info all[3] = {{MIMUSB_ROLE_CDC_NOTIFY, 1U},
                                         {MIMUSB_ROLE_CDC_DATA_IN, 1U},
                                         {MIMUSB_ROLE_CDC_DATA_OUT, 1U}};
        for (std::size_t i = 0U; i < 3U && i < capacity; ++i)
            out[i] = all[i];
        return 3U;
    }
    void rule_cost(const mimusb_spec& spec, std::size_t* rules,
                   std::size_t* answer_bytes) const override {
        // Three rules, 7 bytes of answer: made from a placement of any numbers.
        *rules = 3U;
        *answer_bytes = sizeof(data_of(spec).options.line_coding);
    }
    mimusb_result build(mimusb_device*, mimusb_spec* spec, const mimusb_node_placement& placement,
                        mimusb::detail::NodeBuild* out) const override {
        if (const mimusb_result r = build_cdc_acm(*spec, placement, &out->storage, &out->slots);
            r != MIMUSB_OK)
            return r;
        out->config = &out->storage.view;
        return MIMUSB_OK;
    }
};

const CdcAcmClass kCdcAcm{};

} // namespace

namespace mimusb::detail {

const FunctionClass& cdc_acm_class() noexcept { return kCdcAcm; }

} // namespace mimusb::detail

extern "C" {

mimusb_result MIMUSB_CALL mimusb_spec_create_cdc_acm(const mimusb_cdc_acm_options* options,
                                                     mimusb_spec** out_spec) try {
    mimusb::detail::clear_error();
    if (out_spec == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_spec", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_spec = nullptr;
    if (!mimusb::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                      static_cast<std::uint32_t>(sizeof(mimusb_cdc_acm_options)),
                                      "options")) {
        return MIMUSB_ERR_PARAM;
    }
    if (options->reserved != 0U || options->reserved8[0] != 0U || options->reserved8[1] != 0U ||
        options->reserved8[2] != 0U || options->reserved8b != 0U) {
        set_error(MIMUSB_ERR_PARAM, "options.reserved", "The reserved fields must be zero.");
        return MIMUSB_ERR_PARAM;
    }
    if (options->bcd_cdc == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "options.bcd_cdc",
                  "bcd_cdc must be given (e.g. 0x0120).");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    // dwDTERate, little-endian: 0 is the rate nobody has, so it is the unset value.
    if (options->line_coding[0] == 0U && options->line_coding[1] == 0U &&
        options->line_coding[2] == 0U && options->line_coding[3] == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "options.line_coding",
                  "line_coding must be given: its dwDTERate is not 0.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (!mimusb::detail::valid_usb_string(options->data_name)) {
        set_error(MIMUSB_ERR_PARAM, "options.data_name",
                  "The name must be UTF-8 without control characters, at most 126 UTF-16 code "
                  "units.");
        return MIMUSB_ERR_PARAM;
    }
    auto cdc = std::make_unique<CdcAcmSpec>();
    cdc->options = *options;
    cdc->options.data_name = nullptr;
    if (options->data_name != nullptr)
        cdc->data_name = options->data_name;
    auto spec = std::make_unique<mimusb_spec>();
    spec->kind = MIMUSB_PROFILE_CDC_ACM;
    spec->function_class = &kCdcAcm;
    spec->data = std::move(cdc);
    *out_spec = spec.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("spec.create_cdc_acm")

} // extern "C"
