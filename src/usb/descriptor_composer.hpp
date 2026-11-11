// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * Composes every USB descriptor the board presents to the target, as bytes, from the Device
 * options and the open Nodes. Pure: no I/O, no board, no clock. The board holds what this makes
 * and checks that it is a configuration it can run (docs/PROTOCOL.md); it builds nothing itself.
 */

#include "mimhid.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mimhid::usb {

// What one Node contributes: its interface and endpoint settings, its report descriptor and the
// Report IDs that descriptor declares.
struct ComposerNode {
    mimhid_usb_interface iface{}; // iface.name is not read; interface_name is
    mimhid_usb_endpoint ep_in{};
    mimhid_usb_endpoint ep_out{};
    std::u16string interface_name;
    const std::vector<std::uint8_t>* report_descriptor{};
    bool has_report_ids{};
    std::bitset<256> report_ids;
};

struct ComposerInput {
    mimhid_usb_device device{};
    mimhid_usb_config config{};
    std::u16string manufacturer;
    std::u16string product;
    std::u16string serial;
    std::u16string configuration;
    // Nodes in node order: the order of their report descriptors inside a shared interface.
    std::vector<ComposerNode> nodes;
    std::size_t max_interfaces{}; // the board's USB HID interfaces
    bool board_has_unique_id{}; // the board can make the serial number string (serial_from_chip_id)
};

// A string descriptor as it is on the bus (USB 2.0 9.6.7).
struct ComposedString {
    std::uint8_t index{};
    std::uint16_t language{};
    bool from_chip_id{}; // the board makes the string; descriptor is empty
    std::vector<std::uint8_t> descriptor;
};

struct ComposedInterface {
    std::uint8_t number{};
    std::vector<std::uint8_t> report_descriptor; // the Nodes' descriptors, joined in node order
};

struct Composed {
    std::vector<std::uint8_t> device;          // 18 bytes (USB 2.0 9.6.1)
    std::vector<std::uint8_t> configuration;   // the whole configuration descriptor (9.6.3)
    std::vector<ComposedString> strings;       // by index, then language; index 0 is the board's
    std::vector<ComposedInterface> interfaces; // in interface number order
};

// What went wrong when a configuration cannot be composed. `offset` carries the number that
// matters (the missing interface number, the doubled endpoint address, ...).
struct ComposerError {
    mimhid_result result{MIMHID_OK};
    const char* field{};
    const char* reason{};
    std::uint32_t offset{};
    std::uint32_t length{};
};

// String indices: the Device's strings have fixed places; an interface name is 5 + its number.
constexpr std::uint8_t kStringManufacturer = 1;
constexpr std::uint8_t kStringProduct = 2;
constexpr std::uint8_t kStringSerial = 3;
constexpr std::uint8_t kStringConfiguration = 4;
constexpr std::uint8_t kStringInterfaceBase = 5;

// MIMHID_OK and `out` filled, or the first rule that is broken, in `error`.
mimhid_result compose_descriptors(const ComposerInput& input, Composed* out, ComposerError* error);

} // namespace mimhid::usb
