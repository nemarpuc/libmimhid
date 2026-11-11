// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* What the simulated-board scenarios share: the pseudo-terminal the library and the simulated board
 * talk over, the event sink and its record, the waits every call takes, and the specs and
 * options the scenarios start from. Test-only. */
#pragma once

#include "fake_board.h"
#include "mimhid.h"
#include "test.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifndef SIM_HID_ITFS
#define SIM_HID_ITFS 7
#endif

namespace sim {

// The waits every call takes; the library keeps none.
inline constexpr std::uint32_t kControlMs = 500;
inline constexpr std::uint32_t kSendMs = 500;
inline constexpr std::uint32_t kReconnectMs = 3000;
inline constexpr std::uint32_t kDrainMs = 1000;
inline const mimhid_commit_timeouts kCommit = {sizeof(mimhid_commit_timeouts), 0, kControlMs,
                                               kReconnectMs};

// The board end (master) and the library end (slave path) of the pseudo-terminal.
extern int g_master;
extern std::string g_slave;
bool open_pty();

// ---- events -------------------------------------------------------------------------
struct EventRecord {
    mimhid_event_kind kind;
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
    mimhid_device* device{nullptr};
    mimhid_control_action action{MIMHID_CONTROL_ACTION_ANSWER};
    std::vector<std::uint8_t> data;
    std::atomic<int> result{-1};
};
extern SinkReply g_sink_reply;

void MIMHID_CALL on_event(void*, const mimhid_event* event);
bool wait_event(mimhid_event_kind kind, EventRecord* out, int timeout_ms = 2000);

// ---- helpers ------------------------------------------------------------------------
mimhid_device_config base_options();
mimhid_device* open_device(const mimhid_device_config& options);
mimhid_spec* keyboard_spec(std::uint8_t report_id);
mimhid_spec* mouse_spec(std::uint8_t report_id);
mimhid_spec* touch_spec(std::uint8_t report_id, std::uint32_t feature);
mimhid_spec* pen_spec(std::uint8_t report_id);
// A raw spec with one 1-byte Input report and no Report ID.
mimhid_spec* raw_spec_no_id();
// A raw spec with an Output and a Feature report, both under Report ID 5.
mimhid_spec* raw_spec_io();
mimhid_node* open_node(mimhid_device* device, mimhid_spec* spec,
                       const mimhid_hid_node_config* options = nullptr);
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
// test_sim_generic.cpp: Nodes of any class
void test_generic_nodes();
#ifdef SIM_HIGH_SPEED
// test_sim_high_speed.cpp
void test_high_speed_rules();
#endif

} // namespace sim
