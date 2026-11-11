// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Covers the descriptor composer byte for byte. The expected bytes were captured from the
 * descriptor builder the firmware had before the host composed them (the same configurations
 * went through that builder and the bytes were written down), so the target sees the same
 * descriptors as before. Also: the gap check of the interface numbers, the shared-interface
 * rules, and the serial number made from the chip id. */
#include "mimhid.h"
#include "mimhid_proto.h"
#include "test.hpp"
#include "usb/descriptor_composer.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using mimhid::usb::Composed;
using mimhid::usb::ComposerError;
using mimhid::usb::ComposerInput;
using mimhid::usb::ComposerNode;

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4U]);
        out.push_back(digits[b & 0x0FU]);
    }
    return out;
}

std::u16string ascii(const char* text) {
    std::u16string out;
    for (; *text != '\0'; ++text)
        out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*text)));
    return out;
}

// The report descriptors the captures used.
const std::vector<std::uint8_t> kRdA = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0};
const std::vector<std::uint8_t> kRdB = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0xC0};
const std::vector<std::uint8_t> kRdC = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
                                        0x75, 0x08, 0x95, 0x04, 0x09, 0x02, 0x81, 0x02, 0xC0};
const std::vector<std::uint8_t> kRdD = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0xC0};

void set_device(ComposerInput* in, std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd,
                std::uint8_t cls, std::uint8_t sub, std::uint8_t proto, std::uint16_t lang,
                std::uint16_t bcd_usb, std::uint8_t ep0, std::uint8_t attr, std::uint8_t power,
                bool serial_from_id) {
    *in = ComposerInput{};
    in->device.vendor_id = vid;
    in->device.product_id = pid;
    in->device.bcd_device = bcd;
    in->device.device_class = cls;
    in->device.device_subclass = sub;
    in->device.device_protocol = proto;
    in->device.language_id = lang;
    in->device.bcd_usb = bcd_usb;
    in->device.ep0_max_packet_size = ep0;
    in->device.serial_from_chip_id = serial_from_id ? 1U : 0U;
    in->config.configuration_value = 1;
    in->config.bm_attributes = attr;
    in->config.bmax_power = power;
    in->max_interfaces = 15;
    in->board_has_unique_id = true;
}

ComposerNode make_node(std::uint8_t number, std::uint8_t sub, std::uint8_t proto,
                       std::uint16_t bcd_hid, std::uint8_t country, std::uint8_t in_address,
                       std::uint16_t in_size, std::uint8_t in_interval, std::uint8_t out_address,
                       std::uint16_t out_size, std::uint8_t out_interval,
                       const std::vector<std::uint8_t>* descriptor, const char* name,
                       bool has_ids = false, std::uint8_t report_id = 0) {
    ComposerNode n;
    n.iface.number = number;
    n.iface.class_code = 0x03;
    n.iface.subclass = sub;
    n.iface.protocol = proto;
    n.iface.bcd_hid = bcd_hid;
    n.iface.country_code = country;
    n.ep_in = {in_address, in_interval, in_size};
    n.ep_out = {out_address, out_interval, out_size};
    n.report_descriptor = descriptor;
    n.interface_name = ascii(name);
    n.has_report_ids = has_ids;
    if (has_ids)
        n.report_ids.set(report_id);
    return n;
}

// The strings by index, as hex. An empty string is not sent at all.
std::map<int, std::string> strings_of(const Composed& c) {
    std::map<int, std::string> out;
    for (const auto& s : c.strings)
        out[s.index] = s.from_chip_id ? std::string("chip id") : hex(s.descriptor);
    return out;
}

void check_reports(const Composed& c, const std::vector<std::string>& expected) {
    MIMHID_CHECK(c.interfaces.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < c.interfaces.size(); ++i) {
        MIMHID_CHECK(c.interfaces[i].number == i);
        MIMHID_CHECK(hex(c.interfaces[i].report_descriptor) == expected[i]);
    }
}

Composed compose_ok(const ComposerInput& in) {
    Composed c;
    ComposerError e;
    MIMHID_CHECK(mimhid::usb::compose_descriptors(in, &c, &e) == MIMHID_OK);
    return c;
}

void test_minimal_keyboard() {
    ComposerInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 1, 1, 0x0111, 0, 0x81, 8, 10, 0, 0, 0, &kRdA, ""));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(hex(c.device) == "120100020000004034127856000100000001");
    MIMHID_CHECK(hex(c.configuration) ==
                 "0902220001010080320904000001030101000921110100012207000705810308000a");
    MIMHID_CHECK(c.strings.empty()); // no string: no index is set
    check_reports(c, {"05010906a101c0"});
}

void test_strings_and_two_out_endpoints() {
    ComposerInput in;
    set_device(&in, 0xCAFE, 0x4001, 0x0203, 0, 0, 0, 0x0409, 0x0200, 16, 0xA0, 100, false);
    in.manufacturer = ascii("Acme");
    in.product = ascii("Widget");
    in.serial = ascii("SN0001");
    in.configuration = ascii("Main");
    in.nodes.push_back(make_node(0, 0, 0, 0x0110, 33, 0x81, 8, 4, 0x01, 8, 4, &kRdA, "Keys"));
    in.nodes.push_back(make_node(1, 0, 2, 0x0111, 0, 0x82, 64, 1, 0, 0, 0, &kRdB, ""));
    in.nodes.push_back(make_node(2, 0, 0, 0x0101, 0, 0x85, 32, 8, 0x03, 16, 2, &kRdC, "Third"));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(hex(c.device) == "1201000200000010feca0140030201020301");
    MIMHID_CHECK(
        hex(c.configuration) ==
        "09026200030104a0640904000002030000050921100121012207000705810308000407050103080004090401"
        "000103000200092111010001220900070582034000010904020002030000070921010100012212000705850320"
        "000807050303100002");
    const std::map<int, std::string> s = strings_of(c);
    MIMHID_CHECK(s.size() == 6U);
    MIMHID_CHECK(s.at(1) == "0a03410063006d006500");
    MIMHID_CHECK(s.at(2) == "0e03570069006400670065007400");
    MIMHID_CHECK(s.at(3) == "0e0353004e003000300030003100");
    MIMHID_CHECK(s.at(4) == "0a034d00610069006e00");
    MIMHID_CHECK(s.at(5) == "0a034b00650079007300");     // interface 0: 5 + 0
    MIMHID_CHECK(s.at(7) == "0c0354006800690072006400"); // interface 2: 5 + 2
    for (const auto& text : c.strings)
        MIMHID_CHECK(text.language == 0x0409 && !text.from_chip_id);
    check_reports(c,
                  {"05010906a101c0", "05010902a1018501c0", "0600ff0901a10185027508950409028102c0"});
}

void test_shared_interface() {
    ComposerInput in;
    set_device(&in, 0x0001, 0x0002, 0x0003, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 250, false);
    in.nodes.push_back(
        make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdB, "Shared", true, 1));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 16, 5, 0, 0, 0, &kRdC, "", true, 2));
    in.nodes.push_back(
        make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdD, "Shared", true, 3));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(hex(c.device) == "120100020000004001000200030000000001");
    MIMHID_CHECK(
        hex(c.configuration) ==
        "09023b0002010080fa0904000001030000050921110100012212000705810310000509040100010300"
        "000009211101000122120007058203100005");
    const std::map<int, std::string> s = strings_of(c);
    MIMHID_CHECK(s.size() == 1U && s.at(5) == "0e03530068006100720065006400");
    // The Nodes' report descriptors are joined in node order.
    check_reports(c,
                  {"05010902a1018501c0050c0901a1018503c0", "0600ff0901a10185027508950409028102c0"});
}

void test_unicode_and_serial_from_chip_id() {
    ComposerInput in;
    set_device(&in, 0xFFFF, 0xFFFE, 0x9999, 0xEF, 2, 1, 0x0409, 0x0210, 8, 0xE0, 250, true);
    in.manufacturer = {0x00DC, 0x006E, 0x00EF, 0x0020, 0xD83D, 0xDE00};
    in.product = {0x65E5, 0x672C, 0x8A9E};
    for (int i = 0; i < MIMHID_STRING_MAX; ++i)
        in.configuration.push_back(static_cast<char16_t>('a' + i % 26));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x8F, 64, 255, 0x0F, 64, 255, &kRdD, ""));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(hex(c.device) == "12011002ef020108fffffeff999901020301");
    MIMHID_CHECK(
        hex(c.configuration) ==
        "09022900010104e0fa09040000020300000009211101000122090007058f034000ff07050f034000ff");
    const std::map<int, std::string> s = strings_of(c);
    MIMHID_CHECK(s.size() == 4U);
    MIMHID_CHECK(s.at(1) == "0e03dc006e00ef0020003dd800de");
    MIMHID_CHECK(s.at(2) == "0803e5652c679e8a");
    // The board makes the serial number from its chip id: no bytes are sent, the index is set.
    MIMHID_CHECK(s.at(3) == "chip id");
    // 126 units is the longest string: bLength 254 (USB 2.0 9.6.7).
    MIMHID_CHECK(c.strings.back().index == 4 && c.strings.back().descriptor.size() == 254U &&
                 c.strings.back().descriptor[0] == 0xFE);
    check_reports(c, {"050c0901a1018503c0"});

    // The same Device on a board without a unique id: no serial number string at all, as before;
    // and an explicit serial wins over the chip id.
    in.board_has_unique_id = false;
    Composed without = compose_ok(in);
    MIMHID_CHECK(without.device[16] == 0 && strings_of(without).count(3) == 0U);
    in.board_has_unique_id = true;
    in.serial = ascii("X");
    Composed explicit_serial = compose_ok(in);
    MIMHID_CHECK(explicit_serial.device[16] == 3 &&
                 strings_of(explicit_serial).at(3) == "04035800");
}

void test_four_interfaces_in_number_order() {
    ComposerInput in;
    set_device(&in, 0x0483, 0x5750, 0x0001, 0, 0, 0, 0x0809, 0x0200, 32, 0x80, 1, false);
    in.product = ascii("Quad");
    // Nodes in an order that is not the order of their interfaces.
    in.nodes.push_back(make_node(3, 0, 0, 0x0111, 0, 0x84, 8, 1, 0, 0, 0, &kRdA, "d"));
    in.nodes.push_back(make_node(0, 1, 1, 0x0111, 0, 0x81, 8, 2, 0x02, 8, 3, &kRdB, "a"));
    in.nodes.push_back(make_node(2, 1, 2, 0x0111, 0, 0x83, 64, 3, 0, 0, 0, &kRdC, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0200, 0, 0x82, 64, 4, 0x05, 64, 5, &kRdD, "b"));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(hex(c.device) == "120100020000002083045057010000020001");
    MIMHID_CHECK(
        hex(c.configuration) ==
        "09027b000401008001090400000203010105092111010001220900070581030800020705020308000309040100"
        "020300000609210002000122090007058203400004070505034000050904020001030102000921110100012212"
        "000705830340000309040300010300000809211101000122070007058403080001");
    const std::map<int, std::string> s = strings_of(c);
    MIMHID_CHECK(s.size() == 4U);
    MIMHID_CHECK(s.at(2) == "0a035100750061006400" && s.at(5) == "04036100" &&
                 s.at(6) == "04036200" && s.at(8) == "04036400");
    check_reports(c, {"05010902a1018501c0", "050c0901a1018503c0",
                      "0600ff0901a10185027508950409028102c0", "05010906a101c0"});
}

ComposerError compose_error(const ComposerInput& in, mimhid_result expected) {
    Composed c;
    ComposerError e;
    MIMHID_CHECK(mimhid::usb::compose_descriptors(in, &c, &e) == expected);
    MIMHID_CHECK(e.result == expected && e.field != nullptr && e.reason != nullptr);
    return e;
}

// Interface numbers are real: 0, 1, 2, ... without a gap, and the error names the missing number.
void test_gap_in_interface_numbers() {
    ComposerInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(2, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    ComposerError e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(e.field, "node_config.iface.number") == 0 && e.offset == 1U);

    // Not starting at 0: 0 is the missing number.
    in.nodes.clear();
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(e.offset == 0U);

    // The first missing number is named when several are.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(4, 0, 0, 0x0111, 0, 0x85, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(6, 0, 0, 0x0111, 0, 0x87, 8, 1, 0, 0, 0, &kRdA, ""));
    e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(e.offset == 2U);

    // 0, 1, 2 with Nodes out of order and one interface shared: fine.
    in.nodes.clear();
    in.nodes.push_back(make_node(2, 0, 0, 0x0111, 0, 0x83, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdC, "", true, 2));
    const Composed c = compose_ok(in);
    MIMHID_CHECK(c.interfaces.size() == 3U && c.configuration[4] == 3U);
}

void test_interface_limit() {
    ComposerInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.max_interfaces = 2;
    for (std::uint8_t i = 0; i < 3; ++i)
        in.nodes.push_back(make_node(i, 0, 0, 0x0111, 0, static_cast<std::uint8_t>(0x81 + i), 8, 1,
                                     0, 0, 0, &kRdA, ""));
    const ComposerError e = compose_error(in, MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(e.offset == 2U && e.length == 3U);
}

void test_shared_interface_rules() {
    ComposerInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    // Without Report IDs two Nodes cannot share an interface (HID 1.11 6.2.2.7).
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    ComposerError e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(e.field, "node.report_id") == 0);
    // The same Report ID twice.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(e.field, "node.report_id") == 0);
    // Different settings on one interface.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 1, 0, 0, 0, &kRdC, "", true, 2));
    e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(e.field, "node_config.iface") == 0);
    in.nodes.back() = make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdC, "name", true, 2);
    e = compose_error(in, MIMHID_ERR_PARAM); // another interface name
    MIMHID_CHECK(std::strcmp(e.field, "node_config.iface") == 0);
}

void test_endpoint_address_used_twice() {
    ComposerInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    const ComposerError e = compose_error(in, MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(e.field, "node_config.ep_in.address") == 0 && e.length == 1U);
    // An OUT endpoint of another interface on the same address as an OUT endpoint.
    in.nodes.back() = make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0x01, 8, 1, &kRdA, "");
    in.nodes.front() = make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0x01, 8, 1, &kRdA, "");
    compose_error(in, MIMHID_ERR_PARAM);
    // IN and OUT of one number are different addresses (USB 2.0 9.6.6): fine.
    in.nodes.back() = make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0x02, 8, 1, &kRdA, "");
    compose_ok(in);
}

} // namespace

void test_descriptor_composer() {
    test_minimal_keyboard();
    test_strings_and_two_out_endpoints();
    test_shared_interface();
    test_unicode_and_serial_from_chip_id();
    test_four_interfaces_in_number_order();
    test_gap_in_interface_numbers();
    test_interface_limit();
    test_shared_interface_rules();
    test_endpoint_address_used_twice();
}
