// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The checks of a Device given as bytes and of a BOS descriptor: what is accepted, and for every
 * way a descriptor can fail to hold together, the result and the offset the refusal names. */
#include "mimusb.h"
#include "test.hpp"
#include "usb/descriptor_walker.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using mimusb::usb::ComposerError;
using mimusb::usb::ComposerLimits;
using mimusb::usb::WalkedDescriptors;
using Bytes = std::vector<std::uint8_t>;

ComposerLimits limits(bool high_speed = false) {
    ComposerLimits l;
    l.max_interfaces = 8;
    l.max_endpoint_number = 7;
    l.max_endpoint_bytes = 512;
    l.max_config_bytes = 256;
    l.max_bos_bytes = 64;
    l.max_strings = 8;
    l.endpoint_queue_slots = 32;
    l.transfer_types = 0x0CU;
    l.high_speed = high_speed;
    return l;
}

Bytes device_bytes(std::uint16_t bcd = 0x0210) {
    return {18,
            1,
            static_cast<std::uint8_t>(bcd),
            static_cast<std::uint8_t>(bcd >> 8),
            0,
            0,
            0,
            64,
            0x09,
            0x12,
            0x01,
            0x00,
            0x00,
            0x01,
            1,
            2,
            3,
            1};
}

// One interface (class 0xFF) with a bulk IN 0x81 and a bulk OUT 0x01: 9 + 9 + 7 + 7 bytes.
Bytes config_bytes() {
    return {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 0,    0, 2,  0xFF, 1,
            2, 0, 7,  5, 0x81, 2, 64, 0,    0,  7, 5, 0x01, 2, 64, 0,    0};
}

Bytes bos_bytes() { return {5, 0x0F, 12, 0, 1, 7, 0x10, 0x02, 0x02, 0, 0, 0}; }

mimusb_result walk(const Bytes& d, const Bytes& c, const Bytes* bos, ComposerError* e,
                   WalkedDescriptors* out = nullptr, bool high_speed = false) {
    WalkedDescriptors local;
    return mimusb::usb::walk_descriptors(
        d.data(), d.size(), c.data(), c.size(), bos == nullptr ? nullptr : bos->data(),
        bos == nullptr ? 0U : bos->size(), limits(high_speed), out != nullptr ? out : &local, e);
}

// The result and offset of the refusal of a configuration with one byte changed.
void config_refused(std::size_t at, std::uint8_t value, mimusb_result result,
                    std::uint32_t offset) {
    Bytes c = config_bytes();
    c[at] = value;
    ComposerError e;
    const mimusb_result got = walk(device_bytes(), c, nullptr, &e);
    if (got != result || e.offset != offset)
        std::fprintf(stderr, "config[%zu]=%u: result %d offset %u, wanted %d at %u\n", at, value,
                     static_cast<int>(got), e.offset, static_cast<int>(result), offset);
    MIMUSB_CHECK(got == result);
    MIMUSB_CHECK(e.result == result && e.offset == offset);
    MIMUSB_CHECK(std::strcmp(e.field, "descriptors.configuration") == 0);
}

void test_accepted() {
    ComposerError e;
    WalkedDescriptors w;
    MIMUSB_CHECK(walk(device_bytes(), config_bytes(), nullptr, &e, &w) == MIMUSB_OK);
    MIMUSB_CHECK(w.interface_count == 1U && w.endpoints.size() == 2U && w.bcd_usb == 0x0210U);
    MIMUSB_CHECK(w.endpoints[0].address == 0x81U && w.endpoints[0].offset == 18U &&
                 w.endpoints[1].address == 0x01U && w.endpoints[1].interface_number == 0U);
    // A BOS rides along with bcdUSB 0x0201 or more.
    const Bytes bos = bos_bytes();
    MIMUSB_CHECK(walk(device_bytes(0x0201), config_bytes(), &bos, &e) == MIMUSB_OK);
    // A class descriptor between the interface and its endpoints, as given.
    Bytes c = config_bytes();
    c.insert(c.begin() + 18, {9, 0x21, 0x11, 0x01, 0, 1, 0x22, 4, 0});
    c[2] = static_cast<std::uint8_t>(c.size());
    MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e, &w) == MIMUSB_OK &&
                 w.endpoints[0].offset == 27U);
    // An association over two interfaces.
    Bytes two = {9, 2, 0, 0, 2,    1, 0, 0x80, 50, 8, 0x0B, 0, 2, 0xFF, 1, 2, 0, 9,
                 4, 0, 0, 0, 0xFF, 1, 2, 0,    9,  4, 1,    0, 0, 0xFF, 1, 2, 0};
    two[2] = static_cast<std::uint8_t>(two.size());
    MIMUSB_CHECK(walk(device_bytes(), two, nullptr, &e, &w) == MIMUSB_OK &&
                 w.interface_count == 2U);
}

void test_device_refused() {
    ComposerError e;
    Bytes d = device_bytes();
    const Bytes c = config_bytes();
    d.pop_back();
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 0U && e.length == 17U &&
                 std::strcmp(e.field, "descriptors.device") == 0);
    d = device_bytes();
    d[0] = 17;
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 0U);
    d = device_bytes();
    d[1] = 2;
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 1U);
    d = device_bytes(0);
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_UNSET_FIELD && e.offset == 2U);
    d = device_bytes();
    d[7] = 63;
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 7U);
    d[7] = 8; // full speed takes 8
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_OK);
    MIMUSB_CHECK(walk(d, c, nullptr, &e, nullptr, true) == MIMUSB_ERR_PARAM && e.offset == 7U);
    d = device_bytes();
    d[17] = 2;
    MIMUSB_CHECK(walk(d, c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 17U);
    WalkedDescriptors w;
    MIMUSB_CHECK(mimusb::usb::walk_descriptors(nullptr, 0, c.data(), c.size(), nullptr, 0, limits(),
                                               &w, &e) == MIMUSB_ERR_PARAM &&
                 e.offset == 0U);
}

void test_configuration_refused() {
    ComposerError e;
    // The header.
    config_refused(0, 8, MIMUSB_ERR_PARAM, 0);
    config_refused(1, 3, MIMUSB_ERR_PARAM, 1);
    config_refused(2, 33, MIMUSB_ERR_PARAM, 2); // wTotalLength
    config_refused(2, 31, MIMUSB_ERR_PARAM, 2);
    config_refused(4, 2, MIMUSB_ERR_PARAM, 4);    // bNumInterfaces
    config_refused(5, 2, MIMUSB_ERR_PARAM, 5);    // bConfigurationValue
    config_refused(7, 0x00, MIMUSB_ERR_PARAM, 7); // D7 clear
    config_refused(7, 0x81, MIMUSB_ERR_PARAM, 7); // reserved bits
    // The interface.
    config_refused(9, 8, MIMUSB_ERR_PARAM, 9);   // bLength
    config_refused(11, 1, MIMUSB_ERR_PARAM, 11); // number must be 0
    config_refused(12, 1, MIMUSB_ERR_PARAM, 11); // an alternate setting with no setting 0 before it
    config_refused(13, 1, MIMUSB_ERR_PARAM,
                   25); // one declared: the second endpoint is one too many
    config_refused(13, 3, MIMUSB_ERR_PARAM, 13); // three declared, two follow (at the interface)
    // The endpoints.
    config_refused(18, 6, MIMUSB_ERR_PARAM, 18);       // bLength of an endpoint
    config_refused(20, 0x91, MIMUSB_ERR_PARAM, 20);    // bits 6..4
    config_refused(20, 0x80, MIMUSB_ERR_PARAM, 20);    // number 0
    config_refused(20, 0x88, MIMUSB_ERR_OVERFLOW, 20); // above the controller
    config_refused(20, 0x01, MIMUSB_ERR_PARAM, 27);    // the second endpoint reuses the address
    config_refused(21, 1, MIMUSB_ERR_UNSUPPORTED, 21); // isochronous
    config_refused(21, 0, MIMUSB_ERR_UNSUPPORTED, 21); // control
    config_refused(22, 63, MIMUSB_ERR_PARAM, 20);      // bulk packet size
    config_refused(22, 0, MIMUSB_ERR_UNSET_FIELD, 20); // zero packet size
    // A descriptor that does not fit, and one with nothing to hold it.
    {
        Bytes c = config_bytes();
        c[25] = 9; // the last endpoint claims 9 bytes: past the end
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 25U);
        c = config_bytes();
        c[25] = 1; // bLength below 2
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 25U);
        c = config_bytes();
        c.erase(c.begin() + 9, c.begin() + 18); // endpoints with no interface
        c[2] = static_cast<std::uint8_t>(c.size());
        c[4] = 0;
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM);
        c = {9, 2, 9, 0, 0, 1, 0, 0x80, 50}; // no interface at all
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 9U);
        c = config_bytes();
        c.resize(8); // not even the header
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 0U);
    }
    // Interface numbers 0, 1, 2 ...; a second interface that jumps to 2.
    {
        Bytes c = config_bytes();
        const Bytes second = {9, 4, 2, 0, 0, 0xFF, 1, 2, 0};
        c.insert(c.end(), second.begin(), second.end());
        c[2] = static_cast<std::uint8_t>(c.size());
        c[4] = 2;
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 34U &&
                     e.length == 1U);
    }
    // An interval of an interrupt endpoint, and the totals of the board.
    {
        Bytes c = config_bytes();
        c[21] = 3;
        c[22] = 16;
        c[24] = 0; // bInterval missing
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_UNSET_FIELD);
        c[24] = 4;
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_OK);
        ComposerLimits l = limits();
        l.max_interfaces = 0;
        Bytes d = device_bytes();
        Bytes ok = config_bytes();
        WalkedDescriptors w;
        MIMUSB_CHECK(mimusb::usb::walk_descriptors(d.data(), d.size(), ok.data(), ok.size(),
                                                   nullptr, 0, l, &w, &e) == MIMUSB_ERR_OVERFLOW);
        l = limits();
        l.max_config_bytes = 31;
        MIMUSB_CHECK(mimusb::usb::walk_descriptors(d.data(), d.size(), ok.data(), ok.size(),
                                                   nullptr, 0, l, &w, &e) == MIMUSB_ERR_OVERFLOW &&
                     e.offset == 31U && e.length == 32U);
    }
    // An association that names interfaces that are not there, or not the next one.
    {
        Bytes c = {9,    2, 0, 0, 1, 1, 0, 0x80, 50, 8,    0x0B, 0, 2,
                   0xFF, 1, 2, 0, 9, 4, 0, 0,    0,  0xFF, 1,    2, 0};
        c[2] = static_cast<std::uint8_t>(c.size());
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM &&
                     e.offset == c.size());
        c[11] = 1; // bFirstInterface is not the next interface
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 11U);
        c[11] = 0;
        c[12] = 0; // names none
        MIMUSB_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMUSB_ERR_PARAM && e.offset == 9U);
    }
}

// An interface with alternate settings: setting 0 with no endpoint (no bandwidth), setting 1 with
// an isochronous IN endpoint of the audio-class 9-byte form, setting 2 with a larger packet.
Bytes streaming_config() {
    return {9, 2, 0,    0,    1,    1,    0, 0x80, 50, // configuration
            9, 4, 0,    0,    0,    0x01, 2, 0,    0,  // interface 0, setting 0
            9, 4, 0,    1,    1,    0x01, 2, 0,    0,  // setting 1
            9, 5, 0x81, 0x05, 0xC0, 0,    1, 0,    0,  // iso IN, 192 bytes, bInterval 1
            9, 4, 0,    2,    1,    0x01, 2, 0,    0,  // setting 2
            9, 5, 0x81, 0x05, 0x80, 1,    1, 0,    0}; // the same address, 384 bytes
}

ComposerLimits iso_limits(bool high_speed = false) {
    ComposerLimits l = limits(high_speed);
    l.transfer_types = 0x0EU; // isochronous, bulk, interrupt
    l.max_transfer_bytes = 1023;
    return l;
}

mimusb_result walk_iso(const Bytes& c, ComposerError* e, WalkedDescriptors* out = nullptr,
                       bool high_speed = false) {
    Bytes cfg = c;
    cfg[2] = static_cast<std::uint8_t>(cfg.size());
    const Bytes d = device_bytes();
    WalkedDescriptors local;
    return mimusb::usb::walk_descriptors(d.data(), d.size(), cfg.data(), cfg.size(), nullptr, 0,
                                         iso_limits(high_speed), out != nullptr ? out : &local, e);
}

void test_alternate_settings_and_isochronous() {
    ComposerError e;
    WalkedDescriptors w;
    // Accepted: one interface, three settings, the endpoint named by two of them.
    MIMUSB_CHECK(walk_iso(streaming_config(), &e, &w) == MIMUSB_OK);
    MIMUSB_CHECK(w.interface_count == 1U && w.endpoints.size() == 2U);
    MIMUSB_CHECK(w.endpoints[0].address == 0x81U && w.endpoints[0].alternate_setting == 1U &&
                 w.endpoints[0].transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS &&
                 w.endpoints[0].max_packet_size == 192U && w.endpoints[0].interface_number == 0U);
    MIMUSB_CHECK(w.endpoints[1].alternate_setting == 2U && w.endpoints[1].max_packet_size == 384U);

    // A board without isochronous endpoints says so, at the attributes byte.
    {
        Bytes c = streaming_config();
        c[2] = static_cast<std::uint8_t>(c.size());
        const Bytes d = device_bytes();
        MIMUSB_CHECK(mimusb::usb::walk_descriptors(d.data(), d.size(), c.data(), c.size(), nullptr,
                                                   0, limits(), &w, &e) == MIMUSB_ERR_UNSUPPORTED &&
                     e.offset == 30U);
    }
    // The settings come in order, with nothing between them, after setting 0.
    {
        Bytes c = streaming_config();
        c[39] = 3; // setting 2 named 3
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 39U && e.length == 2U);
        c = streaming_config();
        c[21] = 2; // setting 1 named 2
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 21U && e.length == 1U);
        c = streaming_config();
        c[20] = 1; // an alternate setting of an interface that is not the last one
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 20U);
    }
    // An isochronous endpoint: the bits it may carry, its size and its interval.
    {
        Bytes c = streaming_config();
        c[30] = 0x45; // bits 7..6 are reserved
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 30U);
        c = streaming_config();
        c[31] = 0xFF;
        c[32] = 0x03; // 1023 is the most at full speed
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_OK);
        c[32] = 0x04; // 1024 is not
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_OVERFLOW && e.offset == 31U);
        MIMUSB_CHECK(walk_iso(c, &e, &w, true) == MIMUSB_ERR_OVERFLOW); // the board's frame, 1023
        c = streaming_config();
        c[31] = 0;
        c[32] = 0; // zero
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_OVERFLOW && e.offset == 31U);
        c = streaming_config();
        c[33] = 2; // bInterval is 1 at full speed
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 33U);
        MIMUSB_CHECK(walk_iso(c, &e, &w, true) == MIMUSB_OK); // 1 to 16 (an exponent) at high speed
        c[33] = 17;
        MIMUSB_CHECK(walk_iso(c, &e, &w, true) == MIMUSB_ERR_PARAM && e.offset == 33U);
    }
    // An address comes again only in the same interface, as the same type; a bulk or interrupt
    // endpoint with the same descriptor.
    {
        Bytes c = streaming_config();
        c[47] = 0x82; // the endpoint of setting 2 is another one: fine
        MIMUSB_CHECK(walk_iso(c, &e, &w) == MIMUSB_OK && w.endpoints.size() == 2U);
        c = streaming_config();
        c[48] = 0x02; // the address 0x81 comes again as a bulk endpoint: another type
        MIMUSB_CHECK(walk_iso(c, &e) != MIMUSB_OK);
    }
    {
        // Two bulk settings sharing an endpoint: the same descriptor is accepted, another size is
        // not.
        Bytes c = {9, 2,  0, 0, 1, 1, 0, 0x80, 50, 9,    4, 0, 0, 1, 0xFF, 1,    2, 0,  7, 5, 0x81,
                   2, 64, 0, 0, 9, 4, 0, 1,    1,  0xFF, 1, 2, 0, 7, 5,    0x81, 2, 64, 0, 0};
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_OK);
        c[36] = 32;
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM);
        // A setting twice in one setting is the same address twice.
        c = {9, 2, 0, 0, 1,    1, 0,  0x80, 50, 9, 4, 0,    0, 2,  0xFF, 1,
             2, 0, 7, 5, 0x81, 2, 64, 0,    0,  7, 5, 0x81, 2, 64, 0,    0};
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM && e.offset == 27U);
    }
    // Another interface may not use an address a setting of the first has.
    {
        Bytes c = streaming_config();
        const Bytes second = {9, 4, 1, 0, 1, 0xFF, 1, 2, 0, 7, 5, 0x81, 2, 64, 0, 0};
        c.insert(c.end(), second.begin(), second.end());
        c[4] = 2;
        MIMUSB_CHECK(walk_iso(c, &e) == MIMUSB_ERR_PARAM);
    }
    // A bulk endpoint may carry the 9-byte form too (a MIDI streaming endpoint is one); 8 is no
    // form.
    {
        Bytes c = {9,    2, 0, 0, 1, 1, 0,    0x80, 50, 9, 4, 0, 0, 1,
                   0x01, 3, 0, 0, 9, 5, 0x01, 2,    64, 0, 0, 0, 0};
        MIMUSB_CHECK(walk_iso(c, &e, &w) == MIMUSB_OK && w.endpoints.size() == 1U);
        c[18] = 8;
        c.resize(26);
        MIMUSB_CHECK(walk_iso(c, &e, &w) == MIMUSB_ERR_PARAM && e.offset == 18U);
    }
}

void test_bos() {
    ComposerError e;
    const Bytes c = config_bytes();
    const Bytes good = bos_bytes();
    // A BOS needs bcdUSB 0x0201 or more.
    MIMUSB_CHECK(walk(device_bytes(0x0200), c, &good, &e) == MIMUSB_ERR_PARAM && e.offset == 2U &&
                 std::strcmp(e.field, "descriptors.device") == 0);
    MIMUSB_CHECK(walk(device_bytes(0x0110), c, &good, &e) == MIMUSB_ERR_PARAM);
    // Each way the descriptor fails to hold together, with the offset it names.
    const struct {
        std::size_t at;
        std::uint8_t value;
    } mistakes[] = {{0, 4}, {1, 0x10}, {2, 11}, {2, 13}, {4, 0}, {4, 2}, {5, 2}, {5, 8}, {6, 0x0F}};
    for (const auto& m : mistakes) {
        Bytes b = good;
        b[m.at] = m.value;
        MIMUSB_CHECK(walk(device_bytes(), c, &b, &e) == MIMUSB_ERR_PARAM &&
                     std::strcmp(e.field, "descriptors.bos") == 0);
        MIMUSB_CHECK(
            e.offset ==
            (m.at == 0 || m.at == 1 || m.at == 2 || m.at == 4 ? m.at : 5 + (m.at == 6 ? 1 : 0)));
    }
    // Only a header; only part of one; too long for the board.
    const Bytes empty = {5, 0x0F, 5, 0, 0};
    MIMUSB_CHECK(walk(device_bytes(), c, &empty, &e) == MIMUSB_OK);
    const Bytes short_header = {5, 0x0F, 4, 0};
    MIMUSB_CHECK(walk(device_bytes(), c, &short_header, &e) == MIMUSB_ERR_PARAM && e.offset == 0U);
    Bytes big(65, 0);
    big[0] = 5;
    big[1] = 0x0F;
    big[2] = 65;
    big[4] = 1;
    big[5] = 60;
    big[6] = 0x10;
    MIMUSB_CHECK(walk(device_bytes(), c, &big, &e) == MIMUSB_ERR_OVERFLOW && e.offset == 64U &&
                 e.length == 65U);
    // check_bos on its own, with the caller's field.
    MIMUSB_CHECK(mimusb::usb::check_bos(good.data(), good.size(), limits(), "bos", &e) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(mimusb::usb::check_bos(nullptr, 0, limits(), "bos", &e) == MIMUSB_ERR_PARAM &&
                 std::strcmp(e.field, "bos") == 0);
}

} // namespace

void test_descriptor_walker() {
    test_accepted();
    test_device_refused();
    test_configuration_refused();
    test_alternate_settings_and_isochronous();
    test_bos();
}
