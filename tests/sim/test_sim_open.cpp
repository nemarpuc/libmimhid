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
    mimusb_hid_node_config no{};
    test_fill_node_config(&no, 0);
    no.name = "Keys";
    no.ep_in.interval = 4;
    no.ep_in.max_packet_size = 16;
    mimusb_hid_node_config mo{};
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
    mimusb_spec* raw = raw_spec_no_id();
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
    mimusb_spec* raw = raw_spec_no_id();
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
    mimusb_spec* raw = raw_spec_no_id();
    mimusb_hid_node_config no{};
    test_fill_node_config(&no, 0);
    no.subclass = 1;
    no.protocol = 1;
    no.ep_in.max_packet_size = 6;
    no.ep_out.address = 0x01;
    no.ep_out.max_packet_size = 5;
    no.ep_out.interval = 1;
    no.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    no.country_code = 33;
    no.bcd_hid = 0x0110;
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
    mimusb_hid_node_config broken = no;
    // The class of a general Node is the caller's, and never 0.
    {
        mimusb_usb_endpoint ep{};
        ep.address = 0x82;
        ep.transfer_type = MIMUSB_TRANSFER_BULK;
        ep.max_packet_size = 64;
        ep.policy = {MIMUSB_FULL_DROP_NEWEST, 2};
        mimusb_usb_interface itf{};
        itf.number = 1; // beside the Node above
        itf.class_code = 0;
        itf.endpoints = &ep;
        itf.endpoint_count = 1;
        mimusb_node_config general;
        mimusb_node_config_init(&general);
        general.interfaces = &itf;
        general.interface_count = 1;
        MIMUSB_CHECK(mimusb_node_open(device, &general, &kCommit, &bad) == MIMUSB_ERR_UNSET_FIELD &&
                     bad == nullptr);
        // Class 0x03 is the caller's too: a hand-written HID interface
        // (examples/c/hid_interface.c).
        itf.class_code = 0x03;
        MIMUSB_CHECK(mimusb_node_open(device, &general, &kCommit, &bad) == MIMUSB_OK &&
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
        MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                     MIMUSB_ERR_PARAM);
    }
    for (const std::uint8_t bad_out : {0x80, 0x81, 0x10, 0x40}) {
        broken = no;
        broken.ep_out.address = bad_out;
        broken.ep_out.max_packet_size = 8;
        broken.ep_out.interval = 1;
        broken.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                     MIMUSB_ERR_PARAM);
    }
    // A number the board's controller does not have is refused before the board is asked: the
    // target's SET_CONFIGURATION would fail. The firmware core of this build has the numbers 1 to
    // SIM_ENDPOINTS (the board's PONG says so).
    if (SIM_ENDPOINTS < 15) {
        const std::uint8_t above = static_cast<std::uint8_t>(SIM_ENDPOINTS + 1);
        broken = no;
        broken.ep_in.address = static_cast<std::uint8_t>(0x80U | above);
        MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                         MIMUSB_ERR_OVERFLOW &&
                     bad == nullptr);
        broken = no;
        broken.ep_out.address = above;
        broken.ep_out.max_packet_size = 8;
        broken.ep_out.interval = 1;
        broken.ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
        MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                         MIMUSB_ERR_OVERFLOW &&
                     bad == nullptr);
    }
    // A HID Node's endpoints are interrupt endpoints, and its IN policy is one of the three.
    broken = no;
    broken.ep_in.transfer_type = MIMUSB_TRANSFER_BULK;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                     MIMUSB_ERR_UNSUPPORTED &&
                 bad == nullptr);
    broken = no;
    broken.interface_number = 1; // beside the Node above
    broken.ep_in.address = 0x82;
    broken.ep_out.address = 0x02;
    broken.ep_in.policy.on_full = MIMUSB_FULL_WAIT; // all three policies are allowed now
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) == MIMUSB_OK);
    if (bad != nullptr)
        MIMUSB_CHECK(mimusb_node_close(bad, &kCommit, kDrainMs) == MIMUSB_OK);
    bad = nullptr;
    // HID 1.11 4.2 and 4.3: subclass 0 or 1, protocol 0, 1 or 2; the rest is reserved.
    broken = no;
    broken.subclass = 2;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                 MIMUSB_ERR_PARAM);
    broken = no;
    broken.subclass = 1;
    broken.protocol = 3;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                 MIMUSB_ERR_PARAM);
    broken = no;
    broken.reserved = 1;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                 MIMUSB_ERR_PARAM);
    broken = no;
    // The board's endpoints hold 64 bytes.
    broken.ep_in.max_packet_size = 65;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                 MIMUSB_ERR_OVERFLOW);
    broken.ep_in.max_packet_size = 64;
    broken.ep_out.max_packet_size = 65;
    MIMUSB_CHECK(mimusb_node_open_profile(device, raw, &broken, &kCommit, &bad) ==
                 MIMUSB_ERR_OVERFLOW);
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
    mimusb_spec* raw = raw_spec_no_id();
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
        mimusb_spec* next = raw_spec_no_id();
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
    mimusb_hid_node_config config{};
    test_fill_node_config(&config, 0);
    for (mimusb_spec* spec : specs) {
        mimusb_node* node = nullptr;
        MIMUSB_CHECK(mimusb_node_open_profile(device, spec, &config, &timeouts, &node) ==
                     MIMUSB_OK);
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
                 o.transport.maximum_report_bytes == 0 && o.transport.linux_hid.usages == 0 &&
                 o.usb_config.bm_attributes == 0 && o.usb_config.bmax_power == 0);
    mimusb_device* device = nullptr;
    MIMUSB_CHECK(mimusb_device_open(g_slave.c_str(), &o, &device) == MIMUSB_ERR_UNSET_FIELD &&
                 device == nullptr);

    mimusb_hid_node_config nc{};
    mimusb_hid_node_config_init(&nc);
    MIMUSB_CHECK(nc.struct_size == sizeof(nc) && nc.interface_number == 0 && nc.bcd_hid == 0 &&
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
    mimusb_hid_node_config good{};
    test_fill_node_config(&good, 0);
    MIMUSB_CHECK(mimusb_node_open_profile(device, kbd, &good, &zero, &node) ==
                     MIMUSB_ERR_UNSET_FIELD &&
                 node == nullptr);
    MIMUSB_CHECK(mimusb_node_open_profile(device, kbd, &nc, &kCommit, &node) ==
                     MIMUSB_ERR_UNSET_FIELD &&
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
    mimusb_hid_node_config first{};
    test_fill_node_config(&first, 0);
    mimusb_node* nk = open_node(device, kbd, &first);

    mimusb_hid_node_config slower = first;
    slower.ep_in.interval = 8;
    mimusb_node* nm = nullptr;
    MIMUSB_CHECK(mimusb_node_open_profile(device, mouse, &slower, &kCommit, &nm) ==
                     MIMUSB_ERR_PARAM &&
                 nm == nullptr);
    mimusb_hid_node_config same_endpoint{};
    test_fill_node_config(&same_endpoint, 1);
    same_endpoint.ep_in.address = 0x81;
    MIMUSB_CHECK(mimusb_node_open_profile(device, mouse, &same_endpoint, &kCommit, &nm) ==
                     MIMUSB_ERR_PARAM &&
                 nm == nullptr);
    // The refused opens left the keyboard working, and a matching Node shares its interface.
    MIMUSB_CHECK(mimusb_node_open_profile(device, mouse, &first, &kCommit, &nm) == MIMUSB_OK);
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

} // namespace sim
