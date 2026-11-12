// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The shared parts of the simulated-board scenarios (sim_support.hpp). */
#include "sim_support.hpp"

#include "fill_values.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

int g_checks = 0;
int g_failures = 0;

namespace sim {

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

std::mutex g_event_mutex;

std::vector<EventRecord> g_events;

SinkReply g_sink_reply;

void MIMUSB_CALL on_event(void*, const mimusb_event* event) {
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_events.push_back({event->kind, event->node_id, event->report_id, event->value,
                            std::vector<std::uint8_t>(event->data, event->data + event->length),
                            event->control_token, event->control_flags, event->endpoint_address,
                            event->interface_number});
    }
    if (event->kind == MIMUSB_EVENT_CONTROL_SETUP &&
        (event->control_flags & MIMUSB_CONTROL_FLAG_HELD) && g_sink_reply.enabled.load()) {
        g_sink_reply.result.store(static_cast<int>(mimusb_device_control_reply(
            g_sink_reply.device, event->control_token, g_sink_reply.action,
            g_sink_reply.data.empty() ? nullptr : g_sink_reply.data.data(),
            g_sink_reply.data.size(), kSendMs)));
    }
}

bool wait_event(mimusb_event_kind kind, EventRecord* out, int timeout_ms) {
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

mimusb_device_config base_options() {
    mimusb_device_config o{};
    test_fill_device_config(&o);
    o.transport.baud_rate = 3000000;
    o.usb_device.vendor_id = 0x1209; // pid.codes test IDs
    o.usb_device.product_id = 0x0001;
    o.transport.validate_reports = 1;
    o.transport.event_sink = on_event;
    return o;
}

mimusb_device* open_device(const mimusb_device_config& options) {
    mimusb_device_info info{};
    std::strncpy(info.port_name, g_slave.c_str(), sizeof(info.port_name) - 1);
    mimusb_device* device = nullptr;
    const mimusb_result r = mimusb_device_open(info.port_name, &options, &device);
    if (r != MIMUSB_OK) {
        std::fprintf(stderr, "device_open: %s (%s)\n", mimusb_result_name(r),
                     mimusb_last_error()->reason);
        ++g_failures;
        return nullptr;
    }
    return device;
}

mimusb_spec* keyboard_spec(std::uint8_t report_id) {
    mimusb_keyboard_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.usage_minimum = 0x04;
    o.usage_maximum = 0x65;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_keyboard(&o, &spec) == MIMUSB_OK);
    return spec;
}

mimusb_spec* mouse_spec(std::uint8_t report_id) {
    mimusb_mouse_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.button_count = 3;
    o.x = {-32767, 32767, 16, {}};
    o.y = {-32767, 32767, 16, {}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_mouse(&o, &spec) == MIMUSB_OK);
    return spec;
}

mimusb_spec* touch_spec(std::uint8_t report_id, std::uint32_t feature) {
    mimusb_touchscreen_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.maximum_contacts = 3;
    o.contacts_per_report = 3;
    o.contact_id = {0, 15, 4, {}};
    o.x = {0, 32767, 16, {}};
    o.y = {0, 32767, 16, {}};
    o.contact_count = {0, 3, 2, {}};
    o.enable_contact_count_maximum_feature_declaration = feature;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_touchscreen(&o, &spec) == MIMUSB_OK);
    return spec;
}

mimusb_spec* pen_spec(std::uint8_t report_id) {
    mimusb_pen_options o{};
    o.struct_size = sizeof(o);
    o.report_id = {report_id != 0 ? 1U : 0U, report_id, {0, 0, 0}};
    o.x = {0, 32767, 16, {}};
    o.y = {0, 32767, 16, {}};
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_pen(&o, &spec) == MIMUSB_OK);
    return spec;
}

namespace {
constexpr std::uint8_t kRawDescriptorNoId[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x15,
                                               0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
                                               0x09, 0x01, 0x81, 0x02, 0xC0};
} // namespace

mimusb_spec* raw_spec_no_id() {
    static const mimusb_raw_report report{{0, 0, {}}, 1};
    mimusb_raw_options o{};
    o.struct_size = sizeof(o);
    o.descriptor = kRawDescriptorNoId;
    o.descriptor_length = sizeof(kRawDescriptorNoId);
    o.reports = &report;
    o.report_count = 1;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_raw(&o, &spec) == MIMUSB_OK);
    return spec;
}

namespace {
constexpr std::uint8_t kRawDescriptorIo[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x05, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x08, 0x09, 0x01, 0x81, 0x02, 0x09, 0x02, 0x91, 0x02, 0x09, 0x03, 0xB1, 0x02, 0xC0};
} // namespace

mimusb_spec* raw_spec_io() {
    static const mimusb_raw_report report{{1, 5, {}}, 2};
    mimusb_raw_options o{};
    o.struct_size = sizeof(o);
    o.descriptor = kRawDescriptorIo;
    o.descriptor_length = sizeof(kRawDescriptorIo);
    o.reports = &report;
    o.report_count = 1;
    o.requires_output = 1;
    o.requires_feature_response = 1;
    mimusb_spec* spec = nullptr;
    MIMUSB_CHECK(mimusb_spec_create_raw(&o, &spec) == MIMUSB_OK);
    return spec;
}

mimusb_node* open_node(mimusb_device* device, mimusb_spec* spec,
                       const mimusb_hid_node_config* options) {
    mimusb_hid_node_config defaults{};
    test_fill_node_config(&defaults, 0);
    mimusb_node* node = nullptr;
    const mimusb_result r = mimusb_node_open_profile(
        device, spec, options != nullptr ? options : &defaults, &kCommit, &node);
    if (r != MIMUSB_OK) {
        std::fprintf(stderr, "node_open: %s (%s)\n", mimusb_result_name(r),
                     mimusb_last_error()->reason);
        ++g_failures;
        return nullptr;
    }
    return node;
}

mimusb_node* open_vendor_node(mimusb_device* device, const mimusb_control_rule* rules,
                              const std::size_t rule_count) {
    mimusb_usb_endpoint ep{};
    ep.address = 0x81;
    ep.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    ep.max_packet_size = 64;
    ep.interval = 1;
    ep.policy = {MIMUSB_FULL_DROP_NEWEST, 4};
    mimusb_usb_interface itf{};
    itf.class_code = 0xFF;
    itf.endpoints = &ep;
    itf.endpoint_count = 1;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    config.rules = rules;
    config.rule_count = rule_count;
    mimusb_node* node = nullptr;
    const mimusb_result r = mimusb_node_open(device, &config, &kCommit, &node);
    if (r != MIMUSB_OK) {
        std::fprintf(stderr, "node_open: %s (%s)\n", mimusb_result_name(r),
                     mimusb_last_error()->reason);
        ++g_failures;
        return nullptr;
    }
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 3000) == MIMUSB_OK);
    return node;
}

bool next_report(fake_report_t* r, int timeout_ms) {
    return fake_board_wait_report(r, static_cast<std::uint32_t>(timeout_ms)) != 0;
}

bool all_zero_after_id(const fake_report_t& r, bool has_id) {
    for (std::size_t i = has_id ? 1U : 0U; i < r.length; ++i) {
        if (r.data[i] != 0)
            return false;
    }
    return true;
}

} // namespace sim
