// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Descriptor layouts: USB 2.0 9.6.1 (Device), 9.6.3 (Configuration), 9.6.5 (Interface), 9.6.6
 * (Endpoint), 9.6.7 (String); HID 1.11 6.2.1 (HID descriptor).
 */

#include "usb/descriptor_composer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace mimhid::usb {

namespace {

constexpr std::uint8_t kTypeDevice = 1;
constexpr std::uint8_t kTypeConfiguration = 2;
constexpr std::uint8_t kTypeString = 3;
constexpr std::uint8_t kTypeInterface = 4;
constexpr std::uint8_t kTypeEndpoint = 5;
constexpr std::uint8_t kTypeHid = 0x21;
constexpr std::uint8_t kTypeHidReport = 0x22;
constexpr std::uint8_t kTransferInterrupt = 3; // USB 2.0 9.6.6, bmAttributes bits 1..0

void put_u16(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

bool fail(ComposerError* error, mimhid_result result, const char* field, const char* reason,
          std::uint32_t offset = 0, std::uint32_t length = 0) {
    if (error != nullptr)
        *error = ComposerError{result, field, reason, offset, length};
    return false;
}

std::vector<std::uint8_t> string_descriptor(const std::u16string& units) {
    std::vector<std::uint8_t> d;
    d.push_back(static_cast<std::uint8_t>(2U + 2U * units.size()));
    d.push_back(kTypeString);
    for (const char16_t unit : units)
        put_u16(&d, unit);
    return d;
}

bool same_endpoint(const mimhid_usb_endpoint& a, const mimhid_usb_endpoint& b) {
    return a.address == b.address && a.poll_interval_ms == b.poll_interval_ms &&
           a.max_packet_size == b.max_packet_size;
}

bool same_settings(const ComposerNode& a, const ComposerNode& b) {
    const mimhid_usb_interface& x = a.iface;
    const mimhid_usb_interface& y = b.iface;
    return x.class_code == y.class_code && x.subclass == y.subclass && x.protocol == y.protocol &&
           x.bcd_hid == y.bcd_hid && x.country_code == y.country_code &&
           a.interface_name == b.interface_name && same_endpoint(a.ep_in, b.ep_in) &&
           same_endpoint(a.ep_out, b.ep_out);
}

void endpoint_descriptor(std::vector<std::uint8_t>* out, const mimhid_usb_endpoint& ep) {
    out->push_back(7);
    out->push_back(kTypeEndpoint);
    out->push_back(ep.address);
    out->push_back(kTransferInterrupt);
    put_u16(out, ep.max_packet_size);
    out->push_back(ep.poll_interval_ms);
}

} // namespace

mimhid_result compose_descriptors(const ComposerInput& input, Composed* out, ComposerError* error) {
    const auto refuse = [&](mimhid_result result, const char* field, const char* reason,
                            std::uint32_t offset = 0, std::uint32_t length = 0) {
        fail(error, result, field, reason, offset, length);
        return result;
    };

    // The interfaces: the Nodes by interface number, each in node order.
    std::map<std::uint32_t, std::vector<const ComposerNode*>> by_number;
    for (const ComposerNode& node : input.nodes)
        by_number[node.iface.number].push_back(&node);

    if (by_number.size() > input.max_interfaces)
        return refuse(MIMHID_ERR_OVERFLOW, "node_config.iface.number",
                      "Number of distinct interfaces exceeds the board's maximum interfaces.",
                      static_cast<std::uint32_t>(input.max_interfaces),
                      static_cast<std::uint32_t>(by_number.size()));

    // USB 2.0 9.6.5: bInterfaceNumber is the index of the interface in the configuration, so the
    // numbers are 0, 1, 2, ... in order. A gap is the caller's to close; nothing is renumbered.
    std::uint32_t expected = 0;
    for (const auto& entry : by_number) {
        if (entry.first != expected)
            return refuse(MIMHID_ERR_PARAM, "node_config.iface.number",
                          "Interface numbers must be 0, 1, 2, ... without a gap; the missing "
                          "number is in offset.",
                          expected, static_cast<std::uint32_t>(by_number.size()));
        ++expected;
    }

    // HID 1.11 6.2.1 / 5.6: Nodes sharing an interface join their report descriptors, so every
    // Report ID must be unique on it and every Node must have some.
    for (const auto& entry : by_number) {
        const auto& group = entry.second;
        if (group.size() < 2U)
            continue;
        std::bitset<256> combined;
        for (const ComposerNode* node : group) {
            if (!node->has_report_ids || node->report_ids.none())
                return refuse(MIMHID_ERR_PARAM, "node.report_id",
                              "Multiple nodes sharing an interface must all declare Report IDs.",
                              entry.first);
            if ((combined & node->report_ids).any())
                return refuse(MIMHID_ERR_PARAM, "node.report_id",
                              "Duplicate Report ID assigned on the same interface.", entry.first);
            combined |= node->report_ids;
        }
        // The interface is described from its first Node, so the others must agree with it.
        for (const ComposerNode* node : group) {
            if (!same_settings(*group.front(), *node))
                return refuse(MIMHID_ERR_PARAM, "node_config.iface",
                              "Nodes sharing an interface must have the same interface and "
                              "endpoint settings.",
                              entry.first);
        }
    }

    // An endpoint address belongs to one interface (USB 2.0 9.6.6).
    std::set<std::uint8_t> endpoints;
    for (const auto& entry : by_number) {
        const ComposerNode& first = *entry.second.front();
        if (!endpoints.insert(first.ep_in.address).second ||
            (first.ep_out.address != 0U && !endpoints.insert(first.ep_out.address).second))
            return refuse(MIMHID_ERR_PARAM, "node_config.ep_in.address",
                          "Two interfaces use the same endpoint address.", 0, entry.first);
    }

    Composed composed;

    // Strings 1..4 and one per named interface; the language of all of them is the Device's
    // (USB 2.0 9.6.7). The serial number is the Device's own text, else the board's when asked.
    const mimhid_usb_device& dev = input.device;
    const mimhid_usb_config& cfg = input.config;
    const auto add_string = [&](std::uint8_t index, const std::u16string& text) {
        composed.strings.push_back(ComposedString{
            index, static_cast<std::uint16_t>(dev.language_id), false, string_descriptor(text)});
    };
    if (!input.manufacturer.empty())
        add_string(kStringManufacturer, input.manufacturer);
    if (!input.product.empty())
        add_string(kStringProduct, input.product);
    std::uint8_t serial_index = 0;
    if (!input.serial.empty()) {
        add_string(kStringSerial, input.serial);
        serial_index = kStringSerial;
    } else if (dev.serial_from_chip_id == 1U && input.board_has_unique_id) {
        composed.strings.push_back(
            ComposedString{kStringSerial, static_cast<std::uint16_t>(dev.language_id), true, {}});
        serial_index = kStringSerial;
    }
    if (!input.configuration.empty())
        add_string(kStringConfiguration, input.configuration);

    // The Configuration descriptor with, per interface in number order, the Interface, HID and
    // Endpoint descriptors (HID 1.11 7.1: the HID descriptor follows the Interface descriptor).
    std::vector<std::uint8_t>& c = composed.configuration;
    c.assign(9, 0);
    for (const auto& entry : by_number) {
        const ComposerNode& first = *entry.second.front();
        ComposedInterface itf;
        itf.number = static_cast<std::uint8_t>(entry.first);
        for (const ComposerNode* node : entry.second)
            itf.report_descriptor.insert(itf.report_descriptor.end(),
                                         node->report_descriptor->begin(),
                                         node->report_descriptor->end());
        const bool has_out = first.ep_out.max_packet_size > 0U;
        const std::uint8_t name_index =
            first.interface_name.empty()
                ? 0
                : static_cast<std::uint8_t>(kStringInterfaceBase + itf.number);
        if (name_index != 0)
            add_string(name_index, first.interface_name);

        c.push_back(9);
        c.push_back(kTypeInterface);
        c.push_back(itf.number);
        c.push_back(0); // bAlternateSetting
        c.push_back(has_out ? 2U : 1U);
        c.push_back(first.iface.class_code);
        c.push_back(first.iface.subclass);
        c.push_back(first.iface.protocol);
        c.push_back(name_index);

        c.push_back(9);
        c.push_back(kTypeHid);
        put_u16(&c, first.iface.bcd_hid);
        c.push_back(first.iface.country_code);
        c.push_back(1); // bNumDescriptors
        c.push_back(kTypeHidReport);
        put_u16(&c, static_cast<std::uint32_t>(itf.report_descriptor.size()));

        endpoint_descriptor(&c, first.ep_in);
        if (has_out)
            endpoint_descriptor(&c, first.ep_out);
        composed.interfaces.push_back(std::move(itf));
    }
    const std::size_t total = c.size();
    c[0] = 9;
    c[1] = kTypeConfiguration;
    c[2] = static_cast<std::uint8_t>(total & 0xFFU);
    c[3] = static_cast<std::uint8_t>((total >> 8U) & 0xFFU);
    c[4] = static_cast<std::uint8_t>(composed.interfaces.size());
    c[5] = cfg.configuration_value;
    c[6] = input.configuration.empty() ? 0 : kStringConfiguration;
    c[7] = cfg.bm_attributes;
    c[8] = cfg.bmax_power;

    // The Device descriptor.
    std::vector<std::uint8_t>& d = composed.device;
    d.push_back(18);
    d.push_back(kTypeDevice);
    put_u16(&d, dev.bcd_usb);
    d.push_back(dev.device_class);
    d.push_back(dev.device_subclass);
    d.push_back(dev.device_protocol);
    d.push_back(dev.ep0_max_packet_size);
    put_u16(&d, dev.vendor_id);
    put_u16(&d, dev.product_id);
    put_u16(&d, dev.bcd_device);
    d.push_back(input.manufacturer.empty() ? 0 : kStringManufacturer);
    d.push_back(input.product.empty() ? 0 : kStringProduct);
    d.push_back(serial_index);
    d.push_back(1); // bNumConfigurations

    std::sort(composed.strings.begin(), composed.strings.end(),
              [](const ComposedString& a, const ComposedString& b) { return a.index < b.index; });
    *out = std::move(composed);
    return MIMHID_OK;
}

} // namespace mimhid::usb
