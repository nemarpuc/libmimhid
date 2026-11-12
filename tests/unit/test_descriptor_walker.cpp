// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The checks of a Device given as bytes and of a BOS descriptor: what is accepted, and for every
 * way a descriptor can fail to hold together, the result and the offset the refusal names. */
#include "mimhid.h"
#include "test.hpp"
#include "usb/descriptor_walker.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using mimhid::usb::ComposerError;
using mimhid::usb::ComposerLimits;
using mimhid::usb::WalkedDescriptors;
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

mimhid_result walk(const Bytes& d, const Bytes& c, const Bytes* bos, ComposerError* e,
                   WalkedDescriptors* out = nullptr, bool high_speed = false) {
    WalkedDescriptors local;
    return mimhid::usb::walk_descriptors(
        d.data(), d.size(), c.data(), c.size(), bos == nullptr ? nullptr : bos->data(),
        bos == nullptr ? 0U : bos->size(), limits(high_speed), out != nullptr ? out : &local, e);
}

// The result and offset of the refusal of a configuration with one byte changed.
void config_refused(std::size_t at, std::uint8_t value, mimhid_result result,
                    std::uint32_t offset) {
    Bytes c = config_bytes();
    c[at] = value;
    ComposerError e;
    const mimhid_result got = walk(device_bytes(), c, nullptr, &e);
    if (got != result || e.offset != offset)
        std::fprintf(stderr, "config[%zu]=%u: result %d offset %u, wanted %d at %u\n", at, value,
                     static_cast<int>(got), e.offset, static_cast<int>(result), offset);
    MIMHID_CHECK(got == result);
    MIMHID_CHECK(e.result == result && e.offset == offset);
    MIMHID_CHECK(std::strcmp(e.field, "descriptors.configuration") == 0);
}

void test_accepted() {
    ComposerError e;
    WalkedDescriptors w;
    MIMHID_CHECK(walk(device_bytes(), config_bytes(), nullptr, &e, &w) == MIMHID_OK);
    MIMHID_CHECK(w.interface_count == 1U && w.endpoints.size() == 2U && w.bcd_usb == 0x0210U);
    MIMHID_CHECK(w.endpoints[0].address == 0x81U && w.endpoints[0].offset == 18U &&
                 w.endpoints[1].address == 0x01U && w.endpoints[1].interface_number == 0U);
    // A BOS rides along with bcdUSB 0x0201 or more.
    const Bytes bos = bos_bytes();
    MIMHID_CHECK(walk(device_bytes(0x0201), config_bytes(), &bos, &e) == MIMHID_OK);
    // A class descriptor between the interface and its endpoints, as given.
    Bytes c = config_bytes();
    c.insert(c.begin() + 18, {9, 0x21, 0x11, 0x01, 0, 1, 0x22, 4, 0});
    c[2] = static_cast<std::uint8_t>(c.size());
    MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e, &w) == MIMHID_OK &&
                 w.endpoints[0].offset == 27U);
    // An association over two interfaces.
    Bytes two = {9, 2, 0, 0, 2,    1, 0, 0x80, 50, 8, 0x0B, 0, 2, 0xFF, 1, 2, 0, 9,
                 4, 0, 0, 0, 0xFF, 1, 2, 0,    9,  4, 1,    0, 0, 0xFF, 1, 2, 0};
    two[2] = static_cast<std::uint8_t>(two.size());
    MIMHID_CHECK(walk(device_bytes(), two, nullptr, &e, &w) == MIMHID_OK &&
                 w.interface_count == 2U);
}

void test_device_refused() {
    ComposerError e;
    Bytes d = device_bytes();
    const Bytes c = config_bytes();
    d.pop_back();
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 0U && e.length == 17U &&
                 std::strcmp(e.field, "descriptors.device") == 0);
    d = device_bytes();
    d[0] = 17;
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 0U);
    d = device_bytes();
    d[1] = 2;
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 1U);
    d = device_bytes(0);
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_UNSET_FIELD && e.offset == 2U);
    d = device_bytes();
    d[7] = 63;
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 7U);
    d[7] = 8; // full speed takes 8
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_OK);
    MIMHID_CHECK(walk(d, c, nullptr, &e, nullptr, true) == MIMHID_ERR_PARAM && e.offset == 7U);
    d = device_bytes();
    d[17] = 2;
    MIMHID_CHECK(walk(d, c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 17U);
    WalkedDescriptors w;
    MIMHID_CHECK(mimhid::usb::walk_descriptors(nullptr, 0, c.data(), c.size(), nullptr, 0, limits(),
                                               &w, &e) == MIMHID_ERR_PARAM &&
                 e.offset == 0U);
}

void test_configuration_refused() {
    ComposerError e;
    // The header.
    config_refused(0, 8, MIMHID_ERR_PARAM, 0);
    config_refused(1, 3, MIMHID_ERR_PARAM, 1);
    config_refused(2, 33, MIMHID_ERR_PARAM, 2); // wTotalLength
    config_refused(2, 31, MIMHID_ERR_PARAM, 2);
    config_refused(4, 2, MIMHID_ERR_PARAM, 4);    // bNumInterfaces
    config_refused(5, 2, MIMHID_ERR_PARAM, 5);    // bConfigurationValue
    config_refused(7, 0x00, MIMHID_ERR_PARAM, 7); // D7 clear
    config_refused(7, 0x81, MIMHID_ERR_PARAM, 7); // reserved bits
    // The interface.
    config_refused(9, 8, MIMHID_ERR_PARAM, 9);         // bLength
    config_refused(11, 1, MIMHID_ERR_PARAM, 11);       // number must be 0
    config_refused(12, 1, MIMHID_ERR_UNSUPPORTED, 12); // alternate setting
    config_refused(13, 1, MIMHID_ERR_PARAM,
                   25); // one declared: the second endpoint is one too many
    config_refused(13, 3, MIMHID_ERR_PARAM, 13); // three declared, two follow (at the interface)
    // The endpoints.
    config_refused(18, 6, MIMHID_ERR_PARAM, 18);       // bLength of an endpoint
    config_refused(20, 0x91, MIMHID_ERR_PARAM, 20);    // bits 6..4
    config_refused(20, 0x80, MIMHID_ERR_PARAM, 20);    // number 0
    config_refused(20, 0x88, MIMHID_ERR_OVERFLOW, 20); // above the controller
    config_refused(20, 0x01, MIMHID_ERR_PARAM, 27);    // the second endpoint reuses the address
    config_refused(21, 1, MIMHID_ERR_UNSUPPORTED, 21); // isochronous
    config_refused(21, 0, MIMHID_ERR_UNSUPPORTED, 21); // control
    config_refused(22, 63, MIMHID_ERR_PARAM, 20);      // bulk packet size
    config_refused(22, 0, MIMHID_ERR_UNSET_FIELD, 20); // zero packet size
    // A descriptor that does not fit, and one with nothing to hold it.
    {
        Bytes c = config_bytes();
        c[25] = 9; // the last endpoint claims 9 bytes: past the end
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 25U);
        c = config_bytes();
        c[25] = 1; // bLength below 2
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 25U);
        c = config_bytes();
        c.erase(c.begin() + 9, c.begin() + 18); // endpoints with no interface
        c[2] = static_cast<std::uint8_t>(c.size());
        c[4] = 0;
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM);
        c = {9, 2, 9, 0, 0, 1, 0, 0x80, 50}; // no interface at all
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 9U);
        c = config_bytes();
        c.resize(8); // not even the header
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 0U);
    }
    // Interface numbers 0, 1, 2 ...; a second interface that jumps to 2.
    {
        Bytes c = config_bytes();
        const Bytes second = {9, 4, 2, 0, 0, 0xFF, 1, 2, 0};
        c.insert(c.end(), second.begin(), second.end());
        c[2] = static_cast<std::uint8_t>(c.size());
        c[4] = 2;
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 34U &&
                     e.length == 1U);
    }
    // An interval of an interrupt endpoint, and the totals of the board.
    {
        Bytes c = config_bytes();
        c[21] = 3;
        c[22] = 16;
        c[24] = 0; // bInterval missing
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_UNSET_FIELD);
        c[24] = 4;
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_OK);
        ComposerLimits l = limits();
        l.max_interfaces = 0;
        Bytes d = device_bytes();
        Bytes ok = config_bytes();
        WalkedDescriptors w;
        MIMHID_CHECK(mimhid::usb::walk_descriptors(d.data(), d.size(), ok.data(), ok.size(),
                                                   nullptr, 0, l, &w, &e) == MIMHID_ERR_OVERFLOW);
        l = limits();
        l.max_config_bytes = 31;
        MIMHID_CHECK(mimhid::usb::walk_descriptors(d.data(), d.size(), ok.data(), ok.size(),
                                                   nullptr, 0, l, &w, &e) == MIMHID_ERR_OVERFLOW &&
                     e.offset == 31U && e.length == 32U);
    }
    // An association that names interfaces that are not there, or not the next one.
    {
        Bytes c = {9,    2, 0, 0, 1, 1, 0, 0x80, 50, 8,    0x0B, 0, 2,
                   0xFF, 1, 2, 0, 9, 4, 0, 0,    0,  0xFF, 1,    2, 0};
        c[2] = static_cast<std::uint8_t>(c.size());
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM &&
                     e.offset == c.size());
        c[11] = 1; // bFirstInterface is not the next interface
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 11U);
        c[11] = 0;
        c[12] = 0; // names none
        MIMHID_CHECK(walk(device_bytes(), c, nullptr, &e) == MIMHID_ERR_PARAM && e.offset == 9U);
    }
}

void test_bos() {
    ComposerError e;
    const Bytes c = config_bytes();
    const Bytes good = bos_bytes();
    // A BOS needs bcdUSB 0x0201 or more.
    MIMHID_CHECK(walk(device_bytes(0x0200), c, &good, &e) == MIMHID_ERR_PARAM && e.offset == 2U &&
                 std::strcmp(e.field, "descriptors.device") == 0);
    MIMHID_CHECK(walk(device_bytes(0x0110), c, &good, &e) == MIMHID_ERR_PARAM);
    // Each way the descriptor fails to hold together, with the offset it names.
    const struct {
        std::size_t at;
        std::uint8_t value;
    } mistakes[] = {{0, 4}, {1, 0x10}, {2, 11}, {2, 13}, {4, 0}, {4, 2}, {5, 2}, {5, 8}, {6, 0x0F}};
    for (const auto& m : mistakes) {
        Bytes b = good;
        b[m.at] = m.value;
        MIMHID_CHECK(walk(device_bytes(), c, &b, &e) == MIMHID_ERR_PARAM &&
                     std::strcmp(e.field, "descriptors.bos") == 0);
        MIMHID_CHECK(
            e.offset ==
            (m.at == 0 || m.at == 1 || m.at == 2 || m.at == 4 ? m.at : 5 + (m.at == 6 ? 1 : 0)));
    }
    // Only a header; only part of one; too long for the board.
    const Bytes empty = {5, 0x0F, 5, 0, 0};
    MIMHID_CHECK(walk(device_bytes(), c, &empty, &e) == MIMHID_OK);
    const Bytes short_header = {5, 0x0F, 4, 0};
    MIMHID_CHECK(walk(device_bytes(), c, &short_header, &e) == MIMHID_ERR_PARAM && e.offset == 0U);
    Bytes big(65, 0);
    big[0] = 5;
    big[1] = 0x0F;
    big[2] = 65;
    big[4] = 1;
    big[5] = 60;
    big[6] = 0x10;
    MIMHID_CHECK(walk(device_bytes(), c, &big, &e) == MIMHID_ERR_OVERFLOW && e.offset == 64U &&
                 e.length == 65U);
    // check_bos on its own, with the caller's field.
    MIMHID_CHECK(mimhid::usb::check_bos(good.data(), good.size(), limits(), "bos", &e) ==
                 MIMHID_OK);
    MIMHID_CHECK(mimhid::usb::check_bos(nullptr, 0, limits(), "bos", &e) == MIMHID_ERR_PARAM &&
                 std::strcmp(e.field, "bos") == 0);
}

} // namespace

void test_descriptor_walker() {
    test_accepted();
    test_device_refused();
    test_configuration_refused();
    test_bos();
}
