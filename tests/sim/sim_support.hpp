// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* What the simulated-board scenarios share: the pseudo-terminal the library and the simulated board
 * talk over, the event sink and its record, the waits every call takes, and the specs and
 * options the scenarios start from. Test-only. */
#pragma once

#include "fake_board.h"
#include "mimusb.h"
#include "test.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifndef SIM_ENDPOINTS
#define SIM_ENDPOINTS 7
#endif

namespace sim {

// The waits every call takes; the library keeps none.
inline constexpr std::uint32_t kControlMs = 500;
inline constexpr std::uint32_t kSendMs = 500;
inline constexpr std::uint32_t kReconnectMs = 3000;
inline constexpr std::uint32_t kDrainMs = 1000;
inline const mimusb_commit_timeouts kCommit = {sizeof(mimusb_commit_timeouts), 0, kControlMs,
                                               kReconnectMs};

// The board end (master) and the library end (slave path) of the pseudo-terminal.
extern int g_master;
extern std::string g_slave;
bool open_pty();

// ---- events -------------------------------------------------------------------------
struct EventRecord {
    mimusb_event_kind kind;
    std::uint16_t node_id;
    std::uint16_t report_id;
    std::uint32_t value;
    std::vector<std::uint8_t> data;
    std::uint32_t token;
    std::uint32_t flags;
    std::uint32_t endpoint_address;
    std::uint32_t interface_number;
};
extern std::mutex g_event_mutex;
extern std::vector<EventRecord> g_events;

// A reply the sink gives by itself to every held transfer that it sees (control tests).
struct SinkReply {
    std::atomic<bool> enabled{false};
    mimusb_device* device{nullptr};
    mimusb_control_action action{MIMUSB_CONTROL_ACTION_ANSWER};
    std::vector<std::uint8_t> data;
    std::atomic<int> result{-1};
};
extern SinkReply g_sink_reply;

void MIMUSB_CALL on_event(void*, const mimusb_event* event);
bool wait_event(mimusb_event_kind kind, EventRecord* out, int timeout_ms = 2000);

// ---- helpers ------------------------------------------------------------------------
mimusb_device_config base_options();
mimusb_device* open_device(const mimusb_device_config& options);
mimusb_spec* keyboard_spec(std::uint8_t report_id);
mimusb_spec* mouse_spec(std::uint8_t report_id);
mimusb_spec* touch_spec(std::uint8_t report_id, std::uint32_t feature);
mimusb_spec* pen_spec(std::uint8_t report_id);
// A raw spec with one 1-byte Input report and no Report ID.
mimusb_spec* raw_spec_no_id();
// A raw spec with an Output and a Feature report, both under Report ID 5.
mimusb_spec* raw_spec_io();
mimusb_node* open_node(mimusb_device* device, mimusb_spec* spec,
                       const mimusb_hid_node_config* options = nullptr);
// A Node of one vendor interface (number 0, class 0xFF) with an interrupt IN endpoint 0x81 and the
// rules given, committed; the Device is waited on until the target has it mounted. The smallest
// thing that gives the Device a USB configuration to hold rules in.
mimusb_node* open_vendor_node(mimusb_device* device, const mimusb_control_rule* rules = nullptr,
                              std::size_t rule_count = 0);
bool next_report(fake_report_t* r, int timeout_ms = 2000);
bool all_zero_after_id(const fake_report_t& r, bool has_id);

// ---- scenarios, by file; test_sim_main.cpp decides the order ------------------------
// test_sim_open.cpp
void test_open_identity_and_descriptors();
void test_no_names();
void test_zero_identity();
void test_chip_serial_and_boot();
void test_keep_attached_and_discovery();
void test_filled_values();
void test_init_fills_nothing();
void test_shared_interfaces();
// test_sim_arguments.cpp
void test_bad_arguments();
// test_sim_line_rate.cpp
void test_line_rate();
// test_sim_reports.cpp
void test_reports_and_neutral();
void test_queue_depth_and_loss();
void test_touch_feature();
void test_batch_and_packing();
void test_limits();
void test_interface_numbers();
// test_sim_events.cpp (test_link_loss closes the pseudo-terminal, so it runs last)
void test_events_and_raw();
void test_link_loss();
// test_sim_control.cpp
void test_control();
void test_control_rules();
// test_sim_generic.cpp: Nodes of any class
void test_generic_nodes();
void test_byte_device();
#ifdef SIM_HIGH_SPEED
// test_sim_high_speed.cpp
void test_high_speed_rules();
#endif
#ifdef SIM_ISO
// test_sim_isochronous.cpp
void test_isochronous();
#endif

} // namespace sim
