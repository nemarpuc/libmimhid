// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* End to end: the host library talks through a pseudo-terminal to the real firmware core,
 * which presents what a USB target would see. */
#include "fake_board.h"
#include "mimhid.h"
#include "mimhid_proto.h"
#include "test.hpp"
#include "transport/link.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

int g_failures = 0;

namespace {

#ifndef E2E_HID_ITFS
#define E2E_HID_ITFS 7
#endif

int g_master = -1;
std::string g_slave;

bool open_pty() {
    g_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (g_master < 0 || grantpt(g_master) != 0 || unlockpt(g_master) != 0)
        return false;
    const char* name = ptsname(g_master);
    if (name == nullptr)
        return false;
    g_slave = name;
    termios tio{};
    tcgetattr(g_master, &tio);
    cfmakeraw(&tio);
    tcsetattr(g_master, TCSANOW, &tio);
    fcntl(g_master, F_SETFL, fcntl(g_master, F_GETFL) | O_NONBLOCK);
    return true;
}

// ---- events -------------------------------------------------------------------------
struct EventRecord {
    mimhid_event_kind kind;
    std::uint16_t node_id;
    std::uint16_t report_id;
    std::uint32_t value;
    std::vector<std::uint8_t> data;
    std::uint32_t token;
    std::uint32_t flags;
};
std::mutex g_event_mutex;
std::vector<EventRecord> g_events;

// A reply the sink gives by itself to every held transfer that it sees (control tests).
struct SinkReply {
    std::atomic<bool> enabled{false};
    mimhid_device* device{nullptr};
    mimhid_control_action action{MIMHID_CONTROL_ACTION_ANSWER};
    std::vector<std::uint8_t> data;
    std::atomic<int> result{-1};
} g_sink_reply;

void MIMHID_CALL on_event(void*, const mimhid_event* event) {
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.push_back({event->kind, event->node_id, event->report_id, event->value,
                            std::vector<std::uint8_t>(event->data, event->data + event->length),
                            event->control_token, event->control_flags});
    }
    if (event->kind == MIMHID_EVENT_CONTROL_SETUP && (event->control_flags & MIMHID_CONTROL_FLAG_HELD) &&
        g_sink_reply.enabled.load()) {
        g_sink_reply.result.store(static_cast<int>(mimhid_device_control_reply(
            g_sink_reply.device, event->control_token, g_sink_reply.action,
            g_sink_reply.data.empty() ? nullptr : g_sink_reply.data.data(), g_sink_reply.data.size())));
    }
}

bool wait_event(mimhid_event_kind kind, EventRecord* out, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(g_event_mutex);
            for (auto it = g_events.begin(); it != g_events.end(); ++it) {
                if (it->kind == kind) {
                    *out = *it;
                    g_events.erase(it);
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// ---- helpers ------------------------------------------------------------------------
mimhid_device_config base_options() {
    mimhid_device_config o{};
    mimhid_device_config_init(&o);
    o.transport.baud_rate = 3000000;
    o.transport.control_timeout_ms = 500;
    o.transport.send_timeout_ms = 500;
    o.transport.reconnect_timeout_ms = 3000;
    o.transport.close_drain_timeout_ms = 1000;
    o.usb_device.language_id = 0x0409;
    o.usb_device.bcd_usb = 0x0200;
    o.usb_device.ep0_max_packet_size = 64;
    o.usb_device.vendor_id = 0x046D;
    o.usb_device.product_id = 0xC52B;
    o.usb_device.bcd_device = 0x0100;
    o.usb_config.configuration_value = 1;
    o.usb_config.bm_attributes = 0x80;
    o.usb_config.bmax_power = 50;
    o.transport.linux_descriptor_policy_bytes = 4096;
    o.transport.linux_hid_fields_per_report_policy = 256;
    o.transport.linux_hid_global_stack_depth_policy = 4;
    o.transport.linux_hid_usages_policy = 1024;
    o.transport.linux_hid_report_data_policy_bits = 8192;
    o.transport.linux_hid_report_size_policy_bits = 32;
    o.transport.validate_reports = 1;
    o.transport.event_sink = on_event;
    return o;
}

mimhid_device* open_device(const mimhid_device_config& options) {
    mimhid_device_info info{};
    std::strncpy(info.port_name, g_slave.c_str(), sizeof(info.port_name) - 1);
    mimhid_device* device = nullptr;
    const mimhid_result r = mimhid_device_open(info.port_name, &options, &device);
    if (r != MIMHID_OK) {
        std::fprintf(stderr, "device_open: %s (%s)\n", mimhid_result_name(r),
                     mimhid_last_error()->reason);
        ++g_failures;
        return nullptr;
    }
    return device;
}

mimhid_spec* keyboard_spec(std::uint8_t report_id) {
    mimhid_keyboard_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.usage_minimum = 0x04;
    o.usage_maximum = 0x65;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_keyboard(&o, &spec) == MIMHID_OK);
    return spec;
}

mimhid_spec* mouse_spec(std::uint8_t report_id) {
    mimhid_mouse_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.button_count = 3;
    o.x = {-32767, 32767, 16, {}};
    o.y = {-32767, 32767, 16, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_mouse(&o, &spec) == MIMHID_OK);
    return spec;
}

mimhid_spec* touch_spec(std::uint8_t report_id, std::uint32_t feature) {
    mimhid_touchscreen_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.maximum_contacts = 3;
    o.contacts_per_report = 3;
    o.contact_id = {0, 15, 4, {}};
    o.x = {0, 32767, 16, {}};
    o.y = {0, 32767, 16, {}};
    o.contact_count = {0, 3, 2, {}};
    o.enable_contact_count_maximum_feature_declaration = feature;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_touchscreen(&o, &spec) == MIMHID_OK);
    return spec;
}

mimhid_spec* pen_spec(std::uint8_t report_id) {
    mimhid_pen_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.x = {0, 32767, 16, {}};
    o.y = {0, 32767, 16, {}};
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_pen(&o, &spec) == MIMHID_OK);
    return spec;
}

constexpr std::uint8_t kRawDescriptorNoId[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x15,
                                               0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
                                               0x09, 0x01, 0x81, 0x02, 0xC0};

mimhid_spec* raw_spec_no_id() {
    static const mimhid_raw_report report{{0, 0, {}}, 1};
    mimhid_raw_options o{};
    o.struct_size = sizeof(o);
    o.descriptor = kRawDescriptorNoId;
    o.descriptor_length = sizeof(kRawDescriptorNoId);
    o.reports = &report;
    o.report_count = 1;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_raw(&o, &spec) == MIMHID_OK);
    return spec;
}

// Raw spec with an Output and a Feature report, both under Report ID 5.
constexpr std::uint8_t kRawDescriptorIo[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x05, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x08, 0x09, 0x01, 0x81, 0x02, 0x09, 0x02, 0x91, 0x02, 0x09, 0x03, 0xB1, 0x02, 0xC0};

mimhid_spec* raw_spec_io() {
    static const mimhid_raw_report report{{1, 5, {}}, 2};
    mimhid_raw_options o{};
    o.struct_size = sizeof(o);
    o.descriptor = kRawDescriptorIo;
    o.descriptor_length = sizeof(kRawDescriptorIo);
    o.reports = &report;
    o.report_count = 1;
    o.requires_output = 1;
    o.requires_feature_response = 1;
    mimhid_spec* spec = nullptr;
    MIMHID_CHECK(mimhid_spec_create_raw(&o, &spec) == MIMHID_OK);
    return spec;
}

mimhid_node* open_node(mimhid_device* device, mimhid_spec* spec,
                       const mimhid_node_config* options = nullptr) {
    mimhid_node_config defaults{};
    mimhid_node_config_init(&defaults);
    defaults.iface.number = 0;
    defaults.iface.class_code = 3;
    defaults.iface.bcd_hid = 0x0111;
    defaults.ep_in.address = 0x81;
    defaults.ep_in.poll_interval_ms = 1;
    defaults.ep_in.max_packet_size = 64;
    mimhid_node* node = nullptr;
    const mimhid_result r =
        mimhid_node_open(device, spec, options != nullptr ? options : &defaults, &node);
    if (r != MIMHID_OK) {
        std::fprintf(stderr, "node_open: %s (%s)\n", mimhid_result_name(r),
                     mimhid_last_error()->reason);
        ++g_failures;
        return nullptr;
    }
    return node;
}

bool next_report(fake_report_t* r, int timeout_ms = 2000) {
    return fake_board_wait_report(r, static_cast<std::uint32_t>(timeout_ms)) != 0;
}

bool all_zero_after_id(const fake_report_t& r, bool has_id) {
    for (std::size_t i = has_id ? 1U : 0U; i < r.length; ++i) {
        if (r.data[i] != 0)
            return false;
    }
    return true;
}

// ---- scenarios ----------------------------------------------------------------------

void test_open_identity_and_descriptors() {
    mimhid_device_config o = base_options();
    o.usb_device.manufacturer = "Maker";
    o.usb_device.product = "Gadget";
    o.usb_device.serial = "SN-42";
    o.usb_config.string = "Cfg";
    o.usb_device.bcd_device = 0x0234;
    o.usb_device.language_id = 0x0407;
    o.usb_config.bm_attributes = 0x80 | 0x40 | 0x20;
    o.usb_config.bmax_power = 125;
    o.usb_device.device_class = 0xEF;
    o.usb_device.device_subclass = 2;
    o.usb_device.device_protocol = 1;
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_is_ready(device) == 0);

    mimhid_device_info board{};
    MIMHID_CHECK(mimhid_device_board_info(device, &board) == MIMHID_OK);
    // What the board said about itself when the Device was opened.
    MIMHID_CHECK(board.protocol_version == MIMHID_PROTO_VERSION);
    MIMHID_CHECK(board.version_major == MIMHID_VERSION_MAJOR &&
                 board.version_minor == MIMHID_VERSION_MINOR &&
                 board.version_patch == MIMHID_VERSION_PATCH);
    MIMHID_CHECK(board.max_interfaces == E2E_HID_ITFS && board.max_nodes == 8);
    MIMHID_CHECK(board.max_report_bytes == 64);
    MIMHID_CHECK(board.chip == MIMHID_CHIP_UNKNOWN && board.usb_high_speed == 0);
    MIMHID_CHECK(board.has_unique_id == 1 && board.unique_id_length == 8 &&
                 board.unique_id[0] == 0xDE && board.unique_id[7] == 0x04);
    MIMHID_CHECK(board.baud_mask == 0x00FF && board.baud_rate == 3000000U);

    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_spec* mouse = mouse_spec(2);
    mimhid_node_config no{};
    mimhid_node_config_init(&no);
    no.iface.number = 0;
    no.iface.class_code = 3;
    no.ep_in.address = 0x81;
    no.iface.name = "Keys";
    no.ep_in.poll_interval_ms = 4;
    no.ep_in.max_packet_size = 16;
    no.iface.bcd_hid = 0x0111;
    mimhid_node_config mo{};
    mimhid_node_config_init(&mo);
    mo.iface.number = 1;
    mo.iface.class_code = 3;
    mo.ep_in.address = 0x82;
    mo.ep_in.poll_interval_ms = 1;
    mo.ep_in.max_packet_size = 64;
    mo.iface.bcd_hid = 0x0111;
    mimhid_node* nk = open_node(device, kbd, &no);
    mimhid_node* nm = open_node(device, mouse, &mo);
    MIMHID_CHECK(nk != nullptr && nm != nullptr);
    MIMHID_CHECK(mimhid_node_id(nk) == 1 && mimhid_node_id(nm) == 2);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 1);

    const std::uint8_t* dd = fake_board_device_descriptor();
    MIMHID_CHECK(dd[8] == 0x6D && dd[9] == 0x04 && dd[10] == 0x2B && dd[11] == 0xC5);
    MIMHID_CHECK(dd[12] == 0x34 && dd[13] == 0x02);
    MIMHID_CHECK(dd[4] == 0xEF && dd[5] == 2 && dd[6] == 1);
    char text[64];
    MIMHID_CHECK(fake_board_string(1, text, sizeof(text)) && std::strcmp(text, "Maker") == 0);
    MIMHID_CHECK(fake_board_string(2, text, sizeof(text)) && std::strcmp(text, "Gadget") == 0);
    MIMHID_CHECK(fake_board_string(3, text, sizeof(text)) && std::strcmp(text, "SN-42") == 0);
    MIMHID_CHECK(fake_board_string(4, text, sizeof(text)) && std::strcmp(text, "Cfg") == 0);
    MIMHID_CHECK(fake_board_string(5, text, sizeof(text)) && std::strcmp(text, "Keys") == 0);

    uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMHID_CHECK(c[4] == 2 && length == 9 + 2 * 25);
    MIMHID_CHECK(c[7] == (0x80 | 0x40 | 0x20) && c[8] == 125);
    MIMHID_CHECK(c[9 + 20] == 0x81 && c[9 + 22] == 16 && c[9 + 24] == 4); // keyboard interface
    MIMHID_CHECK(c[9 + 25 + 20] == 0x82 && c[9 + 25 + 22] == 64 && c[9 + 25 + 24] == 1);

    // Each interface carries exactly its Spec's descriptor.
    const std::uint8_t* bytes = nullptr;
    size_t spec_length = 0;
    MIMHID_CHECK(mimhid_spec_descriptor(kbd, &bytes, &spec_length) == MIMHID_OK);
    uint16_t rd_length = 0;
    const std::uint8_t* rd = fake_board_report_descriptor(0, &rd_length);
    MIMHID_CHECK(rd_length == spec_length && std::memcmp(rd, bytes, spec_length) == 0);
    MIMHID_CHECK(mimhid_spec_descriptor(mouse, &bytes, &spec_length) == MIMHID_OK);
    rd = fake_board_report_descriptor(1, &rd_length);
    MIMHID_CHECK(rd_length == spec_length && std::memcmp(rd, bytes, spec_length) == 0);

    MIMHID_CHECK(mimhid_node_close(nm) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 1);
    MIMHID_CHECK(mimhid_node_close(nk) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 0);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    mimhid_spec_release(kbd);
    mimhid_spec_release(mouse);
}

void test_reports_and_neutral() {
    mimhid_device_config o = base_options();
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_spec* mouse = mouse_spec(2);
    mimhid_node* nk = open_node(device, kbd);
    mimhid_node* nm = open_node(device, mouse);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    fake_board_clear_reports();

    mimhid_capability_manifest manifest{};
    manifest.struct_size = sizeof(manifest);
    MIMHID_CHECK(mimhid_node_manifest(nk, &manifest) == MIMHID_OK);
    const std::uint32_t kbd_bytes = manifest.reports[0].wire_length;

    // Key down, then up.
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x04, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r));
    MIMHID_CHECK(r.data[0] == 1 && r.length == kbd_bytes && !all_zero_after_id(r, true));
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x04, 0) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && all_zero_after_id(r, true));

    // Mouse motion lands in the 16-bit X and Y fields.
    MIMHID_CHECK(mimhid_mouse_move(nm, 5, -3) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nm) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && r.data[0] == 2);
    bool seen_x = false, seen_y = false;
    for (std::size_t i = 1; i + 1 < r.length; ++i) {
        seen_x = seen_x || (r.data[i] == 0x05 && r.data[i + 1] == 0x00);
        seen_y = seen_y || (r.data[i] == 0xFD && r.data[i + 1] == 0xFF);
    }
    MIMHID_CHECK(seen_x && seen_y);

    // Closing a node with held input sends the neutral report first.
    MIMHID_CHECK(mimhid_keyboard_key(nk, 0x05, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nk) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && !all_zero_after_id(r, true));
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_node_close(nk) == MIMHID_OK);
    // The release was sent before the board re-enumerated without the keyboard.
    MIMHID_CHECK(next_report(&r) && r.data[0] == 1 && all_zero_after_id(r, true));
    MIMHID_CHECK(fake_board_interface_count() == 1);

    // Closing the Device detaches the last node.
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 0);
    mimhid_spec_release(kbd);
    mimhid_spec_release(mouse);
}

void test_touch_feature() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* touch = touch_spec(3, 1);
    mimhid_node* node = open_node(device, touch);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    std::uint8_t buffer[8] = {};
    MIMHID_CHECK(fake_board_feature(0, 3, buffer, sizeof(buffer)) == 1 && buffer[0] == 3);

    // A touch frame: contact down, then lifted.
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 0, 1, 100, 200, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.data[0] == 3);
    MIMHID_CHECK(mimhid_touchscreen_contact(node, 0, 0, 100, 200, nullptr) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit_blocking(node, 1000) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r));

    MIMHID_CHECK(mimhid_node_close(node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    mimhid_spec_release(touch);
}

void test_batch_and_packing() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* specs[4] = {keyboard_spec(1), mouse_spec(2), touch_spec(3, 0), pen_spec(4)};
    mimhid_node* nodes[4] = {};
    const unsigned connects_before = fake_board_connects();
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_ERR_BUSY);
    for (int i = 0; i < 4; ++i) {
        mimhid_node_config opt{};
        mimhid_node_config_init(&opt);
        opt.iface.number = static_cast<uint8_t>((E2E_HID_ITFS >= 4) ? (uint32_t)i : (uint32_t)(i % E2E_HID_ITFS));
        opt.iface.class_code = 3;
        opt.ep_in.address = static_cast<uint8_t>(0x81 + i);
        opt.ep_in.poll_interval_ms = 1;
        opt.ep_in.max_packet_size = 64;
        opt.iface.bcd_hid = 0x0111;
        nodes[i] = open_node(device, specs[i], &opt);
    }
    // Nothing is committed inside the batch, and staged nodes refuse reports.
    MIMHID_CHECK(fake_board_connects() == connects_before);
    MIMHID_CHECK(mimhid_keyboard_key(nodes[0], 0x04, 1) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_node_submit(nodes[0]) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(mimhid_device_batch_end(device) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_batch_end(device) == MIMHID_ERR_BUSY);
    MIMHID_CHECK(fake_board_connects() == connects_before + 1);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);

    // Four nodes need four interfaces, or are packed when the board has fewer.
    const unsigned expected = E2E_HID_ITFS >= 4 ? 4 : E2E_HID_ITFS;
    MIMHID_CHECK(fake_board_interface_count() == expected);
    std::size_t total = 0;
    for (unsigned i = 0; i < expected; ++i) {
        uint16_t length = 0;
        fake_board_report_descriptor(static_cast<std::uint8_t>(i), &length);
        total += length;
    }
    std::size_t expected_total = 0;
    for (auto* spec : specs) {
        const std::uint8_t* bytes = nullptr;
        size_t length = 0;
        MIMHID_CHECK(mimhid_spec_descriptor(spec, &bytes, &length) == MIMHID_OK);
        expected_total += length;
    }
    MIMHID_CHECK(total == expected_total);

    // Reports of packed nodes all arrive, each with its own Report ID.
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_keyboard_key(nodes[0], 0x04, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nodes[0]) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.data[0] == 1);
    MIMHID_CHECK(mimhid_mouse_move(nodes[1], 1, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_submit(nodes[1]) == MIMHID_OK);
    MIMHID_CHECK(next_report(&r) && r.data[0] == 2);

    // Closing inside a batch only stages the change.
    MIMHID_CHECK(mimhid_device_batch_begin(device) == MIMHID_OK);
    const unsigned connects = fake_board_connects();
    for (auto* node : nodes)
        MIMHID_CHECK(mimhid_node_close(node) == MIMHID_OK);
    MIMHID_CHECK(fake_board_connects() == connects);
    MIMHID_CHECK(mimhid_device_batch_end(device) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 0);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    for (auto* spec : specs)
        mimhid_spec_release(spec);
}

void test_limits() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    // Nodes without Report IDs cannot share an interface, so the board's interface count is a hard
    // limit; the node count is another.
    std::vector<mimhid_node*> nodes;
    std::vector<mimhid_spec*> specs;
    mimhid_result last = MIMHID_OK;
    const unsigned limit = E2E_HID_ITFS < 8 ? E2E_HID_ITFS : 8;
    for (unsigned i = 0; i < limit + 1; ++i) {
        specs.push_back(raw_spec_no_id());
        mimhid_node_config no{};
        mimhid_node_config_init(&no);
        no.iface.number = static_cast<uint8_t>(i);
        no.iface.class_code = 3;
        no.ep_in.address = static_cast<uint8_t>(0x81 + (i % 8));
        no.ep_in.poll_interval_ms = 1;
        no.ep_in.max_packet_size = 64;
        no.iface.bcd_hid = 0x0111;
        mimhid_node* node = nullptr;
        last = mimhid_node_open(device, specs.back(), &no, &node);
        if (last != MIMHID_OK)
            break;
        nodes.push_back(node);
    }
    MIMHID_CHECK(nodes.size() == limit && last == MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(fake_board_interface_count() == limit);
    // The failed open left the others working.
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    for (auto* node : nodes)
        MIMHID_CHECK(mimhid_node_close(node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    for (auto* spec : specs)
        mimhid_spec_release(spec);
}

void test_events_and_raw() {
    mimhid_device_config o = base_options();
    o.usb_config.bm_attributes |= 0x20;
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    mimhid_spec* raw = raw_spec_io();
    mimhid_node* node = open_node(device, raw);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));

    // The target writes an Output report and reads a Feature report.
    const std::uint8_t out_data[] = {0x12};
    fake_board_target_set_report(0, 2, 5, out_data, sizeof(out_data));
    MIMHID_CHECK(wait_event(MIMHID_EVENT_OUTPUT_REPORT, &ev));
    MIMHID_CHECK(ev.node_id == 1 && ev.report_id == 5 && ev.data.size() == 1 && ev.data[0] == 0x12);
    const std::uint8_t feature[] = {0xAB, 0xCD};
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, feature, sizeof(feature)) == MIMHID_OK);
    std::uint8_t buffer[8] = {};
    MIMHID_CHECK(fake_board_feature(0, 5, buffer, sizeof(buffer)) == 2 && buffer[0] == 0xAB &&
                 buffer[1] == 0xCD);

    // Frames longer than 127 bytes carry a two-byte length: a Feature answer the target reads
    // whole, and a report the target writes.
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_feature_bytes == 1024 && limits.max_report_bytes == 64 &&
                 limits.max_endpoint_bytes == 64);
    std::vector<std::uint8_t> big(644);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, big.data(), big.size()) == MIMHID_OK);
    std::vector<std::uint8_t> got(1024);
    MIMHID_CHECK(fake_board_feature(0, 5, got.data(), got.size()) == big.size() &&
                 std::memcmp(got.data(), big.data(), big.size()) == 0);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, big.data(), 1025) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_set_feature_response(node, 5, feature, sizeof(feature)) == MIMHID_OK);
    fake_board_target_set_report(0, 2, 5, big.data(), 300);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_OUTPUT_REPORT, &ev));
    MIMHID_CHECK(ev.report_id == 5 && ev.data.size() == 300 &&
                 std::memcmp(ev.data.data(), big.data(), 300) == 0);

    fake_board_target_set_protocol(0, 0);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SET_PROTOCOL, &ev) && ev.node_id == 1 && ev.value == 0);
    fake_board_target_set_idle(0, 9);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SET_IDLE, &ev) && ev.value == 9);

    // Remote wakeup works only while the target has the device suspended.
    MIMHID_CHECK(mimhid_device_remote_wakeup(device) == MIMHID_ERR_BUSY);
    fake_board_target_suspend(1);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_SUSPEND, &ev) && ev.value == 1);
    MIMHID_CHECK(mimhid_device_remote_wakeup(device) == MIMHID_OK);
    fake_board_target_resume();
    MIMHID_CHECK(wait_event(MIMHID_EVENT_RESUME, &ev));

    // Raw reports: exact length and Report ID only.
    fake_board_clear_reports();
    const std::uint8_t good[] = {5, 0x01};
    MIMHID_CHECK(mimhid_raw_submit(node, good, sizeof(good)) == MIMHID_OK);
    fake_report_t r{};
    MIMHID_CHECK(next_report(&r) && r.length == 2 && r.data[0] == 5 && r.data[1] == 1);
    const std::uint8_t bad[] = {6, 0x01};
    MIMHID_CHECK(mimhid_raw_submit(node, bad, sizeof(bad)) == MIMHID_ERR_PARAM);

    MIMHID_CHECK(mimhid_node_close(node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    mimhid_spec_release(raw);
}

void test_chip_serial_and_boot() {
    mimhid_device_config o = base_options();
    o.usb_device.serial_from_chip_id = 1;
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimhid_spec* raw = raw_spec_no_id();
    mimhid_node_config no{};
    mimhid_node_config_init(&no);
    no.iface.number = 0;
    no.iface.class_code = 3;
    no.iface.subclass = 1;
    no.iface.protocol = 1;
    no.ep_in.poll_interval_ms = 1;
    no.ep_in.address = 0x81;
    no.ep_in.max_packet_size = 6;
    no.ep_out.address = 0x01;
    no.ep_out.max_packet_size = 5;
    no.ep_out.poll_interval_ms = 1;
    no.iface.country_code = 33;
    no.iface.bcd_hid = 0x0110;
    mimhid_node* node = open_node(device, raw, &no);
    char text[64];
    MIMHID_CHECK(fake_board_string(3, text, sizeof(text)) &&
                 std::strcmp(text, "DEADBEEF01020304") == 0);
    uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMHID_CHECK(c[9 + 5] == 3 && c[9 + 6] == 1 && c[9 + 7] == 1); // boot keyboard
    MIMHID_CHECK(c[9 + 11] == 0x10 && c[9 + 12] == 0x01 && c[9 + 13] == 33);
    // Packet sizes are taken as given, not rounded to a power of two.
    MIMHID_CHECK(c[9 + 20] == 0x81 && c[9 + 22] == 6 && c[9 + 23] == 0);
    MIMHID_CHECK(c[9 + 25 + 2] == 0x01 && c[9 + 25 + 4] == 5 && c[9 + 25 + 5] == 0);
    mimhid_node* bad = nullptr;
    mimhid_node_config broken = no;
    broken.iface.class_code = 0;
    MIMHID_CHECK(mimhid_node_open(device, raw, &broken, &bad) == MIMHID_ERR_UNSET_FIELD &&
                 bad == nullptr);
    broken = no;
    // The board's endpoints hold 64 bytes.
    broken.ep_in.max_packet_size = 65;
    MIMHID_CHECK(mimhid_node_open(device, raw, &broken, &bad) == MIMHID_ERR_OVERFLOW);
    broken.ep_in.max_packet_size = 64;
    broken.ep_out.max_packet_size = 65;
    MIMHID_CHECK(mimhid_node_open(device, raw, &broken, &bad) == MIMHID_ERR_OVERFLOW);
    MIMHID_CHECK(mimhid_node_close(node) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    mimhid_spec_release(raw);
}

void test_keep_attached_and_discovery() {
    mimhid_device_config o = base_options();
    o.transport.keep_attached = 1;
    mimhid_device* device = open_device(o);
    if (device == nullptr)
        return;
    mimhid_spec* raw = raw_spec_no_id();
    mimhid_node* node = open_node(device, raw);
    MIMHID_CHECK(node != nullptr);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 1); // the USB device stays attached
    mimhid_spec_release(raw);

    // A closed device can be opened again; the board answers pings with its limits.
    mimhid_device* again = open_device(base_options());
    if (again != nullptr) {
        mimhid_device_info info{};
        MIMHID_CHECK(mimhid_device_board_info(again, &info) == MIMHID_OK &&
                     info.max_interfaces == E2E_HID_ITFS);
        MIMHID_CHECK(mimhid_device_close(again) == MIMHID_OK);
    }
}

// Frames written by hand on the slave end, as another program would.
void raw_frame(int fd, std::uint8_t type, const std::uint8_t* payload, std::uint8_t length) {
    std::uint8_t frame[8];
    std::uint8_t x = static_cast<std::uint8_t>(type ^ length);
    frame[0] = MIMHID_SYNC;
    frame[1] = type;
    frame[2] = length;
    for (std::uint8_t i = 0; i < length; ++i) {
        frame[3 + i] = payload[i];
        x = static_cast<std::uint8_t>(x ^ payload[i]);
    }
    frame[3 + length] = x;
    MIMHID_CHECK(write(fd, frame, 4U + length) == 4 + length);
}

bool raw_answer(int fd, std::uint8_t type) {
    std::uint8_t buf[256];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::size_t have = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = read(fd, buf + have, sizeof(buf) - have);
        if (n > 0)
            have += static_cast<std::size_t>(n);
        for (std::size_t i = 0; i + 1 < have; ++i) {
            if (buf[i] == MIMHID_SYNC && buf[i + 1] == type)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void set_speed(int fd, std::uint32_t baud) {
#ifdef __APPLE__
    const speed_t speed = baud; // speeds are plain numbers there
#else
    const speed_t speed = baud == 3000000U ? B3000000 : B115200;
#endif
    termios tio{};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tcsetattr(fd, TCSANOW, &tio);
}

mimhid_device* open_at(std::uint32_t baud, mimhid_result* result) {
    mimhid_device_config o = base_options();
    o.transport.baud_rate = baud;
    mimhid_device* device = nullptr;
    *result = mimhid_device_open(g_slave.c_str(), &o, &device);
    return device;
}

std::uint32_t link_baud(const mimhid_device* device) {
    mimhid_device_info info{};
    return mimhid_device_board_info(device, &info) == MIMHID_OK ? info.baud_rate : 0U;
}

// What mimhid_discover measures on each port, run on the pseudo-terminal.
std::uint32_t measure() {
    mimhid::transport::SerialPort port;
    if (!port.open(g_slave.c_str(), MIMHID_BOOT_BAUD).ok)
        return 0;
    mimhid::transport::Handshaker handshake(port, 50);
    mimhid::transport::BoardInfo board;
    if (!handshake.find(&board))
        return 0;
    const std::uint32_t fastest = handshake.fastest(&board);
    return handshake.to_boot(&board) ? fastest : 0;
}

void test_line_rate() {
    mimhid_result r = MIMHID_OK;
    // The rate is the caller's: none is chosen for it.
    mimhid_device* device = open_at(0, &r);
    MIMHID_CHECK(r == MIMHID_ERR_UNSET_FIELD && device == nullptr);

    // The fastest rate the board makes; the board is left on the boot rate.
    MIMHID_CHECK(measure() == 3000000U && fake_board_baud() == MIMHID_BOOT_BAUD);
    device = open_at(3000000U, &r);
    MIMHID_CHECK(r == MIMHID_OK && link_baud(device) == 3000000U);
    MIMHID_CHECK(fake_board_baud() == 3000000U);
    mimhid_device_close(device);
    MIMHID_CHECK(fake_board_baud() == MIMHID_BOOT_BAUD);

    // An adapter that cannot go past 1,000,000 sends at that rate whatever it is set to: the
    // faster rates are tried and given up, and 1,000,000 is measured.
    fake_board_set_adapter_limit(1000000U);
    MIMHID_CHECK(measure() == 1000000U && fake_board_baud() == MIMHID_BOOT_BAUD);
    // A rate the adapter does not carry fails, and leaves the board on the boot rate.
    device = open_at(2000000U, &r);
    MIMHID_CHECK(r == MIMHID_ERR_UNSUPPORTED && device == nullptr);
    MIMHID_CHECK(fake_board_baud() == MIMHID_BOOT_BAUD);
    fake_board_set_adapter_limit(0);

    // A rate the board cannot make, and a slower one.
    device = open_at(4000000U, &r);
    MIMHID_CHECK(r == MIMHID_ERR_UNSUPPORTED && device == nullptr);
    device = open_at(921600U, &r);
    MIMHID_CHECK(r == MIMHID_OK && link_baud(device) == 921600U && fake_board_baud() == 921600U);
    mimhid_device_close(device);

    // A program that went away left the board on 3,000,000: it is found there.
    const int fd = open(g_slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    MIMHID_CHECK(fd >= 0);
    set_speed(fd, 115200U);
    const std::uint8_t to_fast[2] = {0x55, 7};
    raw_frame(fd, MIMHID_T_SET_BAUD, to_fast, 2);
    MIMHID_CHECK(raw_answer(fd, MIMHID_T_ACK));
    set_speed(fd, 3000000U);
    const std::uint8_t version[1] = {MIMHID_PROTO_VERSION};
    raw_frame(fd, MIMHID_T_PING, version, 1);
    MIMHID_CHECK(raw_answer(fd, MIMHID_T_PONG));
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * MIMHID_BAUD_CONFIRM_MS));
    MIMHID_CHECK(fake_board_baud() == 3000000U);
    device = open_at(921600U, &r);
    MIMHID_CHECK(r == MIMHID_OK && link_baud(device) == 921600U);
    mimhid_device_close(device);
    MIMHID_CHECK(fake_board_baud() == MIMHID_BOOT_BAUD);

#ifdef __linux__
    // Only one program has the port. A macOS pseudo-terminal does not keep TIOCEXCL, so this
    // is checked on Linux only.
    device = open_at(3000000U, &r);
    MIMHID_CHECK(r == MIMHID_OK);
    mimhid_device* second = open_at(3000000U, &r);
    MIMHID_CHECK(r == MIMHID_ERR_ACCESS && second == nullptr);
    mimhid_device_close(device);
#endif
}

// Strings that are not given are left out: the library does not name the device.
void test_no_names() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* raw = raw_spec_no_id();
    mimhid_node* node = open_node(device, raw);
    MIMHID_CHECK(node != nullptr);
    const std::uint8_t* d = fake_board_device_descriptor();
    MIMHID_CHECK(d[14] == 0 && d[15] == 0 && d[16] == 0); // iManufacturer, iProduct, iSerial
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    mimhid_spec_release(raw);
}

void test_bad_arguments() {
    mimhid_device_info info{};
    std::strncpy(info.port_name, g_slave.c_str(), sizeof(info.port_name) - 1);
    mimhid_device* device = nullptr;
    mimhid_device_config o = base_options();
    o.usb_device.vendor_id = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD &&
                 device == nullptr);
    o = base_options();
    o.transport.linux_hid_usages_policy = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    // Nothing is chosen for the caller.
    for (std::uint32_t mimhid_transport_options::*field :
         {&mimhid_transport_options::control_timeout_ms, &mimhid_transport_options::send_timeout_ms,
          &mimhid_transport_options::reconnect_timeout_ms,
          &mimhid_transport_options::close_drain_timeout_ms}) {
        o = base_options();
        o.transport.*field = 0;
        MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    }
    o = base_options();
    o.usb_device.language_id = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.bcd_usb = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_device.ep0_max_packet_size = 48;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_config.configuration_value = 0;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSET_FIELD);
    o = base_options();
    o.usb_config.bm_attributes = 0x00;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.usb_config.bmax_power = 251;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.transport.baud_rate = 12345;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_UNSUPPORTED);
    o = base_options();
    o.usb_device.manufacturer = "caf\xC3\xA9";
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    o.struct_size = 8;
    MIMHID_CHECK(mimhid_device_open(info.port_name, &o, &device) == MIMHID_ERR_PARAM);
    o = base_options();
    MIMHID_CHECK(mimhid_device_open("/dev/does-not-exist", &o, &device) == MIMHID_ERR_ACCESS);
    MIMHID_CHECK(mimhid_device_open(nullptr, &o, &device) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_open("", &o, &device) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_close(nullptr) == MIMHID_ERR_PARAM);
}

void test_link_loss() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(1);
    mimhid_node* node = open_node(device, kbd);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    // The board end of the cable disappears.
    close(g_master);
    g_master = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    mimhid_result r = MIMHID_OK;
    while (std::chrono::steady_clock::now() < deadline) {
        r = mimhid_device_latched_error(device);
        if (r == MIMHID_ERR_NO_DEVICE)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MIMHID_CHECK(r == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_keyboard_key(node, 0x04, 1) == MIMHID_ERR_NO_DEVICE);
    MIMHID_CHECK(mimhid_device_is_ready(device) == 0);
    mimhid_device_close(device);
    mimhid_spec_release(kbd);
}

// ---- control transfers --------------------------------------------------------------
mimhid_control_rule control_rule(std::uint8_t bm, std::uint8_t request, std::uint32_t timeout_ms,
                                 mimhid_control_action when_late) {
    mimhid_control_rule r{};
    r.request_type_mask = 0xFF;
    r.request_type_value = bm;
    r.request_mask = 0xFF;
    r.request_value = request;
    r.timeout_ms = timeout_ms;
    r.when_late = when_late;
    return r;
}

// Sends a request from the target. Events of earlier transfers that nobody read are dropped first.
void target_control(std::uint8_t bm, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                    std::uint16_t length, const std::uint8_t* out = nullptr, std::size_t out_length = 0) {
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    const std::uint8_t setup[8] = {bm, request, static_cast<std::uint8_t>(value),
                                   static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(index),
                                   static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(length),
                                   static_cast<std::uint8_t>(length >> 8)};
    fake_board_target_control(setup, out, out_length);
}

// The next report of the request with this type and number.
bool wait_setup(EventRecord* ev, std::uint8_t bm, std::uint8_t request, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_event(MIMHID_EVENT_CONTROL_SETUP, ev, 100))
            continue;
        if (ev->data.size() == 8 && ev->data[0] == bm && ev->data[1] == request)
            return true;
    }
    return false;
}

bool next_control(fake_control_result_t* r, int timeout_ms = 2000) {
    return fake_board_wait_control(r, static_cast<std::uint32_t>(timeout_ms)) != 0;
}

void test_control() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK && limits.max_control_rules == 8 &&
                 limits.max_control_data_bytes == 1024);

    // Arguments.
    const mimhid_control_rule good = control_rule(0xC0, 0x42, 100, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(nullptr, 0, &good, 1) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 2, &good, 1) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, nullptr, 1) == MIMHID_ERR_PARAM);
    mimhid_control_rule many[MIMHID_CONTROL_MAX_RULES + 1];
    for (auto& r : many)
        r = good;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, many, MIMHID_CONTROL_MAX_RULES + 1) ==
                 MIMHID_ERR_PARAM);
    mimhid_control_rule bad = good;
    bad.timeout_ms = 0;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1) == MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = 0;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1) == MIMHID_ERR_UNSET_FIELD);
    bad = good;
    bad.when_late = 5;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1) == MIMHID_ERR_PARAM);
    bad = good;
    bad.timeout_ms = MIMHID_CONTROL_MAX_TIMEOUT_MS + 1;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1) == MIMHID_ERR_PARAM);
    bad = good;
    bad.value_value = 1; // outside its mask of zero
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &bad, 1) == MIMHID_ERR_PARAM);
    const std::uint8_t one[1] = {1};
    MIMHID_CHECK(mimhid_device_control_reply(device, 0, MIMHID_CONTROL_ACTION_ANSWER, nullptr, 0) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 256, MIMHID_CONTROL_ACTION_ANSWER, nullptr, 0) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, 0, nullptr, 0) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, MIMHID_CONTROL_ACTION_STALL, one, 1) ==
                 MIMHID_ERR_PARAM);
    std::vector<std::uint8_t> too_big(1025);
    MIMHID_CHECK(mimhid_device_control_reply(device, 1, MIMHID_CONTROL_ACTION_ANSWER, too_big.data(),
                                             too_big.size()) == MIMHID_ERR_OVERFLOW);

    // Reporting every transfer, holding none.
    fake_control_result_t r{};
    EventRecord ev;
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 1, nullptr, 0) == MIMHID_OK);
    target_control(0x80, 6, 0x0302, 0x0409, 255);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6) && ev.flags == 0 && ev.token != 0 &&
                 ev.data.size() == 8 && ev.data[0] == 0x80 && ev.data[1] == 6 && ev.data[2] == 0x02 &&
                 ev.data[3] == 0x03 && ev.data[4] == 0x09 && ev.data[5] == 0x04 && ev.data[6] == 255);
    target_control(0x00, 5, 9, 0, 0); // SET_ADDRESS
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(wait_setup(&ev, 0x00, 5) && ev.flags == MIMHID_CONTROL_FLAG_FIXED);

    // An answer with data, given by the program's own thread.
    const mimhid_control_rule vendor_in = control_rule(0xC0, 0x42, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &vendor_in, 1) == MIMHID_OK);
    target_control(0xC0, 0x42, 0x1234, 0x5678, 64);
    MIMHID_CHECK(wait_setup(&ev, 0xC0, 0x42) && ev.flags == MIMHID_CONTROL_FLAG_HELD &&
                 ev.data[2] == 0x34 && ev.data[3] == 0x12);
    const std::vector<std::uint8_t> payload = {9, 8, 7, 6, 5};
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER,
                                             payload.data(), payload.size()) == MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == payload.size() &&
                 std::memcmp(r.data, payload.data(), payload.size()) == 0);

    // The same, answered from inside the event sink; the whole answer goes out even when wLength is
    // shorter, as a device that overruns the host's length does.
    g_sink_reply.device = device;
    g_sink_reply.action = MIMHID_CONTROL_ACTION_ANSWER;
    g_sink_reply.data = payload;
    g_sink_reply.enabled.store(true);
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == payload.size() &&
                 std::memcmp(r.data, payload.data(), payload.size()) == 0);
    MIMHID_CHECK(g_sink_reply.result.load() == MIMHID_OK);
    g_sink_reply.action = MIMHID_CONTROL_ACTION_STALL;
    g_sink_reply.data.clear();
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    g_sink_reply.enabled.store(false);

    // Nobody answers: the rule's action after its time (a stall here).
    const mimhid_control_rule quick = control_rule(0xC0, 0x42, 60, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &quick, 1) == MIMHID_OK);
    const auto start = std::chrono::steady_clock::now();
    target_control(0xC0, 0x42, 0, 0, 3);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    const auto waited = std::chrono::steady_clock::now() - start;
    MIMHID_CHECK(waited >= std::chrono::milliseconds(55) && waited < std::chrono::milliseconds(1000));

    // A standard request answered after its time by an empty data stage, and one left to the stack.
    const mimhid_control_rule std_get = control_rule(0x80, 6, 40, MIMHID_CONTROL_ACTION_ANSWER);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &std_get, 1) == MIMHID_OK);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ANSWERED && r.length == 0 &&
                 r.setup[0] == 0xC0 && r.setup[1] == 6); // retyped
    const mimhid_control_rule std_leave = control_rule(0x80, 6, 40, MIMHID_CONTROL_ACTION_LEAVE);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &std_leave, 1) == MIMHID_OK);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT && r.setup[0] == 0x80);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6) && ev.flags == MIMHID_CONTROL_FLAG_HELD);
    target_control(0x80, 6, 0x0300, 0, 4);
    MIMHID_CHECK(wait_setup(&ev, 0x80, 6));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_LEAVE, nullptr, 0) ==
                 MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT && r.setup[0] == 0x80);

    // A request without a data stage: ANSWER is an acknowledgement.
    const mimhid_control_rule no_data = control_rule(0x00, 3, 500, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &no_data, 1) == MIMHID_OK);
    target_control(0x00, 3, 1, 0, 0);
    MIMHID_CHECK(wait_setup(&ev, 0x00, 3));
    MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER, nullptr, 0) ==
                 MIMHID_OK);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_ACKED);

    // Host to device with a data stage: the program sees the data and gives the verdict.
    const mimhid_control_rule set_report = control_rule(0x21, 9, 1000, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &set_report, 1) == MIMHID_OK);
    const std::uint8_t sent[3] = {0x11, 0x22, 0x33};
    for (int round = 0; round < 2; ++round) {
        target_control(0x21, 9, 0x0302, 0, 3, sent, sizeof(sent));
        MIMHID_CHECK(wait_setup(&ev, 0x21, 9) && ev.flags == MIMHID_CONTROL_FLAG_HELD);
        MIMHID_CHECK(mimhid_device_control_reply(device, ev.token, MIMHID_CONTROL_ACTION_ANSWER, nullptr,
                                                 0) == MIMHID_OK);
        EventRecord data_ev;
        MIMHID_CHECK(wait_event(MIMHID_EVENT_CONTROL_DATA, &data_ev) && data_ev.token == ev.token &&
                     data_ev.data.size() == 3 && std::memcmp(data_ev.data.data(), sent, 3) == 0);
        MIMHID_CHECK(mimhid_device_control_reply(device, data_ev.token,
                                                 round == 0 ? MIMHID_CONTROL_ACTION_ANSWER
                                                            : MIMHID_CONTROL_ACTION_STALL,
                                                 nullptr, 0) == MIMHID_OK);
        MIMHID_CHECK(next_control(&r) &&
                     r.outcome == (round == 0 ? FAKE_CONTROL_ACKED : FAKE_CONTROL_STALLED));
        MIMHID_CHECK(r.length == 3 && std::memcmp(r.data, sent, 3) == 0);
    }
    // A host-to-device transfer whose verdict never comes: the rule's action.
    const mimhid_control_rule slow = control_rule(0x21, 9, 50, MIMHID_CONTROL_ACTION_STALL);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, &slow, 1) == MIMHID_OK);
    g_sink_reply.action = MIMHID_CONTROL_ACTION_ANSWER;
    g_sink_reply.data.clear();
    g_sink_reply.enabled.store(true);
    target_control(0x21, 9, 0x0302, 0, 3, sent, sizeof(sent));
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_STALLED);
    g_sink_reply.enabled.store(false);

    // Taking the routing away, and closing the Device, leave the board answering for itself.
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 0, nullptr, 0) == MIMHID_OK);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    target_control(0x80, 6, 0x0100, 0, 18);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    MIMHID_CHECK(mimhid_device_set_control_routing(device, 1, &std_get, 1) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
    device = open_device(base_options());
    if (device == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.clear();
    }
    target_control(0x80, 6, 0x0100, 0, 18);
    MIMHID_CHECK(next_control(&r) && r.outcome == FAKE_CONTROL_LEFT);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        for (const auto& e : g_events)
            MIMHID_CHECK(e.kind != MIMHID_EVENT_CONTROL_SETUP);
    }
    MIMHID_CHECK(mimhid_device_close(device) == MIMHID_OK);
}

} // namespace

int main() {
    if (!open_pty()) {
        std::fprintf(stderr, "cannot create a pseudo-terminal\n");
        return 1;
    }
    if (fake_board_start(g_master) != 0)
        return 1;

    test_bad_arguments();
    test_line_rate();
    test_no_names();
    test_open_identity_and_descriptors();
    test_reports_and_neutral();
    test_touch_feature();
    test_batch_and_packing();
    test_limits();
    test_events_and_raw();
    test_chip_serial_and_boot();
    test_keep_attached_and_discovery();
    test_control();
    test_link_loss();

    fake_board_stop();
    if (g_failures == 0)
        std::printf("end-to-end OK (%d HID interfaces)\n", E2E_HID_ITFS);
    return g_failures == 0 ? 0 : 1;
}
