// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * Composes every USB descriptor the board presents to the target, as bytes, from the Device
 * options and the interfaces of the open Nodes. Pure: no I/O, no board, no clock. It reads no
 * class: an interface is its numbers, its class descriptors as given and its endpoints. The board
 * holds what this makes and checks that it is a configuration it can run (docs/PROTOCOL.md); it
 * builds nothing itself. What only HID needs (Nodes sharing an interface, Report IDs, the HID
 * descriptor and the rules that serve it) is made before this, in hid_layer.hpp.
 */

#include "mimusb.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mimusb::usb {

// One item of a setting, in bus order: an endpoint (its descriptor is made from `endpoint`) or a
// descriptor given as bytes (one whole descriptor, placed as given).
struct ComposerItem {
    bool is_endpoint{false};
    mimusb_usb_endpoint endpoint{};
    std::vector<std::uint8_t> descriptor;
};

// One alternate setting of an interface.
struct ComposerSetting {
    std::vector<ComposerItem> items;
};

// One interface of a Node: its settings are bAlternateSetting 0, 1, 2, ... in order.
struct ComposerInterface {
    std::uint8_t number{};
    std::uint8_t class_code{};
    std::uint8_t subclass{};
    std::uint8_t protocol{};
    std::u16string name; // empty: the interface has no string
    std::vector<ComposerSetting> settings;
};

// The interfaces of one Node and the association that holds them together, if it has one.
struct ComposerGroup {
    std::vector<ComposerInterface> interfaces;
    std::vector<std::uint8_t> association; // empty for none
    std::uint16_t node_id{};               // for the details of an error; 0 for none
};

// What the board takes (its PONG).
struct ComposerLimits {
    std::size_t max_interfaces{}; // of any class
    std::size_t max_endpoint_number{};
    std::size_t max_endpoint_bytes{};
    std::size_t max_config_bytes{};
    std::size_t max_bos_bytes{};
    std::size_t max_strings{};
    std::size_t endpoint_queue_slots{};
    std::size_t max_transfer_bytes{}; // one frame to an IN endpoint; bounds an isochronous packet
    std::uint32_t transfer_types{};   // bit n: transfer type n
    bool high_speed{};
    bool has_unique_id{}; // the board can make the serial number string
};

struct ComposerInput {
    mimusb_usb_device device{};
    mimusb_usb_config config{};
    std::u16string manufacturer;
    std::u16string product;
    std::u16string serial;
    std::u16string configuration;
    // Strings the caller added (mimusb_device_add_string), by index (5 or more).
    std::map<std::uint8_t, std::u16string> user_strings;
    std::vector<ComposerGroup> groups; // in node order
    ComposerLimits limits;
    // True when `groups` are the interfaces of one Node about to join others: every check that
    // looks at that Node alone is made, and the interface numbers need not start at 0 or be free
    // of gaps.
    bool partial{false};
};

// A string descriptor as it is on the bus (USB 2.0 9.6.7).
struct ComposedString {
    std::uint8_t index{};
    std::uint16_t language{};
    bool from_chip_id{}; // the board makes the string; descriptor is empty
    std::vector<std::uint8_t> descriptor;
};

struct Composed {
    std::vector<std::uint8_t> device;        // 18 bytes (USB 2.0 9.6.1)
    std::vector<std::uint8_t> configuration; // the whole configuration descriptor (9.6.3)
    std::vector<ComposedString> strings;     // by index, then language; index 0 is the board's
    std::size_t interface_count{};           // interfaces in the configuration
};

// What went wrong when a configuration cannot be composed. `offset` carries the number that
// matters (the missing interface number, the doubled endpoint address, ...).
struct ComposerError {
    mimusb_result result{MIMUSB_OK};
    const char* field{};
    const char* reason{};
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint16_t node_id{};
};

// String indices: the Device's strings have fixed places; the others come from one pool, from 5.
constexpr std::uint8_t kStringManufacturer = 1;
constexpr std::uint8_t kStringProduct = 2;
constexpr std::uint8_t kStringSerial = 3;
constexpr std::uint8_t kStringConfiguration = 4;
constexpr std::uint8_t kStringFirstFree = 5;

// Slots of the board's endpoint queue pool one IN endpoint of the generic driver takes (docs/
// LIMITS.md): depth + 1 for DROP_NEWEST and WAIT, 2 * depth + 1 for DROP_OLDEST.
std::size_t queue_slots(std::uint8_t depth, std::uint8_t on_full) noexcept;

// The checks of one endpoint that do not depend on how it was given: its address, the transfer
// type and packet size the board takes, the interval, the audio form. Where it may appear (not an
// isochronous endpoint in setting 0), its policy and the uniqueness of its address are the
// caller's. MIMUSB_OK, or the rule broken, in `error`.
mimusb_result check_endpoint_shape(const mimusb_usb_endpoint& ep, const ComposerLimits& lim,
                                   std::uint16_t node_id, ComposerError* error);

// MIMUSB_OK and `out` filled, or the first rule that is broken, in `error`.
mimusb_result compose_descriptors(const ComposerInput& input, Composed* out, ComposerError* error);

} // namespace mimusb::usb
