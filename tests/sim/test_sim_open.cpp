// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Opening a device and a node: the USB identity and the descriptors the target reads, strings
 * left out, the chip serial number, a board left attached, filled-in values and the *_init
 * functions, and nodes sharing an interface. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sim {

void test_open_identity_and_descriptors() {
    mimusb_device_config o = base_options();
    o.usb_device.manufacturer = "\xE6\x97\xA5\xE6\x9C\xAC Maker"; // 日本 Maker
    o.usb_device.product = "Gadget \xF0\x9F\x98\x80";             // U+1F600, a surrogate pair
    o.usb_device.serial = "SN-42";
    o.usb_config.string = "Cfg";
    o.usb_device.bcd_device = 0x0234;
    o.usb_device.language_id = 0x0407;
    o.usb_config.bm_attributes = 0x80 | 0x40 | 0x20;
    o.usb_config.bmax_power = 125;
    o.usb_device.device_class = 0xEF;
    o.usb_device.device_subclass = 2;
    o.usb_device.device_protocol = 1;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_is_ready(device) == 0);

    mimusb_device_info board{};
    MIMUSB_CHECK(mimusb_device_board_info(device, &board) == MIMUSB_OK);
    // What the board said about itself when the Device was opened.
    MIMUSB_CHECK(board.firmware_compatible == 1);
    MIMUSB_CHECK(board.version_major == MIMUSB_VERSION_MAJOR &&
                 board.version_minor == MIMUSB_VERSION_MINOR &&
                 board.version_patch == MIMUSB_VERSION_PATCH);
    MIMUSB_CHECK(board.max_interfaces == 15); // of any class, whatever the endpoint numbers
    MIMUSB_CHECK(board.max_report_bytes == 64);
    MIMUSB_CHECK(board.max_endpoint_number == SIM_ENDPOINTS);
    MIMUSB_CHECK(board.chip == MIMUSB_CHIP_UNKNOWN && board.usb_high_speed == 0);
    MIMUSB_CHECK(board.has_unique_id == 1 && board.unique_id_length == 8 &&
                 board.unique_id[0] == 0xDE && board.unique_id[7] == 0x04);
    MIMUSB_CHECK(board.max_baud == 3000000U && board.uart_flags == MIMUSB_UART_FLAG_BREAK &&
                 board.baud_rate == 3000000U);

    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_spec* mouse = mouse_spec(2);
    test_hid_node_config no{};
    test_fill_node_config(&no, 0);
    no.name = "Keys";
    no.ep_in.interval = 4;
    no.ep_in.max_packet_size = 16;
    test_hid_node_config mo{};
    test_fill_node_config(&mo, 1);
    mimusb_node* nk = open_node(device, kbd, &no);
    mimusb_node* nm = open_node(device, mouse, &mo);
    MIMUSB_CHECK(nk != nullptr && nm != nullptr);
    MIMUSB_CHECK(mimusb_node_id(nk) == 1 && mimusb_node_id(nm) == 2);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_is_ready(device) == 1);

    const std::uint8_t* dd = fake_board_device_descriptor();
    MIMUSB_CHECK(dd[8] == 0x09 && dd[9] == 0x12 && dd[10] == 0x01 && dd[11] == 0x00);
    MIMUSB_CHECK(dd[12] == 0x34 && dd[13] == 0x02);
    MIMUSB_CHECK(dd[4] == 0xEF && dd[5] == 2 && dd[6] == 1);
    char text[64];
    MIMUSB_CHECK(fake_board_string(1, text, sizeof(text)) &&
                 std::strcmp(text, "\xE6\x97\xA5\xE6\x9C\xAC Maker") == 0);
    MIMUSB_CHECK(fake_board_string(2, text, sizeof(text)) &&
                 std::strcmp(text, "Gadget \xF0\x9F\x98\x80") == 0);
    MIMUSB_CHECK(fake_board_string(3, text, sizeof(text)) && std::strcmp(text, "SN-42") == 0);
    MIMUSB_CHECK(fake_board_string(4, text, sizeof(text)) && std::strcmp(text, "Cfg") == 0);
    MIMUSB_CHECK(fake_board_string(5, text, sizeof(text)) && std::strcmp(text, "Keys") == 0);

    uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMUSB_CHECK(c[4] == 2 && length == 9 + 2 * 25);
    MIMUSB_CHECK(c[7] == (0x80 | 0x40 | 0x20) && c[8] == 125);
    MIMUSB_CHECK(c[9 + 20] == 0x81 && c[9 + 22] == 16 && c[9 + 24] == 4); // keyboard interface
    MIMUSB_CHECK(c[9 + 25 + 20] == 0x82 && c[9 + 25 + 22] == 64 && c[9 + 25 + 24] == 1);

    // Each interface carries exactly its Spec's descriptor.
    const std::uint8_t* bytes = nullptr;
    size_t spec_length = 0;
    MIMUSB_CHECK(mimusb_spec_descriptor(kbd, &bytes, &spec_length) == MIMUSB_OK);
    uint16_t rd_length = 0;
    const std::uint8_t* rd = fake_board_report_descriptor(0, &rd_length);
    MIMUSB_CHECK(rd_length == spec_length && std::memcmp(rd, bytes, spec_length) == 0);
    MIMUSB_CHECK(mimusb_spec_descriptor(mouse, &bytes, &spec_length) == MIMUSB_OK);
    rd = fake_board_report_descriptor(1, &rd_length);
    MIMUSB_CHECK(rd_length == spec_length && std::memcmp(rd, bytes, spec_length) == 0);

    MIMUSB_CHECK(mimusb_node_close(nm, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 1);
    MIMUSB_CHECK(mimusb_node_close(nk, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 0);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
    mimusb_spec_release(mouse);
}

// Strings set to null are left out, the chip's serial number included.
void test_no_names() {
    mimusb_device_config o = base_options();
    o.usb_device.manufacturer = nullptr;
    o.usb_device.product = nullptr;
    o.usb_device.serial_from_chip_id = 0;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimusb_spec* raw = hid_spec_no_id();
    mimusb_node* node = open_node(device, raw);
    MIMUSB_CHECK(node != nullptr);
    const std::uint8_t* d = fake_board_device_descriptor();
    MIMUSB_CHECK(d[14] == 0 && d[15] == 0 && d[16] == 0); // iManufacturer, iProduct, iSerial
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(raw);
}

// A product ID of zero is presented as given (the vendor ID of zero is refused:
// test_init_fills_nothing).
void test_zero_identity() {
    mimusb_device_config o = base_options();
    o.usb_device.product_id = 0;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimusb_spec* raw = hid_spec_no_id();
    mimusb_node* node = open_node(device, raw);
    MIMUSB_CHECK(node != nullptr);
    const std::uint8_t* d = fake_board_device_descriptor();
    MIMUSB_CHECK(d[8] == 0x09 && d[9] == 0x12 && d[10] == 0 && d[11] == 0); // idVendor, idProduct
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(raw);
}

void test_chip_serial_and_boot() {
    mimusb_device_config o = base_options();
    o.usb_device.serial_from_chip_id = 1;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    // HID 1.11 6.2.1, 4.2, 4.3: bcdHID, boot keyboard, country 33 (the HID descriptor and the
    // Interface descriptor say what the Spec's function says).
    mimusb_hid_function function{};
    function.bcd_hid = 0x0110;
    function.subclass = 1;
    function.protocol = 1;
    function.country_code = 33;
    mimusb_spec* raw = nullptr;
    MIMUSB_CHECK(create_hid_spec_no_id(function, &raw) == MIMUSB_OK);
    test_hid_node_config no{};
    test_fill_node_config(&no, 0);
    no.ep_in.max_packet_size = 6;
    no.ep_out.address = 0x01;
    no.ep_out.max_packet_size = 5;
    no.ep_out.interval = 1;
    no.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    mimusb_node* node = open_node(device, raw, &no);
    char text[64];
    MIMUSB_CHECK(fake_board_string(3, text, sizeof(text)) &&
                 std::strcmp(text, "DEADBEEF01020304") == 0);
    uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMUSB_CHECK(c[9 + 5] == 3 && c[9 + 6] == 1 && c[9 + 7] == 1); // boot keyboard
    MIMUSB_CHECK(c[9 + 11] == 0x10 && c[9 + 12] == 0x01 && c[9 + 13] == 33);
    // Packet sizes are taken as given, not rounded to a power of two.
    MIMUSB_CHECK(c[9 + 20] == 0x81 && c[9 + 22] == 6 && c[9 + 23] == 0);
    MIMUSB_CHECK(c[9 + 25 + 2] == 0x01 && c[9 + 25 + 4] == 5 && c[9 + 25 + 5] == 0);
    mimusb_node* bad = nullptr;
    test_hid_node_config broken = no;
    // The class of a general Node is the caller's, and never 0.
    {
        mimusb_usb_endpoint ep{};
        ep.address = 0x82;
        ep.transfer_type = MIMUSB_TRANSFER_BULK;
        ep.max_packet_size = 64;
        ep.policy = {MIMUSB_FULL_DROP_NEWEST, 2};
        const mimusb_usb_item item = mimusb_usb_item_make_endpoint(ep);
        const mimusb_usb_setting setting = {&item, 1};
        mimusb_usb_interface itf{};
        itf.number = 1; // beside the Node above
        itf.class_code = 0;
        itf.settings = &setting;
        itf.setting_count = 1;
        mimusb_node_config general;
        mimusb_node_config_init(&general);
        general.interfaces = &itf;
        general.interface_count = 1;
        MIMUSB_CHECK(test_node_open_interfaces(device, &general, &kCommit, &bad) ==
                         MIMUSB_ERR_UNSET_FIELD &&
                     bad == nullptr);
        // Class 0x03 is the caller's too: a hand-written HID interface
        // (examples/c/hid_interface.c).
        itf.class_code = 0x03;
        MIMUSB_CHECK(test_node_open_interfaces(device, &general, &kCommit, &bad) == MIMUSB_OK &&
                     bad != nullptr);
        if (bad != nullptr)
            MIMUSB_CHECK(mimusb_node_close(bad, &kCommit, kDrainMs) == MIMUSB_OK);
        bad = nullptr;
    }
    // USB 2.0 9.6.6: an endpoint address is a number 1 to 15 with bits 6..4 clear; IN has bit 7
    // set.
    for (const std::uint8_t bad_in : {0x80, 0x90, 0xF1, 0x01, 0x41}) {
        broken = no;
        broken.ep_in.address = bad_in;
        MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) == MIMUSB_ERR_PARAM);
    }
    for (const std::uint8_t bad_out : {0x80, 0x81, 0x10, 0x40}) {
        broken = no;
        broken.ep_out.address = bad_out;
        broken.ep_out.max_packet_size = 8;
        broken.ep_out.interval = 1;
        broken.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) == MIMUSB_ERR_PARAM);
    }
    // A number the board's controller does not have is refused before the board is asked: the
    // target's SET_CONFIGURATION would fail. The firmware core of this build has the numbers 1 to
    // SIM_ENDPOINTS (the board's PONG says so).
    if (SIM_ENDPOINTS < 15) {
        const std::uint8_t above = static_cast<std::uint8_t>(SIM_ENDPOINTS + 1);
        broken = no;
        broken.ep_in.address = static_cast<std::uint8_t>(0x80U | above);
        MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) ==
                         MIMUSB_ERR_OVERFLOW &&
                     bad == nullptr);
        broken = no;
        broken.ep_out.address = above;
        broken.ep_out.max_packet_size = 8;
        broken.ep_out.interval = 1;
        broken.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) ==
                         MIMUSB_ERR_OVERFLOW &&
                     bad == nullptr);
    }
    // A HID Node's endpoints are interrupt endpoints, and its IN policy is one of the three.
    broken = no;
    broken.ep_in.transfer_type = MIMUSB_TRANSFER_BULK;
    MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) ==
                     MIMUSB_ERR_UNSUPPORTED &&
                 bad == nullptr);
    broken = no;
    broken.interface_number = 1; // beside the Node above
    broken.ep_in.address = 0x82;
    broken.ep_out.address = 0x02;
    broken.ep_in.policy.on_full = MIMUSB_FULL_WAIT; // all three policies are allowed now
    MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) == MIMUSB_OK);
    if (bad != nullptr)
        MIMUSB_CHECK(mimusb_node_close(bad, &kCommit, kDrainMs) == MIMUSB_OK);
    bad = nullptr;
    // HID 1.11 4.2, 4.3 and 6.2.1: subclass 0 or 1, protocol 0, 1 or 2, country code 0 to 35, and
    // a bcdHID; the rest is reserved. The Spec refuses them, before any Device is asked.
    {
        mimusb_spec* refused = nullptr;
        mimusb_hid_function f = function;
        f.subclass = 2;
        MIMUSB_CHECK(create_hid_spec_no_id(f, &refused) == MIMUSB_ERR_PARAM && refused == nullptr);
        f = function;
        f.protocol = 3;
        MIMUSB_CHECK(create_hid_spec_no_id(f, &refused) == MIMUSB_ERR_PARAM && refused == nullptr);
        f = function;
        f.country_code = 36;
        MIMUSB_CHECK(create_hid_spec_no_id(f, &refused) == MIMUSB_ERR_PARAM && refused == nullptr);
        f = function;
        f.reserved8[1] = 1;
        MIMUSB_CHECK(create_hid_spec_no_id(f, &refused) == MIMUSB_ERR_PARAM && refused == nullptr);
        f = function;
        f.bcd_hid = 0;
        MIMUSB_CHECK(create_hid_spec_no_id(f, &refused) == MIMUSB_ERR_UNSET_FIELD &&
                     refused == nullptr);
    }
    broken = no;
    // The board's endpoints hold 64 bytes.
    broken.ep_in.max_packet_size = 65;
    MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) == MIMUSB_ERR_OVERFLOW);
    broken.ep_in.max_packet_size = 64;
    broken.ep_out.max_packet_size = 65;
    MIMUSB_CHECK(test_node_open_hid(device, raw, &broken, &kCommit, &bad) == MIMUSB_ERR_OVERFLOW);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(raw);
}

void test_keep_attached_and_discovery() {
    mimusb_device_config o = base_options();
    o.transport.keep_attached = 1;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimusb_spec* raw = hid_spec_no_id();
    mimusb_node* node = open_node(device, raw);
    MIMUSB_CHECK(node != nullptr);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 1); // the USB device stays attached
    mimusb_spec_release(raw);

    // A closed device can be opened again; the board answers pings with its limits.
    mimusb_device* again = open_device(base_options());
    if (again != nullptr) {
        mimusb_device_info info{};
        MIMUSB_CHECK(mimusb_device_board_info(again, &info) == MIMUSB_OK &&
                     info.max_interfaces == 15);
        // The board is still on the target's bus, as a program that ended without closing its
        // Device leaves it. The first configuration of the new Device takes it off the bus first,
        // so the target enumerates the new device and the board reports it mounted.
        MIMUSB_CHECK(fake_board_mounted() == 1);
        mimusb_spec* next = hid_spec_no_id();
        mimusb_node* opened = open_node(again, next);
        MIMUSB_CHECK(opened != nullptr);
        MIMUSB_CHECK(mimusb_device_wait_ready(again, 2000) == MIMUSB_OK);
        mimusb_spec_release(next);
        MIMUSB_CHECK(mimusb_device_close(again, kControlMs, kDrainMs) == MIMUSB_OK);
    }
}

// Every profile filled in with the values the tests use opens on a board configured the same way,
// sends a report and closes.
void test_filled_values() {
    mimusb_device_config o{};
    test_fill_device_config(&o);
    o.usb_device.vendor_id = 0x1209;
    o.usb_device.product_id = 0x0001;
    o.transport.baud_rate = 3000000;
    mimusb_device* device = open_device(o);
    if (device == nullptr)
        return;
    static const std::uint16_t usages[1] = {0xCD};
    static const mimusb_usage_semantic semantics[1] = {MIMUSB_USAGE_ONE_SHOT};
    static const char* const types[1] = {"EV_KEY"};
    static const char* const codes[1] = {"KEY_PLAYPAUSE"};
    mimusb_keyboard_options keyboard{};
    test_fill_keyboard(&keyboard);
    mimusb_mouse_options mouse{};
    test_fill_mouse(&mouse);
    mimusb_toggle_options toggle{};
    test_fill_toggle(&toggle);
    toggle.allowed_usages = usages;
    toggle.allowed_usage_count = 1;
    toggle.usage_semantics = semantics;
    toggle.expected_linux_event_types = types;
    toggle.expected_linux_codes = codes;
    mimusb_gamepad_options gamepad{};
    test_fill_gamepad(&gamepad);
    mimusb_touchscreen_options touchscreen{};
    test_fill_touchscreen(&touchscreen);
    mimusb_touchpad_options touchpad{};
    test_fill_touchpad(&touchpad);
    mimusb_pen_options pen{};
    test_fill_pen(&pen);
    mimusb_digitizer_options digitizer{};
    test_fill_digitizer(&digitizer);
    mimusb_spec* specs[8] = {};
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&keyboard, &specs[0]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_mouse(&mouse, &specs[1]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_toggle(&toggle, &specs[2]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_gamepad(&gamepad, &specs[3]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&touchscreen, &specs[4]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_touchpad(&touchpad, &specs[5]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_pen(&pen, &specs[6]) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_create_digitizer(&digitizer, &specs[7]) == MIMUSB_OK);

    mimusb_commit_timeouts timeouts{};
    test_fill_commit_timeouts(&timeouts);
    test_hid_node_config config{};
    test_fill_node_config(&config, 0);
    for (mimusb_spec* spec : specs) {
        mimusb_node* node = nullptr;
        MIMUSB_CHECK(test_node_open_hid(device, spec, &config, &timeouts, &node) == MIMUSB_OK);
        mimusb_spec_release(spec);
        if (node == nullptr)
            continue;
        MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
        // The strings this Device was opened with reach the board with its configuration.
        char text[64];
        MIMUSB_CHECK(fake_board_string(1, text, sizeof(text)) > 0 &&
                     std::strcmp(text, "libmimusb") == 0);
        fake_board_clear_reports();
        MIMUSB_CHECK(mimusb_node_close(node, &timeouts, kDrainMs) == MIMUSB_OK);
    }
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The *_init functions fill nothing in: an initialized configuration is refused, and so is one
// without a vendor ID or with zero commit timeouts; a product ID of zero is a valid one.
void test_init_fills_nothing() {
    mimusb_device_config o{};
    mimusb_device_config_init(&o);
    MIMUSB_CHECK(o.struct_size == sizeof(o) && o.usb_device.vendor_id == 0 &&
                 o.usb_device.product_id == 0 && o.usb_device.bcd_usb == 0 &&
                 o.usb_device.language_id == 0 && o.usb_device.manufacturer == nullptr &&
                 o.transport.baud_rate == 0 && o.transport.handshake_timeout_ms == 0 &&
                 o.transport.maximum_report_bytes == 0 && o.target.linux_hid.usages == 0 &&
                 o.usb_config.bm_attributes == 0 && o.usb_config.bmax_power == 0);
    mimusb_device* device = nullptr;
    MIMUSB_CHECK(mimusb_device_open(g_slave.c_str(), &o, &device) == MIMUSB_ERR_UNSET_FIELD &&
                 device == nullptr);

    test_hid_node_config nc{};
    test_hid_node_config_init(&nc);
    mimusb_node_placement placement;
    mimusb_node_placement_init(&placement);
    MIMUSB_CHECK(placement.struct_size == sizeof(placement) && placement.first_interface == 0 &&
                 placement.name == nullptr && placement.endpoints == nullptr &&
                 placement.endpoint_count == 0 && nc.interface_number == 0 &&
                 nc.ep_in.address == 0 && nc.ep_in.interval == 0 && nc.ep_in.max_packet_size == 0);
    mimusb_commit_timeouts zero{};
    mimusb_commit_timeouts_init(&zero);
    MIMUSB_CHECK(zero.struct_size == sizeof(zero) && zero.control_timeout_ms == 0 &&
                 zero.reconnect_timeout_ms == 0);

    // The vendor ID: zero stands for one nobody set.
    o = base_options();
    o.usb_device.vendor_id = 0;
    MIMUSB_CHECK(mimusb_device_open(g_slave.c_str(), &o, &device) == MIMUSB_ERR_UNSET_FIELD &&
                 device == nullptr);
    // The product ID: any value is valid, zero included.
    o = base_options();
    o.usb_device.product_id = 0;
    device = open_device(o);
    if (device == nullptr)
        return;
    // A node cannot be opened with the timeouts of an initialized structure, nor with an
    // initialized (all zero) node configuration.
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_node* node = nullptr;
    test_hid_node_config good{};
    test_fill_node_config(&good, 0);
    MIMUSB_CHECK(test_node_open_hid(device, kbd, &good, &zero, &node) == MIMUSB_ERR_UNSET_FIELD &&
                 node == nullptr);
    MIMUSB_CHECK(test_node_open_hid(device, kbd, &nc, &kCommit, &node) == MIMUSB_ERR_UNSET_FIELD &&
                 node == nullptr);
    mimusb_spec_release(kbd);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// Nodes on one interface must agree on its settings, and two interfaces need their own endpoints.
void test_shared_interfaces() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_spec* mouse = mouse_spec(2);
    test_hid_node_config first{};
    test_fill_node_config(&first, 0);
    mimusb_node* nk = open_node(device, kbd, &first);

    test_hid_node_config slower = first;
    slower.ep_in.interval = 8;
    mimusb_node* nm = nullptr;
    MIMUSB_CHECK(test_node_open_hid(device, mouse, &slower, &kCommit, &nm) == MIMUSB_ERR_PARAM &&
                 nm == nullptr);
    test_hid_node_config same_endpoint{};
    test_fill_node_config(&same_endpoint, 1);
    same_endpoint.ep_in.address = 0x81;
    MIMUSB_CHECK(test_node_open_hid(device, mouse, &same_endpoint, &kCommit, &nm) ==
                     MIMUSB_ERR_PARAM &&
                 nm == nullptr);
    // The refused opens left the keyboard working, and a matching Node shares its interface.
    MIMUSB_CHECK(test_node_open_hid(device, mouse, &first, &kCommit, &nm) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 1);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_keyboard_key(nk, 0x04, 1) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_submit(nk, kSendMs) == MIMUSB_OK);
    fake_report_t r{};
    MIMUSB_CHECK(next_report(&r) && r.data[0] == 1);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
    mimusb_spec_release(mouse);
}

// The roles a Spec lists, what a placement must give for them, and what a Spec of interfaces takes.
void test_placement_and_roles() {
    mimusb_spec* kbd = keyboard_spec(1);
    mimusb_role_info roles[4]{};
    std::size_t count = 0;
    MIMUSB_CHECK(mimusb_spec_roles(kbd, roles, 4, &count) == MIMUSB_OK && count == 2 &&
                 roles[0].role == MIMUSB_ROLE_HID_IN && roles[0].required == 1 &&
                 roles[1].role == MIMUSB_ROLE_HID_OUT && roles[1].required == 0);
    // A capacity below the count writes that many and still reports the count.
    mimusb_role_info one{};
    MIMUSB_CHECK(mimusb_spec_roles(kbd, &one, 1, &count) == MIMUSB_OK && count == 2 &&
                 one.role == MIMUSB_ROLE_HID_IN);
    MIMUSB_CHECK(mimusb_spec_roles(nullptr, roles, 4, &count) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_spec_roles(kbd, roles, 4, nullptr) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_spec_roles(kbd, nullptr, 1, &count) == MIMUSB_ERR_PARAM);

    mimusb_device* device = open_device(base_options());
    if (device == nullptr) {
        mimusb_spec_release(kbd);
        return;
    }
    test_hid_node_config good{};
    test_fill_node_config(&good, 0);
    mimusb_placed_endpoint in{};
    in.role = MIMUSB_ROLE_HID_IN;
    in.endpoint = good.ep_in;
    mimusb_node_placement placement;
    mimusb_node_placement_init(&placement);
    placement.endpoints = &in;
    placement.endpoint_count = 1;
    mimusb_node* node = nullptr;

    // The IN role is required.
    mimusb_node_placement none = placement;
    none.endpoints = nullptr;
    none.endpoint_count = 0;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &none, &kCommit, &node) == MIMUSB_ERR_UNSET_FIELD &&
                 node == nullptr);
    // A role is given once, and only a role the Spec has.
    mimusb_placed_endpoint twice[2] = {in, in};
    mimusb_node_placement p = placement;
    p.endpoints = twice;
    p.endpoint_count = 2;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &p, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);
    mimusb_placed_endpoint unknown = in;
    unknown.role = static_cast<mimusb_endpoint_role>(9);
    p = placement;
    p.endpoints = &unknown;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &p, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);
    // Reserved fields are zero; a count needs endpoints and the other way round.
    mimusb_placed_endpoint reserved = in;
    reserved.reserved = 1;
    p = placement;
    p.endpoints = &reserved;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &p, &kCommit, &node) == MIMUSB_ERR_PARAM);
    p = placement;
    p.reserved8[0] = 1;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &p, &kCommit, &node) == MIMUSB_ERR_PARAM);
    p = placement;
    p.endpoint_count = 0;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &p, &kCommit, &node) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_open(device, kbd, nullptr, &kCommit, &node) == MIMUSB_ERR_PARAM);

    // A Spec of interfaces takes an empty placement only, and has no roles.
    mimusb_usb_endpoint ep{};
    ep.address = 0x82;
    ep.transfer_type = MIMUSB_TRANSFER_BULK;
    ep.max_packet_size = 64;
    ep.policy = {MIMUSB_FULL_DROP_NEWEST, 2};
    const mimusb_usb_item item = mimusb_usb_item_make_endpoint(ep);
    const mimusb_usb_setting setting = {&item, 1};
    mimusb_usb_interface itf{};
    itf.number = 0;
    itf.class_code = 0xFF;
    itf.settings = &setting;
    itf.setting_count = 1;
    mimusb_node_config general;
    mimusb_node_config_init(&general);
    general.interfaces = &itf;
    general.interface_count = 1;
    mimusb_spec* ispec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_interfaces(&general, &ispec) == MIMUSB_OK && ispec != nullptr);
    MIMUSB_CHECK(mimusb_spec_roles(ispec, roles, 4, &count) == MIMUSB_OK && count == 0);
    MIMUSB_CHECK(mimusb_node_open(device, ispec, &placement, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);
    mimusb_node_placement named = placement;
    named.endpoints = nullptr;
    named.endpoint_count = 0;
    named.name = "x";
    MIMUSB_CHECK(mimusb_node_open(device, ispec, &named, &kCommit, &node) == MIMUSB_ERR_PARAM);
    named.name = nullptr;
    named.first_interface = 1;
    MIMUSB_CHECK(mimusb_node_open(device, ispec, &named, &kCommit, &node) == MIMUSB_ERR_PARAM);
    mimusb_node_placement empty;
    mimusb_node_placement_init(&empty);
    MIMUSB_CHECK(mimusb_node_open(device, ispec, &empty, &kCommit, &node) == MIMUSB_OK &&
                 node != nullptr);
    if (node != nullptr)
        MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(ispec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
}

// The rules of a placement, for any kind of Spec: tried before the Spec's own, found by slot, and
// counted in what a Spec costs.
void test_extra_rules() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_node* node = nullptr;
    fake_control_result_t res{};
    const std::uint8_t get_idle[8] = {0xA1, 0x02, 0, 0, 0, 0, 1, 0};

    // A HID Node answers GET_IDLE with the library's rule; a rule of the placement takes it over.
    mimusb_spec* kbd = keyboard_spec(0);
    test_hid_node_config good{};
    test_fill_node_config(&good, 0);
    mimusb_placed_endpoint in{};
    in.role = MIMUSB_ROLE_HID_IN;
    in.endpoint = good.ep_in;
    mimusb_node_placement placement;
    mimusb_node_placement_init(&placement);
    placement.endpoints = &in;
    placement.endpoint_count = 1;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &placement, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    fake_board_target_control(get_idle, nullptr, 0);
    MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 && res.outcome == FAKE_CONTROL_ANSWERED);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    node = nullptr;

    mimusb_control_rule stall{};
    stall.request_type_mask = 0xFF;
    stall.request_type_value = 0xA1;
    stall.request_mask = 0xFF;
    stall.request_value = 0x02;
    stall.index_mask = 0xFFFF;
    stall.index_value = 0;
    stall.action = MIMUSB_CONTROL_STALL;
    placement.extra_rules = &stall;
    placement.extra_rule_count = 1;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &placement, &kCommit, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    fake_board_target_control(get_idle, nullptr, 0);
    MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 && res.outcome == FAKE_CONTROL_STALLED);
    // A Spec names no rule it does not bring.
    std::size_t index = 99;
    MIMUSB_CHECK(mimusb_node_find_rule(node, MIMUSB_RULE_SLOT_CDC_LINE_CODING, &index) ==
                 MIMUSB_ERR_UNSUPPORTED);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    node = nullptr;
    // A pointer and a count go together.
    placement.extra_rules = nullptr;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &placement, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);
    placement.extra_rules = &stall;
    placement.extra_rule_count = 0;
    MIMUSB_CHECK(mimusb_node_open(device, kbd, &placement, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);

    // What a HID Spec costs in rules: more than its descriptors, with their bytes.
    std::size_t rules = 0;
    std::size_t bytes = 0;
    const std::uint8_t* descriptor = nullptr;
    std::size_t descriptor_length = 0;
    MIMUSB_CHECK(mimusb_spec_rule_cost(kbd, &rules, &bytes) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_spec_descriptor(kbd, &descriptor, &descriptor_length) == MIMUSB_OK);
    MIMUSB_CHECK(rules >= 8 && bytes >= descriptor_length + 11);
    mimusb_spec_release(kbd);

    // A CDC-ACM Node with a rule of its own: its line coding rule is found after the extra ones.
    mimusb_cdc_acm_options options;
    mimusb_cdc_acm_options_init(&options);
    options.bcd_cdc = 0x0120;
    options.line_coding[1] = 0xC2; // dwDTERate 49664
    options.line_coding[6] = 8;
    mimusb_spec* cdc = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&options, &cdc) == MIMUSB_OK && cdc != nullptr);
    auto endpoint = [](std::uint8_t address, std::uint8_t type) {
        mimusb_usb_endpoint ep{};
        ep.address = address;
        ep.transfer_type = type;
        ep.max_packet_size = type == MIMUSB_TRANSFER_BULK ? 64 : 16;
        ep.interval = type == MIMUSB_TRANSFER_INTERRUPT ? 4 : 0;
        if ((address & 0x80U) != 0U)
            ep.policy = {MIMUSB_FULL_DROP_NEWEST, 2};
        return ep;
    };
    mimusb_placed_endpoint placed[3] = {
        {MIMUSB_ROLE_CDC_NOTIFY, 0, endpoint(0x82, MIMUSB_TRANSFER_INTERRUPT)},
        {MIMUSB_ROLE_CDC_DATA_IN, 0, endpoint(0x83, MIMUSB_TRANSFER_BULK)},
        {MIMUSB_ROLE_CDC_DATA_OUT, 0, endpoint(0x03, MIMUSB_TRANSFER_BULK)}};
    mimusb_node_placement cp;
    mimusb_node_placement_init(&cp);
    cp.endpoints = placed;
    cp.endpoint_count = 3;
    cp.extra_rules = &stall;
    cp.extra_rule_count = 1;
    // Each role has its type and direction, and all three are required.
    mimusb_placed_endpoint wrong = placed[1];
    wrong.endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    mimusb_placed_endpoint wrong_set[3] = {placed[0], wrong, placed[2]};
    mimusb_node_placement bad = cp;
    bad.endpoints = wrong_set;
    MIMUSB_CHECK(mimusb_node_open(device, cdc, &bad, &kCommit, &node) == MIMUSB_ERR_UNSUPPORTED &&
                 node == nullptr);
    bad = cp;
    bad.endpoint_count = 2;
    MIMUSB_CHECK(mimusb_node_open(device, cdc, &bad, &kCommit, &node) == MIMUSB_ERR_UNSET_FIELD &&
                 node == nullptr);
    mimusb_placed_endpoint hid_role[3] = {placed[0], placed[1], placed[2]};
    hid_role[2].role = MIMUSB_ROLE_HID_IN;
    bad.endpoints = hid_role;
    bad.endpoint_count = 3;
    MIMUSB_CHECK(mimusb_node_open(device, cdc, &bad, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);

    MIMUSB_CHECK(mimusb_node_open(device, cdc, &cp, &kCommit, &node) == MIMUSB_OK &&
                 node != nullptr);
    MIMUSB_CHECK(mimusb_node_find_rule(node, MIMUSB_RULE_SLOT_CDC_LINE_CODING, &index) ==
                     MIMUSB_OK &&
                 index == 2); // after the one extra rule
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    const std::uint8_t get_coding[8] = {0xA1, 0x21, 0, 0, 0, 0, 7, 0};
    fake_board_target_control(get_coding, nullptr, 0);
    MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 && res.outcome == FAKE_CONTROL_ANSWERED &&
                 res.length == 7 && std::memcmp(res.data, options.line_coding, 7) == 0);
    const std::uint8_t next[7] = {0x00, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x08};
    MIMUSB_CHECK(mimusb_node_set_rule_data(node, index, next, sizeof(next), kControlMs) ==
                 MIMUSB_OK);
    fake_board_target_control(get_coding, nullptr, 0);
    MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 && res.outcome == FAKE_CONTROL_ANSWERED &&
                 res.length == 7 && std::memcmp(res.data, next, 7) == 0);
    // The extra rule is number 0 and is not an ANSWER rule.
    MIMUSB_CHECK(mimusb_node_set_rule_data(node, 0, next, sizeof(next), kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);

    // The options an unset or bad value is refused in.
    mimusb_spec* refused = nullptr;
    mimusb_cdc_acm_options o = options;
    o.bcd_cdc = 0;
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&o, &refused) == MIMUSB_ERR_UNSET_FIELD &&
                 refused == nullptr);
    o = options;
    std::memset(o.line_coding, 0, 4);
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&o, &refused) == MIMUSB_ERR_UNSET_FIELD);
    o = options;
    o.reserved8b = 1;
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&o, &refused) == MIMUSB_ERR_PARAM);
    o = options;
    o.struct_size = 1;
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&o, &refused) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(nullptr, &refused) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_spec_create_cdc_acm(&options, nullptr) == MIMUSB_ERR_PARAM);
    std::size_t interfaces = 0;
    MIMUSB_CHECK(mimusb_spec_interface_count(cdc, &interfaces) == MIMUSB_OK && interfaces == 2);
    mimusb_role_info roles[4]{};
    std::size_t count = 0;
    MIMUSB_CHECK(mimusb_spec_roles(cdc, roles, 4, &count) == MIMUSB_OK && count == 3 &&
                 roles[0].role == MIMUSB_ROLE_CDC_NOTIFY &&
                 roles[2].role == MIMUSB_ROLE_CDC_DATA_OUT && roles[2].required == 1);
    mimusb_spec_release(cdc);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace sim
