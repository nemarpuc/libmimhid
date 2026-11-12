// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Covers the descriptor composer byte for byte. The expected bytes were captured from the
 * descriptor builder the firmware had before the host composed them (the same configurations
 * went through that builder and the bytes were written down), so the target sees the same
 * descriptors as before. Also: the gap check of the interface numbers, the shared-interface
 * rules, and the serial number made from the chip id. */
#include "mimusb.h"
#include "mimusb_proto.h"
#include "test.hpp"
#include "usb/descriptor_composer.hpp"
#include "usb/hid_layer.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using mimusb::usb::Composed;
using mimusb::usb::ComposerError;
using mimusb::usb::ComposerInput;
using mimusb::usb::HidNodeInput;

// The step 1 vectors are of HID Nodes. The HID layer joins them into interfaces and the composer
// writes the descriptors, as commit_device does: this wrapper does both from the old inputs.
struct TestInput : ComposerInput {
    std::vector<HidNodeInput> nodes;
    std::size_t max_interfaces{15};
    bool board_has_unique_id{true};
};

// The report descriptor of each interface of the last compose(), as hex: the composer no longer
// carries them (the HID layer's ANSWER rule for GET_DESCRIPTOR of type 0x22 does), so the wrapper
// reads them from the rules the way the board would answer. Test thread only.
std::map<std::size_t, std::string> g_report_descriptors;

std::string hex(const std::vector<std::uint8_t>& bytes);

mimusb_result compose(const TestInput& in, Composed* out, ComposerError* error) {
    ComposerInput full = in;
    full.limits.max_interfaces = in.max_interfaces;
    full.limits.max_endpoint_number = 15;
    full.limits.max_endpoint_bytes = 64;
    full.limits.max_config_bytes = 1024;
    full.limits.max_strings = 24;
    full.limits.endpoint_queue_slots = 64;
    full.limits.transfer_types = (1U << MIMUSB_TRANSFER_BULK) | (1U << MIMUSB_TRANSFER_INTERRUPT);
    full.limits.has_unique_id = in.board_has_unique_id;
    const mimusb_result grouped = mimusb::usb::compose_hid_groups(in.nodes, &full.groups, error);
    if (grouped != MIMUSB_OK)
        return grouped;
    g_report_descriptors.clear();
    for (const mimusb::usb::BoardRule& rule : mimusb::usb::compose_hid_rules(in.nodes)) {
        if (rule.rule.action == MIMUSB_CONTROL_ANSWER && rule.rule.request_value == 0x06U &&
            rule.rule.value_value == 0x2200U)
            g_report_descriptors[rule.iface] = hex(rule.data);
    }
    return mimusb::usb::compose_descriptors(full, out, error);
}

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

void set_device(TestInput* in, std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd,
                std::uint8_t cls, std::uint8_t sub, std::uint8_t proto, std::uint16_t lang,
                std::uint16_t bcd_usb, std::uint8_t ep0, std::uint8_t attr, std::uint8_t power,
                bool serial_from_id) {
    *in = TestInput{};
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
}

HidNodeInput make_node(std::uint8_t number, std::uint8_t sub, std::uint8_t proto,
                       std::uint16_t bcd_hid, std::uint8_t country, std::uint8_t in_address,
                       std::uint16_t in_size, std::uint8_t in_interval, std::uint8_t out_address,
                       std::uint16_t out_size, std::uint8_t out_interval,
                       const std::vector<std::uint8_t>* descriptor, const char* name,
                       bool has_ids = false, std::uint8_t report_id = 0) {
    HidNodeInput n;
    mimusb_hid_node_config& c = n.config;
    c.interface_number = number;
    c.subclass = sub;
    c.protocol = proto;
    c.bcd_hid = bcd_hid;
    c.country_code = country;
    c.ep_in.address = in_address;
    c.ep_in.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    c.ep_in.interval = in_interval;
    c.ep_in.max_packet_size = in_size;
    c.ep_in.policy = {MIMUSB_FULL_DROP_NEWEST, 7};
    if (out_address != 0) {
        c.ep_out.address = out_address;
        c.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        c.ep_out.interval = out_interval;
        c.ep_out.max_packet_size = out_size;
    }
    n.report_descriptor = descriptor;
    n.name = ascii(name);
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
    MIMUSB_CHECK(c.interfaces.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < c.interfaces.size(); ++i) {
        MIMUSB_CHECK(c.interfaces[i].number == i);
        MIMUSB_CHECK(g_report_descriptors[i] == expected[i]);
    }
}

Composed compose_ok(const TestInput& in) {
    Composed c;
    ComposerError e;
    MIMUSB_CHECK(compose(in, &c, &e) == MIMUSB_OK);
    return c;
}

void test_minimal_keyboard() {
    TestInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 1, 1, 0x0111, 0, 0x81, 8, 10, 0, 0, 0, &kRdA, ""));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(hex(c.device) == "120100020000004034127856000100000001");
    MIMUSB_CHECK(hex(c.configuration) ==
                 "0902220001010080320904000001030101000921110100012207000705810308000a");
    MIMUSB_CHECK(c.strings.empty()); // no string: no index is set
    check_reports(c, {"05010906a101c0"});
}

void test_strings_and_two_out_endpoints() {
    TestInput in;
    set_device(&in, 0xCAFE, 0x4001, 0x0203, 0, 0, 0, 0x0409, 0x0200, 16, 0xA0, 100, false);
    in.manufacturer = ascii("Acme");
    in.product = ascii("Widget");
    in.serial = ascii("SN0001");
    in.configuration = ascii("Main");
    in.nodes.push_back(make_node(0, 0, 0, 0x0110, 33, 0x81, 8, 4, 0x01, 8, 4, &kRdA, "Keys"));
    in.nodes.push_back(make_node(1, 0, 2, 0x0111, 0, 0x82, 64, 1, 0, 0, 0, &kRdB, ""));
    in.nodes.push_back(make_node(2, 0, 0, 0x0101, 0, 0x85, 32, 8, 0x03, 16, 2, &kRdC, "Third"));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(hex(c.device) == "1201000200000010feca0140030201020301");
    MIMUSB_CHECK(
        hex(c.configuration) ==
        "09026200030104a0640904000002030000050921100121012207000705810308000407050103080004090401"
        "000103000200092111010001220900070582034000010904020002030000060921010100012212000705850320"
        "000807050303100002");
    const std::map<int, std::string> s = strings_of(c);
    MIMUSB_CHECK(s.size() == 6U);
    MIMUSB_CHECK(s.at(1) == "0a03410063006d006500");
    MIMUSB_CHECK(s.at(2) == "0e03570069006400670065007400");
    MIMUSB_CHECK(s.at(3) == "0e0353004e003000300030003100");
    MIMUSB_CHECK(s.at(4) == "0a034d00610069006e00");
    MIMUSB_CHECK(s.at(5) == "0a034b00650079007300");
    // The names take the lowest free indices from 5, in interface order (the fixed 5 + interface
    // number of step 1 is gone): "Keys" of interface 0 is 5, "Third" of interface 2 is 6.
    MIMUSB_CHECK(s.at(6) == "0c0354006800690072006400");
    for (const auto& text : c.strings)
        MIMUSB_CHECK(text.language == 0x0409 && !text.from_chip_id);
    check_reports(c,
                  {"05010906a101c0", "05010902a1018501c0", "0600ff0901a10185027508950409028102c0"});
}

void test_shared_interface() {
    TestInput in;
    set_device(&in, 0x0001, 0x0002, 0x0003, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 250, false);
    in.nodes.push_back(
        make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdB, "Shared", true, 1));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 16, 5, 0, 0, 0, &kRdC, "", true, 2));
    in.nodes.push_back(
        make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdD, "Shared", true, 3));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(hex(c.device) == "120100020000004001000200030000000001");
    MIMUSB_CHECK(
        hex(c.configuration) ==
        "09023b0002010080fa0904000001030000050921110100012212000705810310000509040100010300"
        "000009211101000122120007058203100005");
    const std::map<int, std::string> s = strings_of(c);
    MIMUSB_CHECK(s.size() == 1U && s.at(5) == "0e03530068006100720065006400");
    // The Nodes' report descriptors are joined in node order.
    check_reports(c,
                  {"05010902a1018501c0050c0901a1018503c0", "0600ff0901a10185027508950409028102c0"});
}

void test_unicode_and_serial_from_chip_id() {
    TestInput in;
    set_device(&in, 0xFFFF, 0xFFFE, 0x9999, 0xEF, 2, 1, 0x0409, 0x0210, 8, 0xE0, 250, true);
    in.manufacturer = {0x00DC, 0x006E, 0x00EF, 0x0020, 0xD83D, 0xDE00};
    in.product = {0x65E5, 0x672C, 0x8A9E};
    for (int i = 0; i < MIMUSB_STRING_MAX; ++i)
        in.configuration.push_back(static_cast<char16_t>('a' + i % 26));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x8F, 64, 255, 0x0F, 64, 255, &kRdD, ""));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(hex(c.device) == "12011002ef020108fffffeff999901020301");
    MIMUSB_CHECK(
        hex(c.configuration) ==
        "09022900010104e0fa09040000020300000009211101000122090007058f034000ff07050f034000ff");
    const std::map<int, std::string> s = strings_of(c);
    MIMUSB_CHECK(s.size() == 4U);
    MIMUSB_CHECK(s.at(1) == "0e03dc006e00ef0020003dd800de");
    MIMUSB_CHECK(s.at(2) == "0803e5652c679e8a");
    // The board makes the serial number from its chip id: no bytes are sent, the index is set.
    MIMUSB_CHECK(s.at(3) == "chip id");
    // 126 units is the longest string: bLength 254 (USB 2.0 9.6.7).
    MIMUSB_CHECK(c.strings.back().index == 4 && c.strings.back().descriptor.size() == 254U &&
                 c.strings.back().descriptor[0] == 0xFE);
    check_reports(c, {"050c0901a1018503c0"});

    // The same Device on a board without a unique id: no serial number string at all, as before;
    // and an explicit serial wins over the chip id.
    in.board_has_unique_id = false;
    Composed without = compose_ok(in);
    MIMUSB_CHECK(without.device[16] == 0 && strings_of(without).count(3) == 0U);
    in.board_has_unique_id = true;
    in.serial = ascii("X");
    Composed explicit_serial = compose_ok(in);
    MIMUSB_CHECK(explicit_serial.device[16] == 3 &&
                 strings_of(explicit_serial).at(3) == "04035800");
}

void test_four_interfaces_in_number_order() {
    TestInput in;
    set_device(&in, 0x0483, 0x5750, 0x0001, 0, 0, 0, 0x0809, 0x0200, 32, 0x80, 1, false);
    in.product = ascii("Quad");
    // Nodes in an order that is not the order of their interfaces.
    in.nodes.push_back(make_node(3, 0, 0, 0x0111, 0, 0x84, 8, 1, 0, 0, 0, &kRdA, "d"));
    in.nodes.push_back(make_node(0, 1, 1, 0x0111, 0, 0x81, 8, 2, 0x02, 8, 3, &kRdB, "a"));
    in.nodes.push_back(make_node(2, 1, 2, 0x0111, 0, 0x83, 64, 3, 0, 0, 0, &kRdC, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0200, 0, 0x82, 64, 4, 0x05, 64, 5, &kRdD, "b"));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(hex(c.device) == "120100020000002083045057010000020001");
    MIMUSB_CHECK(
        hex(c.configuration) ==
        "09027b000401008001090400000203010105092111010001220900070581030800020705020308000309040100"
        "020300000609210002000122090007058203400004070505034000050904020001030102000921110100012212"
        "000705830340000309040300010300000709211101000122070007058403080001");
    const std::map<int, std::string> s = strings_of(c);
    MIMUSB_CHECK(s.size() == 4U);
    MIMUSB_CHECK(s.at(2) == "0a035100750061006400" && s.at(5) == "04036100" &&
                 s.at(6) == "04036200" && s.at(7) == "04036400");
    check_reports(c, {"05010902a1018501c0", "050c0901a1018503c0",
                      "0600ff0901a10185027508950409028102c0", "05010906a101c0"});
}

ComposerError compose_error(const TestInput& in, mimusb_result expected) {
    Composed c;
    ComposerError e;
    MIMUSB_CHECK(compose(in, &c, &e) == expected);
    MIMUSB_CHECK(e.result == expected && e.field != nullptr && e.reason != nullptr);
    return e;
}

// Interface numbers are real: 0, 1, 2, ... without a gap, and the error names the missing number.
void test_gap_in_interface_numbers() {
    TestInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(2, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    ComposerError e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(e.field, "node_config.interfaces.number") == 0 && e.offset == 1U);

    // Not starting at 0: 0 is the missing number.
    in.nodes.clear();
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(e.offset == 0U);

    // The first missing number is named when several are.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(4, 0, 0, 0x0111, 0, 0x85, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(6, 0, 0, 0x0111, 0, 0x87, 8, 1, 0, 0, 0, &kRdA, ""));
    e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(e.offset == 2U);

    // 0, 1, 2 with Nodes out of order and one interface shared: fine.
    in.nodes.clear();
    in.nodes.push_back(make_node(2, 0, 0, 0x0111, 0, 0x83, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdC, "", true, 2));
    const Composed c = compose_ok(in);
    MIMUSB_CHECK(c.interfaces.size() == 3U && c.configuration[4] == 3U);
}

void test_interface_limit() {
    TestInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.max_interfaces = 2;
    for (std::uint8_t i = 0; i < 3; ++i)
        in.nodes.push_back(make_node(i, 0, 0, 0x0111, 0, static_cast<std::uint8_t>(0x81 + i), 8, 1,
                                     0, 0, 0, &kRdA, ""));
    const ComposerError e = compose_error(in, MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(e.offset == 2U && e.length == 3U);
}

void test_shared_interface_rules() {
    TestInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    // Without Report IDs two Nodes cannot share an interface (HID 1.11 6.2.2.7).
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    ComposerError e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(e.field, "node.report_id") == 0);
    // The same Report ID twice.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(e.field, "node.report_id") == 0);
    // Different settings on one interface.
    in.nodes.clear();
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdB, "", true, 1));
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 1, 0, 0, 0, &kRdC, "", true, 2));
    e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(e.field, "node_config.iface") == 0);
    in.nodes.back() = make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdC, "name", true, 2);
    e = compose_error(in, MIMUSB_ERR_PARAM); // another interface name
    MIMUSB_CHECK(std::strcmp(e.field, "node_config.iface") == 0);
}

void test_endpoint_address_used_twice() {
    TestInput in;
    set_device(&in, 0x1234, 0x5678, 0x0100, 0, 0, 0, 0x0409, 0x0200, 64, 0x80, 50, false);
    in.nodes.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    in.nodes.push_back(make_node(1, 0, 0, 0x0111, 0, 0x81, 8, 1, 0, 0, 0, &kRdA, ""));
    const ComposerError e = compose_error(in, MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(e.field, "node_config.interfaces.endpoints.address") == 0 &&
                 e.length == 1U);
    // An OUT endpoint of another interface on the same address as an OUT endpoint.
    in.nodes.back() = make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0x01, 8, 1, &kRdA, "");
    in.nodes.front() = make_node(0, 0, 0, 0x0111, 0, 0x81, 8, 1, 0x01, 8, 1, &kRdA, "");
    compose_error(in, MIMUSB_ERR_PARAM);
    // IN and OUT of one number are different addresses (USB 2.0 9.6.6): fine.
    in.nodes.back() = make_node(1, 0, 0, 0x0111, 0, 0x82, 8, 1, 0x02, 8, 1, &kRdA, "");
    compose_ok(in);
}

// ---- interfaces of any class (no HID layer) ----
mimusb_usb_endpoint general_ep(std::uint8_t address, std::uint8_t type, std::uint16_t size,
                               std::uint8_t interval = 0) {
    mimusb_usb_endpoint e{};
    e.address = address;
    e.transfer_type = type;
    e.max_packet_size = size;
    e.interval = interval;
    if ((address & 0x80U) != 0U)
        e.policy = {MIMUSB_FULL_WAIT, 2};
    return e;
}

mimusb::usb::ComposerLimits full_speed_limits() {
    mimusb::usb::ComposerLimits lim;
    lim.max_interfaces = 15;
    lim.max_endpoint_number = 15;
    lim.max_endpoint_bytes = 64;
    lim.max_config_bytes = 1024;
    lim.max_strings = 24;
    lim.endpoint_queue_slots = 32;
    lim.transfer_types = 0x0C;
    return lim;
}

ComposerInput general_input(std::vector<mimusb::usb::ComposerGroup> groups) {
    ComposerInput in;
    in.device.vendor_id = 0x1234;
    in.device.product_id = 0x5678;
    in.device.bcd_usb = 0x0200;
    in.device.ep0_max_packet_size = 64;
    in.device.language_id = 0x0409;
    in.config.configuration_value = 1;
    in.config.bm_attributes = 0x80;
    in.config.bmax_power = 50;
    in.groups = std::move(groups);
    in.limits = full_speed_limits();
    return in;
}

mimusb::usb::ComposerInterface general_itf(std::uint8_t number, std::uint8_t cls,
                                           std::vector<mimusb_usb_endpoint> eps) {
    mimusb::usb::ComposerInterface i;
    i.number = number;
    i.class_code = cls;
    i.endpoints = std::move(eps);
    return i;
}

void test_general_vendor_and_association() {
    mimusb::usb::ComposerGroup g;
    g.interfaces.push_back(
        general_itf(0, 0xFF, {general_ep(0x81, 2, 64), general_ep(0x01, 2, 64)}));
    g.interfaces[0].class_descriptors = {3, 0x24, 7};
    Composed c;
    ComposerError e;
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(general_input({g}), &c, &e) == MIMUSB_OK);
    // Configuration, Interface (class 0xFF, 2 endpoints), the class bytes as given, two Endpoint
    // descriptors (USB 2.0 9.6.3, 9.6.5, 9.6.6).
    MIMUSB_CHECK(hex(c.configuration) == "09022300010100803209040000"
                                         "02ff000000"
                                         "032407"
                                         "07058102400000"
                                         "07050102400000");
    MIMUSB_CHECK(c.interfaces.size() == 1U);

    // An association goes before the first interface of its Node, which is its bFirstInterface.
    mimusb::usb::ComposerGroup comm, hid;
    hid.interfaces.push_back(general_itf(0, 0xFF, {general_ep(0x82, 2, 64)}));
    comm.interfaces.push_back(general_itf(1, 0x02, {general_ep(0x83, 3, 16, 4)}));
    comm.interfaces.push_back(
        general_itf(2, 0x0A, {general_ep(0x84, 2, 64), general_ep(0x04, 2, 64)}));
    comm.association = {8, 0x0B, 1, 2, 0x02, 0x02, 0x01, 0};
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(general_input({comm, hid}), &c, &e) == MIMUSB_OK);
    MIMUSB_CHECK(c.configuration[4] == 3 && c.interfaces.size() == 3U);
    std::size_t at = 9 + 9 + 7; // after interface 0 and its endpoint
    MIMUSB_CHECK(c.configuration[at] == 8 && c.configuration[at + 1] == 0x0B &&
                 c.configuration[at + 8 + 2] == 1);

    // Refusals.
    const auto refused = [&](mimusb::usb::ComposerGroup group, mimusb_result want,
                             const char* field) {
        ComposerError err;
        Composed out;
        MIMUSB_CHECK(mimusb::usb::compose_descriptors(general_input({group}), &out, &err) == want);
        MIMUSB_CHECK(want == MIMUSB_OK || (err.field != nullptr && std::strstr(err.field, field)));
    };
    mimusb::usb::ComposerGroup base;
    base.interfaces.push_back(general_itf(0, 0xFF, {general_ep(0x81, 2, 64)}));
    mimusb::usb::ComposerGroup m = base;
    m.association = {8, 0x0B, 0, 1, 0xFF, 0, 0, 0};
    refused(m, MIMUSB_OK, "");
    m.association = {8, 0x0B, 0, 2, 0xFF, 0, 0, 0};
    refused(m, MIMUSB_ERR_PARAM, "association");
    m.association = {8, 0x0B, 1, 1, 0xFF, 0, 0, 0};
    refused(m, MIMUSB_ERR_PARAM, "association");
    m.association = {7, 0x0B, 0, 1, 0xFF, 0, 0};
    refused(m, MIMUSB_ERR_PARAM, "association");
    m = base;
    m.interfaces[0].endpoints[0].max_packet_size = 100;
    refused(m, MIMUSB_ERR_OVERFLOW, "max_packet_size");
    m = base;
    m.interfaces[0].endpoints[0].max_packet_size = 24;
    refused(m, MIMUSB_ERR_PARAM, "max_packet_size");
    m = base;
    m.interfaces[0].endpoints[0].transfer_type = 0;
    refused(m, MIMUSB_ERR_UNSET_FIELD, "transfer_type");
    m.interfaces[0].endpoints[0].transfer_type = 1;
    refused(m, MIMUSB_ERR_UNSUPPORTED, "transfer_type");
    m.interfaces[0].endpoints[0].transfer_type = 4;
    refused(m, MIMUSB_ERR_PARAM, "transfer_type");
    m = base;
    m.interfaces[0].endpoints[0].policy = {};
    refused(m, MIMUSB_ERR_UNSET_FIELD, "policy");
    m = base;
    m.interfaces[0].endpoints[0].policy = {MIMUSB_FULL_DROP_OLDEST, 200};
    refused(m, MIMUSB_ERR_OVERFLOW, "policy");
    m = base;
    m.interfaces[0].class_descriptors = {1, 0x24};
    refused(m, MIMUSB_ERR_PARAM, "class_descriptors");
    m = base;
    m.interfaces[0].endpoints.push_back(general_ep(0x03, 3, 8, 0));
    refused(m, MIMUSB_ERR_UNSET_FIELD, "interval");
    m = base;
    m.interfaces[0].endpoints.push_back(general_ep(0x03, 2, 64));
    m.interfaces[0].endpoints.back().policy = {MIMUSB_FULL_WAIT, 1};
    refused(m, MIMUSB_ERR_PARAM, "policy");
    // Queue slots: depth + 1 for DROP_NEWEST and WAIT, 2 * depth + 1 for DROP_OLDEST.
    MIMUSB_CHECK(mimusb::usb::queue_slots(5, MIMUSB_FULL_WAIT) == 6U &&
                 mimusb::usb::queue_slots(5, MIMUSB_FULL_DROP_NEWEST) == 6U &&
                 mimusb::usb::queue_slots(5, MIMUSB_FULL_DROP_OLDEST) == 11U);
    // High speed: bulk packets are 512 bytes (USB 2.0 5.8.3); an interrupt bInterval is 1 to 16.
    ComposerInput hs = general_input({base});
    hs.limits.high_speed = true;
    hs.limits.max_endpoint_bytes = 1024;
    Composed out;
    ComposerError err;
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(hs, &out, &err) == MIMUSB_ERR_PARAM);
    hs.groups[0].interfaces[0].endpoints[0].max_packet_size = 512;
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(hs, &out, &err) == MIMUSB_OK);
    hs.groups[0].interfaces[0].endpoints.push_back(general_ep(0x02, 3, 64, 17));
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(hs, &out, &err) == MIMUSB_ERR_PARAM);
    // The totals: the configuration descriptor and the strings against the board's limits.
    ComposerInput small = general_input({base});
    small.limits.max_config_bytes = 20;
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(small, &out, &err) == MIMUSB_ERR_OVERFLOW);
    small = general_input({base});
    small.limits.max_strings = 1;
    small.manufacturer = ascii("a");
    small.product = ascii("b");
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(small, &out, &err) == MIMUSB_ERR_OVERFLOW);
    // The caller's strings keep their indices; a name takes the lowest index nothing else has.
    ComposerInput named = general_input({base});
    named.groups[0].interfaces[0].name = ascii("n");
    named.user_strings[5] = ascii("u5");
    named.user_strings[7] = ascii("u7");
    MIMUSB_CHECK(mimusb::usb::compose_descriptors(named, &out, &err) == MIMUSB_OK);
    const std::map<int, std::string> strs = strings_of(out);
    MIMUSB_CHECK(strs.count(5) && strs.count(6) && strs.count(7) && out.configuration[9 + 8] == 6);
}

} // namespace

// The control rules of a profile interface (HID 1.11 7.1.1, 7.2): how many, which action each has
// and the values that tell them apart. The board's table is made from exactly these.
void test_hid_rules() {
    using mimusb::usb::BoardRule;
    using Role = BoardRule::Role;
    // One Node without Report IDs: 2 descriptors, GET_REPORT Input from the last report (no
    // first-byte filter, any Report ID), SET_REPORT, SET_IDLE and SET_PROTOCOL taken, and the two
    // GET requests of one byte.
    std::vector<HidNodeInput> one;
    one.push_back(make_node(0, 1, 1, 0x0111, 0, 0x81, 8, 10, 0, 0, 0, &kRdA, ""));
    std::vector<BoardRule> rules = mimusb::usb::compose_hid_rules(one);
    MIMUSB_CHECK(rules.size() == 8U);
    if (rules.size() == 8U) {
        MIMUSB_CHECK(rules[0].rule.action == MIMUSB_CONTROL_ANSWER &&
                     rules[0].rule.request_value == 6U && rules[0].rule.value_mask == 0xFF00U &&
                     rules[0].rule.value_value == 0x2200U && rules[0].data == kRdA &&
                     rules[0].capacity == kRdA.size());
        MIMUSB_CHECK(rules[1].rule.action == MIMUSB_CONTROL_ANSWER &&
                     rules[1].rule.value_value == 0x2100U && rules[1].data.size() == 9U &&
                     rules[1].data[0] == 9U && rules[1].data[6] == 0x22U &&
                     rules[1].data[7] == kRdA.size());
        MIMUSB_CHECK(rules[2].rule.action == MIMUSB_CONTROL_ANSWER_LAST_SENT &&
                     rules[2].rule.request_value == 1U && rules[2].rule.value_value == 0x0100U &&
                     rules[2].rule.value_mask == 0xFF00U &&
                     rules[2].rule.endpoint_address == 0x81U &&
                     rules[2].rule.first_byte_filter == 0U);
        MIMUSB_CHECK(rules[3].role == Role::hid_set_report &&
                     rules[3].rule.action == MIMUSB_CONTROL_ACCEPT &&
                     rules[3].rule.request_value == 9U);
        MIMUSB_CHECK(rules[4].role == Role::hid_set_idle &&
                     rules[4].rule.action == MIMUSB_CONTROL_ACCEPT &&
                     rules[4].rule.request_value == 10U);
        MIMUSB_CHECK(rules[5].role == Role::hid_set_protocol &&
                     rules[5].rule.action == MIMUSB_CONTROL_ACCEPT &&
                     rules[5].rule.request_value == 11U);
        MIMUSB_CHECK(rules[6].role == Role::hid_get_idle &&
                     rules[6].data == std::vector<std::uint8_t>{0U});
        MIMUSB_CHECK(rules[7].role == Role::hid_get_protocol &&
                     rules[7].data == std::vector<std::uint8_t>{1U}); // the report protocol (7.2.6)
        for (const BoardRule& r : rules) // every rule is of its interface
            MIMUSB_CHECK(r.rule.index_mask == 0xFFFFU && r.rule.index_value == 0U && r.iface == 0U);
    }

    // Two Nodes with Report IDs on one interface and a Feature report of the second: GET_REPORT of
    // an Input report gets a rule for each ID with the filter on the first byte, and the Feature
    // answer starts with its Report ID.
    std::vector<HidNodeInput> two;
    two.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdB, "", true, 1));
    two.push_back(make_node(0, 0, 0, 0x0111, 0, 0x81, 16, 5, 0, 0, 0, &kRdC, "", true, 2));
    two[1].features.push_back({2U, {0xAAU, 0xBBU}});
    rules = mimusb::usb::compose_hid_rules(two);
    MIMUSB_CHECK(rules.size() == 10U); // 2 + 2 + 1 + 5
    std::size_t last_sent = 0U;
    for (const BoardRule& r : rules) {
        if (r.rule.action == MIMUSB_CONTROL_ANSWER_LAST_SENT) {
            ++last_sent;
            MIMUSB_CHECK(r.rule.first_byte_filter == 1U && r.rule.value_mask == 0xFFFFU &&
                         r.rule.value_value == (0x0100U | r.rule.first_byte) &&
                         (r.rule.first_byte == 1U || r.rule.first_byte == 2U));
        }
        if (r.rule.action == MIMUSB_CONTROL_ANSWER && r.rule.request_value == 1U) {
            MIMUSB_CHECK(r.rule.value_value == 0x0302U &&
                         r.data == (std::vector<std::uint8_t>{2U, 0xAAU, 0xBBU}));
        }
    }
    MIMUSB_CHECK(last_sent == 2U);
    // The report descriptor answer is the two joined.
    MIMUSB_CHECK(rules[0].data.size() == kRdB.size() + kRdC.size());
}

void test_descriptor_composer() {
    test_hid_rules();
    test_minimal_keyboard();
    test_strings_and_two_out_endpoints();
    test_shared_interface();
    test_unicode_and_serial_from_chip_id();
    test_four_interfaces_in_number_order();
    test_gap_in_interface_numbers();
    test_interface_limit();
    test_shared_interface_rules();
    test_endpoint_address_used_twice();
    test_general_vendor_and_association();
}
