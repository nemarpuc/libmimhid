// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: discovery, device lifetime and configuration, and control transfers.
 */

#include "api/support.hpp"
#include "transport/handshaker.hpp"
#include "transport/link.hpp"
#include "transport/serial_port.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace tr = mimusb::transport;

using mimusb::detail::before_close;
using mimusb::detail::command_error;
using mimusb::detail::commit_device;
using mimusb::detail::CommitWaits;
using mimusb::detail::deliver_event;
using mimusb::detail::describe_board;
using mimusb::detail::kCommandAttempts;
using mimusb::detail::kMaximumReportBytes;
using mimusb::detail::live_nodes;
using mimusb::detail::prepare_device_config;
using mimusb::detail::read_commit_timeouts;
using mimusb::detail::require_timeout;
using mimusb::detail::reserved_is_zero;
using mimusb::detail::set_error;
using mimusb::detail::validate_usb_identity;
using mimusb::detail::validate_usb_identity_on_board;

// The public header repeats the wire protocol's tables.
static_assert(MIMUSB_BOOT_BAUD == 115200U);
// The wire flag and the public one are the same bit.
static_assert(MIMUSB_UART_FLAG_BREAK == MIMUSB_PONG_UART_BREAK);
static_assert(MIMUSB_CHIP_RP2040 == 1 && MIMUSB_CHIP_RP2350 == 2 && MIMUSB_CHIP_ESP32S3 == 3 &&
              MIMUSB_CHIP_NRF52840 == 4 && MIMUSB_CHIP_STM32F4 == 5);
static_assert(sizeof(mimusb_device_info::unique_id) == MIMUSB_UNIQUE_ID_MAX);
static_assert(MIMUSB_CONTROL_MAX_TIMEOUT_MS == MIMUSB_CONTROL_TIMEOUT_MAX);
static_assert(static_cast<int>(MIMUSB_CONTROL_ANSWER) == MIMUSB_RULE_ANSWER &&
              static_cast<int>(MIMUSB_CONTROL_ANSWER_LAST_SENT) == MIMUSB_RULE_ANSWER_LAST_SENT &&
              static_cast<int>(MIMUSB_CONTROL_ACCEPT) == MIMUSB_RULE_ACCEPT &&
              static_cast<int>(MIMUSB_CONTROL_ASK) == MIMUSB_RULE_ASK &&
              static_cast<int>(MIMUSB_CONTROL_STALL) == MIMUSB_RULE_STALL);
static_assert(MIMUSB_EVENT_CONTROL_SETUP == MIMUSB_EV_CONTROL_SETUP &&
              MIMUSB_EVENT_CONTROL_DATA == MIMUSB_EV_CONTROL_DATA);
static_assert(MIMUSB_CONTROL_FLAG_HELD == MIMUSB_CONTROL_F_HELD &&
              MIMUSB_CONTROL_FLAG_TOO_LONG == MIMUSB_CONTROL_F_TOO_LONG &&
              MIMUSB_CONTROL_FLAG_FIXED == MIMUSB_CONTROL_F_FIXED &&
              MIMUSB_CONTROL_FLAG_LOST == MIMUSB_CONTROL_F_LOST &&
              MIMUSB_CONTROL_FLAG_ACCEPTED == MIMUSB_CONTROL_F_ACCEPTED);
static_assert(MIMUSB_CONTROL_ACTION_LEAVE == MIMUSB_REPLY_LEAVE + 1 &&
              MIMUSB_CONTROL_ACTION_ANSWER == MIMUSB_REPLY_ANSWER + 1 &&
              MIMUSB_CONTROL_ACTION_STALL == MIMUSB_REPLY_STALL + 1 &&
              MIMUSB_CONTROL_ACTION_DROP == MIMUSB_REPLY_DROP + 1);
// The largest report this library sends: a high-speed interrupt endpoint packet.

extern "C" {

// ---- discovery -------------------------------------------------------------------------

mimusb_result MIMUSB_CALL mimusb_discover(const std::uint32_t timeout_ms,
                                          mimusb_discovery** out_discovery) try {
    mimusb::detail::clear_error();
    if (out_discovery == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_discovery", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_discovery = nullptr;
    if (const mimusb_result t = require_timeout(timeout_ms, "timeout_ms"); t != MIMUSB_OK)
        return t;
    auto discovery = std::make_unique<mimusb_discovery>();
    std::vector<tr::PortCandidate> ports = tr::list_candidate_ports();
    std::sort(ports.begin(), ports.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    // Each port is searched on its own thread; a board's rate is measured and the board left
    // on the boot rate.
    std::vector<std::optional<mimusb_device_info>> found(ports.size());
    std::vector<std::thread> searches;
    searches.reserve(ports.size());
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].path.size() >= sizeof(mimusb_device_info::port_name))
            continue;
        searches.emplace_back([&, i] {
            tr::SerialPort port;
            if (!port.open(ports[i].path.c_str(), MIMUSB_BOOT_BAUD).ok)
                return;
            tr::Handshaker handshake(port, timeout_ms);
            tr::BoardInfo board;
            if (!handshake.find(&board))
                return;
            // A board of another version is listed with that version, to be flashed again.
            std::uint32_t fastest = handshake.rate();
            if (board.compatible) {
                fastest = handshake.fastest(&board);
                // A link that could not be measured is listed on the boot rate, not dropped.
                if (fastest == 0U) {
                    fastest = MIMUSB_BOOT_BAUD;
                    if (!handshake.to_boot(&board) && !handshake.find(&board))
                        return;
                } else if (!handshake.to_boot(&board)) {
                    return;
                }
            }
            found[i] = describe_board(ports[i].path, ports[i].description, board, fastest);
        });
    }
    for (std::thread& search : searches)
        search.join();
    for (const auto& entry : found) {
        if (entry)
            discovery->entries.push_back(*entry);
    }
    *out_discovery = discovery.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("discover")

std::size_t MIMUSB_CALL mimusb_discovery_count(const mimusb_discovery* discovery) try {
    mimusb::detail::clear_error();
    if (discovery == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "discovery", "The discovery handle is null.");
        return 0U;
    }
    return discovery->entries.size();
}
MIMUSB_C_VALUE_CATCH("discovery.count", 0U)

const mimusb_device_info* MIMUSB_CALL mimusb_discovery_get(const mimusb_discovery* discovery,
                                                           const std::size_t index) try {
    mimusb::detail::clear_error();
    if (discovery == nullptr || index >= discovery->entries.size()) {
        set_error(MIMUSB_ERR_PARAM, "discovery",
                  "A discovery handle and an index in range are required.");
        return nullptr;
    }
    return &discovery->entries[index];
}
MIMUSB_C_VALUE_CATCH("discovery.get", nullptr)

void MIMUSB_CALL mimusb_discovery_destroy(mimusb_discovery* discovery) try {
    mimusb::detail::clear_error();
    if (discovery == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "discovery", "The discovery handle is null.");
        return;
    }
    delete discovery;
}
MIMUSB_C_VOID_CATCH("discovery.destroy")

// ---- device ----------------------------------------------------------------------------

mimusb_result MIMUSB_CALL mimusb_device_open(const char* port_path,
                                             const mimusb_device_config* config,
                                             mimusb_device** out_device) try {
    mimusb::detail::clear_error();
    if (out_device == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "out_device", "The output pointer is null.");
        return MIMUSB_ERR_PARAM;
    }
    *out_device = nullptr;
    if (port_path == nullptr || port_path[0] == '\0') {
        set_error(MIMUSB_ERR_PARAM, "port_path", "A non-empty port_path is required.");
        return MIMUSB_ERR_PARAM;
    }
    mimusb_device_config effective{};
    if (const mimusb_result prepared = prepare_device_config(config, &effective);
        prepared != MIMUSB_OK) {
        return prepared;
    }

    auto device = std::make_unique<mimusb_device>();
    device->config = effective;
    device->manufacturer =
        effective.usb_device.manufacturer != nullptr ? effective.usb_device.manufacturer : "";
    device->product = effective.usb_device.product != nullptr ? effective.usb_device.product : "";
    device->serial = effective.usb_device.serial != nullptr ? effective.usb_device.serial : "";
    device->configuration =
        effective.usb_config.string != nullptr ? effective.usb_config.string : "";
    // The caller's string pointers are not retained.
    device->config.usb_device.manufacturer = device->config.usb_device.product = nullptr;
    device->config.usb_device.serial = nullptr;
    device->config.usb_config.string = nullptr;

    const tr::OpenResult opened = device->link.open(port_path);
    if (!opened.ok) {
        if (opened.step == tr::OpenStep::open) {
            set_error(MIMUSB_ERR_ACCESS, "port",
                      "Failed to open the serial port (it may be in use by another program).",
                      opened.os_error);
            return MIMUSB_ERR_ACCESS;
        }
        set_error(MIMUSB_ERR_IO, "port", "Failed to configure the serial port.", opened.os_error);
        return MIMUSB_ERR_IO;
    }
    // The board's own limits and the line rate come from talking to it.
    tr::Handshaker handshake(device->link.port(), device->config.transport.handshake_timeout_ms);
    if (!handshake.find(&device->board, effective.transport.baud_rate == MIMUSB_BAUD_FASTEST
                                            ? 0U
                                            : effective.transport.baud_rate)) {
        set_error(MIMUSB_ERR_TIMEOUT, "port",
                  "No board answered on this port, not even after a BREAK (check that the firmware "
                  "is from this libmimusb release, the wiring, and the port; a board a crashed "
                  "program left on a fast rate behind an adapter that cannot send a BREAK needs "
                  "its power cycled, or baud_rate set to the rate it was on).");
        return MIMUSB_ERR_TIMEOUT;
    }
    if (!device->board.compatible) {
        set_error(
            MIMUSB_ERR_PROTOCOL, "port",
            "The board's firmware has another major or minor version than this library: "
            "flash the firmware of this libmimusb release.",
            0, 0, 0, 0,
            static_cast<std::uint32_t>(MIMUSB_VERSION_MAJOR << 8 | MIMUSB_VERSION_MINOR),
            static_cast<std::uint32_t>(device->board.version[0] << 8 | device->board.version[1]));
        return MIMUSB_ERR_PROTOCOL;
    }
    std::uint32_t baud = effective.transport.baud_rate;
    tr::Handshake agreed = tr::Handshake::ok;
    if (baud == MIMUSB_BAUD_FASTEST) {
        baud = handshake.fastest(&device->board);
        if (baud == 0U)
            agreed = tr::Handshake::io_error;
    } else {
        agreed = handshake.switch_to(baud, &device->board);
    }
    effective.transport.baud_rate = baud;
    switch (agreed) {
    case tr::Handshake::ok:
        break;
    case tr::Handshake::rate_refused:
        set_error(MIMUSB_ERR_UNSUPPORTED, "baud_rate",
                  "The board cannot run at this rate, or the serial port refused to be set to it.");
        return MIMUSB_ERR_UNSUPPORTED;
    case tr::Handshake::rate_failed:
        set_error(MIMUSB_ERR_UNSUPPORTED, "baud_rate",
                  "The link did not carry this rate without loss: the serial adapter or the wiring "
                  "does not support it. MIMUSB_BAUD_FASTEST measures the fastest rate that works.");
        return MIMUSB_ERR_UNSUPPORTED;
    case tr::Handshake::no_board:
    case tr::Handshake::io_error:
        set_error(MIMUSB_ERR_IO, "port", "The board stopped answering while the rate was agreed.");
        return MIMUSB_ERR_IO;
    }

    if (effective.transport.maximum_report_bytes > device->board.max_report_bytes ||
        effective.transport.maximum_report_bytes > kMaximumReportBytes) {
        set_error(MIMUSB_ERR_OVERFLOW, "transport.maximum_report_bytes",
                  "The limit exceeds what the board accepts.");
        return MIMUSB_ERR_OVERFLOW;
    }
    if (const mimusb_result r = validate_usb_identity_on_board(device->board, effective.usb_device);
        r != MIMUSB_OK) {
        return r;
    }
    device->maximum_report_bytes = effective.transport.maximum_report_bytes;
    device->info = describe_board(port_path, "", device->board, effective.transport.baud_rate);

    mimusb_device* raw = device.get();
    device->link.start([raw](const tr::BoardEvent& event) { deliver_event(raw, event); });

    mimusb::detail::log_message(raw, MIMUSB_LOG_INFO, "device opened");
    *out_device = device.release();
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.open")

mimusb_result MIMUSB_CALL mimusb_device_set_usb_identity(
    mimusb_device* device, const mimusb_usb_device* usb_device, const mimusb_usb_config* usb_config,
    const mimusb_commit_timeouts* timeouts) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (usb_device == nullptr || usb_config == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "usb_identity", "usb_device and usb_config are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result r = validate_usb_identity(*usb_device, *usb_config); r != MIMUSB_OK)
        return r;
    if (const mimusb_result r = validate_usb_identity_on_board(device->board, *usb_device);
        r != MIMUSB_OK) {
        return r;
    }

    device->config.usb_device = *usb_device;
    device->config.usb_config = *usb_config;
    device->manufacturer = usb_device->manufacturer ? usb_device->manufacturer : "";
    device->product = usb_device->product ? usb_device->product : "";
    device->serial = usb_device->serial ? usb_device->serial : "";
    device->configuration = usb_config->string ? usb_config->string : "";
    device->config.usb_device.manufacturer = device->config.usb_device.product = nullptr;
    device->config.usb_device.serial = nullptr;
    device->config.usb_config.string = nullptr;
    device->dirty = true;
    if (!device->batch_open)
        return commit_device(device, 0U, waits);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.set_usb_identity")

mimusb_result MIMUSB_CALL mimusb_device_commit(mimusb_device* device, uint32_t reconnect_delay_ms,
                                               const mimusb_commit_timeouts* timeouts) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    const mimusb_result res = commit_device(device, reconnect_delay_ms, waits);
    if (res == MIMUSB_OK) {
        device->batch_open = false;
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        for (mimusb_node* node : device->slots) {
            if (node != nullptr)
                node->staged_only = false;
        }
    }
    return res;
}
MIMUSB_C_RESULT_CATCH("device.commit")

mimusb_result MIMUSB_CALL mimusb_device_connect(mimusb_device* device,
                                                const mimusb_commit_timeouts* timeouts) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (!device->configured)
        return commit_device(device, 0U, waits);
    // The board's USB device is on the target already (a disconnect or a detaching commit clears
    // `configured`). The board refuses a COMMIT that attaches while it is attached: it would change
    // descriptors under a device the target has enumerated.
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.connect")

mimusb_result MIMUSB_CALL mimusb_device_disconnect(mimusb_device* device,
                                                   const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    const mimusb_result r =
        command_error(device->link.disconnect(control_timeout_ms), "device.disconnect",
                      "The board did not acknowledge disconnect.", MIMUSB_T_COMMIT);
    if (r == MIMUSB_OK) {
        device->configured = false;
    }
    return r;
}
MIMUSB_C_RESULT_CATCH("device.disconnect")

mimusb_result MIMUSB_CALL mimusb_device_close(mimusb_device* device,
                                              const uint32_t control_timeout_ms,
                                              const uint32_t drain_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "device", "The Device handle is null.");
        return MIMUSB_ERR_PARAM;
    }
    // A refused timeout consumes nothing.
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    if (const mimusb_result t = require_timeout(drain_timeout_ms, "drain_timeout_ms");
        t != MIMUSB_OK)
        return t;
    if (device->closing.exchange(true, std::memory_order_acq_rel)) {
        set_error(MIMUSB_ERR_PARAM, "device", "The Device is already closing.");
        return MIMUSB_ERR_PARAM;
    }
    mimusb_result retained = MIMUSB_OK;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);
    for (mimusb_node* node : live_nodes(device)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::uint32_t budget =
            left.count() > 0 ? static_cast<std::uint32_t>(left.count()) : 1U;
        const mimusb_result neutral = before_close(node, budget);
        if (neutral != MIMUSB_OK && retained == MIMUSB_OK)
            retained = neutral;
    }
    if (!device->link.failed() && device->control_routed.load(std::memory_order_acquire)) {
        // The next program finds the board without the Device's own rules and with no reporting
        // of every transfer: only the rules of the Nodes stay.
        std::size_t kept = 0U;
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            kept =
                device->table.size() - std::min(device->table.size(), device->device_rules.size());
        }
        const std::uint8_t apply[2] = {0U, static_cast<std::uint8_t>(kept)};
        (void)device->link.command(MIMUSB_T_RULES_APPLY, apply, sizeof(apply), control_timeout_ms,
                                   1);
    }
    if (!device->link.failed() && device->loss_reported.load(std::memory_order_acquire)) {
        const std::uint8_t off[3] = {0U, 0U, 0U};
        (void)device->link.command(MIMUSB_T_LOSS_CONFIG, off, sizeof(off), control_timeout_ms, 1);
    }
    if (!device->link.failed() && device->configured &&
        device->config.transport.keep_attached == 0U) {
        const std::uint8_t detach[3] = {1U, 0U, 0U};
        const tr::CommandResult sent = device->link.command(MIMUSB_T_COMMIT, detach, sizeof(detach),
                                                            control_timeout_ms, kCommandAttempts);
        if (sent.status != tr::CommandStatus::ack && retained == MIMUSB_OK)
            retained = command_error(sent, "device.close", "The board did not go off the bus.",
                                     MIMUSB_T_COMMIT);
    }
    // The next program to open the board, or a search, finds it at once on the boot rate.
    if (!device->link.failed() && device->info.baud_rate != MIMUSB_BOOT_BAUD) {
        const std::uint8_t boot[4] = {static_cast<std::uint8_t>(MIMUSB_BOOT_BAUD & 0xFFU),
                                      static_cast<std::uint8_t>((MIMUSB_BOOT_BAUD >> 8) & 0xFFU),
                                      static_cast<std::uint8_t>((MIMUSB_BOOT_BAUD >> 16) & 0xFFU),
                                      static_cast<std::uint8_t>(MIMUSB_BOOT_BAUD >> 24)};
        (void)device->link.command(MIMUSB_T_SET_BAUD, boot, sizeof(boot), control_timeout_ms, 1);
    }
    if (device->link.failed() && retained == MIMUSB_OK) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device.close",
                  "The serial link was gone, so the board could not be reset.");
        retained = MIMUSB_ERR_NO_DEVICE;
    }
    mimusb::detail::stop_rule_updater(device); // it sends on the link
    device->link.drain();
    device->link.close();
    for (mimusb_node*& node : device->slots) {
        if (node != nullptr) {
            if (node->spec != nullptr)
                mimusb::detail::release_spec(node->spec);
            delete node;
            node = nullptr;
        }
    }
    mimusb::detail::log_message(device, MIMUSB_LOG_INFO, "device closed");
    delete device;
    if (retained == MIMUSB_OK)
        mimusb::detail::clear_error();
    return retained;
}
MIMUSB_C_RESULT_CATCH("device.close")

mimusb_result MIMUSB_CALL mimusb_device_latched_error(mimusb_device* device) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    const mimusb_result latched = device->latched.exchange(MIMUSB_OK, std::memory_order_acq_rel);
    if (latched != MIMUSB_OK)
        set_error(latched, "device", "A serial write failed earlier.");
    return latched;
}
MIMUSB_C_RESULT_CATCH("device.latched_error")

int32_t MIMUSB_CALL mimusb_device_is_ready(const mimusb_device* device) {
    return device != nullptr && device->link.ready() ? 1 : 0;
}

mimusb_result MIMUSB_CALL mimusb_device_wait_ready(mimusb_device* device,
                                                   const uint32_t timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->link.wait_ready(timeout_ms))
        return MIMUSB_OK;
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    set_error(MIMUSB_ERR_TIMEOUT, "device.wait_ready",
              "The target did not mount the device in time.");
    return MIMUSB_ERR_TIMEOUT;
}
MIMUSB_C_RESULT_CATCH("device.wait_ready")

mimusb_result MIMUSB_CALL mimusb_device_batch_begin(mimusb_device* device) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->batch_open) {
        set_error(MIMUSB_ERR_BUSY, "device.batch", "A batch is already open.");
        return MIMUSB_ERR_BUSY;
    }
    device->batch_open = true;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.batch_begin")

mimusb_result MIMUSB_CALL mimusb_device_batch_end(mimusb_device* device,
                                                  const mimusb_commit_timeouts* timeouts) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    CommitWaits waits;
    if (const mimusb_result r = read_commit_timeouts(timeouts, &waits); r != MIMUSB_OK)
        return r;
    if (!device->batch_open) {
        set_error(MIMUSB_ERR_BUSY, "device.batch", "No batch is open.");
        return MIMUSB_ERR_BUSY;
    }
    device->batch_open = false;
    if (!device->dirty)
        return MIMUSB_OK;
    const mimusb_result committed = commit_device(device, 0U, waits);
    if (committed != MIMUSB_OK) {
        device->batch_open = true; // staged changes stay; the caller may retry or close nodes
        return committed;
    }
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        for (mimusb_node* node : device->slots) {
            if (node != nullptr)
                node->staged_only = false;
        }
    }
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.batch_end")

mimusb_result MIMUSB_CALL mimusb_device_remote_wakeup(mimusb_device* device,
                                                      const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    const tr::CommandResult result = device->link.command(MIMUSB_T_REMOTE_WAKEUP, nullptr, 0,
                                                          control_timeout_ms, kCommandAttempts);
    return command_error(result, "device.remote_wakeup", "The target has not suspended the device.",
                         MIMUSB_T_REMOTE_WAKEUP);
}
MIMUSB_C_RESULT_CATCH("device.remote_wakeup")

mimusb_result MIMUSB_CALL mimusb_device_enter_bootloader(mimusb_device* device,
                                                         const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    static const std::uint8_t key[4] = {'B', 'O', 'O', 'T'};
    // Sent once: a board that restarted cannot answer a second frame.
    const tr::CommandResult result =
        device->link.command(MIMUSB_T_BOOTLOADER, key, sizeof(key), control_timeout_ms, 1);
    return command_error(result, "device.enter_bootloader",
                         "The board refused to enter its bootloader.", MIMUSB_T_BOOTLOADER);
}
MIMUSB_C_RESULT_CATCH("device.enter_bootloader")

mimusb_result MIMUSB_CALL mimusb_device_board_info(const mimusb_device* device,
                                                   mimusb_device_info* out_info) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire) ||
        out_info == nullptr) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device and an output pointer are required.");
        return MIMUSB_ERR_PARAM;
    }
    *out_info = device->info;
    out_info->is_ready = device->link.ready() ? 1U : 0U;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.board_info")

mimusb_result MIMUSB_CALL mimusb_device_link_counters(mimusb_device* device, const uint32_t reset,
                                                      const uint32_t control_timeout_ms,
                                                      mimusb_link_counters* out_counters) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire) ||
        out_counters == nullptr ||
        !mimusb::detail::valid_struct(out_counters, out_counters->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*out_counters)),
                                      "link_counters")) {
        set_error(MIMUSB_ERR_PARAM, "device",
                  "A live Device and an output with its struct_size set are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (!reserved_is_zero(out_counters->reserved, "link_counters.reserved"))
        return MIMUSB_ERR_PARAM;
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    tr::LinkCounters counters;
    const tr::CommandResult sent =
        device->link.counters(reset != 0U, control_timeout_ms, &counters);
    if (!sent.ok())
        return command_error(sent, "device.link_counters", "The board did not report its counters.",
                             MIMUSB_T_COUNTERS);
    out_counters->bad_crc = counters.bad_crc;
    out_counters->framing = counters.framing;
    out_counters->uart_overrun = counters.uart_overrun;
    out_counters->ring_overrun = counters.ring_overrun;
    out_counters->probes_good = counters.probes_good;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.link_counters")

mimusb_result MIMUSB_CALL mimusb_device_limits(const mimusb_device* device,
                                               mimusb_board_limits* out_limits) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire) ||
        out_limits == nullptr ||
        !mimusb::detail::valid_struct(out_limits, out_limits->struct_size,
                                      static_cast<std::uint32_t>(sizeof(*out_limits)),
                                      "board_limits")) {
        set_error(MIMUSB_ERR_PARAM, "device",
                  "A live Device and an output with its struct_size set are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (!reserved_is_zero(out_limits->reserved, "board_limits.reserved"))
        return MIMUSB_ERR_PARAM;
    out_limits->max_report_bytes = device->board.max_report_bytes;
    out_limits->max_feature_bytes = device->board.max_control_data_bytes;
    out_limits->max_endpoint_bytes = device->board.max_endpoint_bytes;
    out_limits->max_endpoint_number = device->board.max_endpoint_number;
    out_limits->max_frame_payload = device->board.max_frame_payload;
    out_limits->max_control_data_bytes = device->board.max_control_data_bytes;
    out_limits->max_control_rules = device->board.max_control_rules;
    out_limits->max_interfaces_any = device->board.max_interfaces_any;
    out_limits->max_answer_bytes = device->board.max_answer_bytes;
    out_limits->max_config_descriptor_bytes = device->board.max_config_descriptor_bytes;
    out_limits->max_strings = device->board.max_strings;
    out_limits->max_transfer_bytes = device->board.max_transfer_bytes;
    out_limits->endpoint_queue_slots = device->board.endpoint_queue_slots;
    out_limits->transfer_types = device->board.transfer_types;
    out_limits->max_bos_bytes = device->board.max_bos_bytes;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.limits")

// ---- strings -----------------------------------------------------------------------------

mimusb_result MIMUSB_CALL mimusb_device_add_string(mimusb_device* device, const char* utf8,
                                                   uint8_t* out_index) try {
    mimusb::detail::clear_error();
    if (out_index == nullptr || utf8 == nullptr || device == nullptr ||
        device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device.add_string",
                  "A live Device, text and an output pointer are required.");
        return MIMUSB_ERR_PARAM;
    }
    *out_index = 0U;
    if (!mimusb::detail::valid_usb_string(utf8)) {
        set_error(MIMUSB_ERR_PARAM, "device.add_string",
                  "The text must be UTF-8 without control characters, at most 126 UTF-16 code "
                  "units.");
        return MIMUSB_ERR_PARAM;
    }
    // The indices from 5 up; the names of interfaces take theirs at the commit from what is left
    // over, so this one is the lowest not given yet. The board's max_strings counts the four of
    // the Device too.
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    std::uint32_t index = 5U;
    while (index <= 255U && device->user_strings.count(static_cast<std::uint8_t>(index)) != 0U)
        ++index;
    if (index > 255U || device->user_strings.size() + 4U >= device->board.max_strings) {
        set_error(MIMUSB_ERR_OVERFLOW, "device.add_string",
                  "The board's string descriptors (max_strings) or the index space are used up.", 0,
                  0, 0, 0, device->board.max_strings, 0);
        return MIMUSB_ERR_OVERFLOW;
    }
    device->user_strings[static_cast<std::uint8_t>(index)] = utf8;
    device->dirty = true;
    *out_index = static_cast<std::uint8_t>(index);
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.add_string")

mimusb_result MIMUSB_CALL mimusb_device_remove_string(mimusb_device* device,
                                                      const uint8_t index) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device.remove_string", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    if (device->user_strings.erase(index) == 0U) {
        set_error(MIMUSB_ERR_PARAM, "device.remove_string",
                  "The index was not given by mimusb_device_add_string.");
        return MIMUSB_ERR_PARAM;
    }
    device->dirty = true;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("device.remove_string")

// ---- control transfers of the target ---------------------------------------------------

mimusb_result MIMUSB_CALL mimusb_device_set_control_rules(
    mimusb_device* device, const uint32_t report_all, const mimusb_control_rule* rules,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const size_t rule_count, const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    if (!mimusb::detail::valid_boolean(report_all)) {
        set_error(MIMUSB_ERR_PARAM, "report_all", "report_all must be 0 or 1.");
        return MIMUSB_ERR_PARAM;
    }
    std::vector<mimusb::usb::BoardRule> copied;
    if (const mimusb_result c =
            mimusb::detail::copy_control_rules(device, rules, rule_count, "rules", &copied);
        c != MIMUSB_OK)
        return c;
    const std::vector<mimusb::usb::BoardRule> old_rules = std::move(device->device_rules);
    const bool old_report_all = device->report_all;
    device->device_rules = std::move(copied);
    device->report_all = report_all != 0U;
    if (!device->configured) {
        device->control_routed.store(device->report_all || !device->device_rules.empty(),
                                     std::memory_order_release);
        return MIMUSB_OK; // they go with the first commit
    }
    if (device->batch_open) {
        device->dirty = true; // they go with the commit that ends the batch
        return MIMUSB_OK;
    }
    const mimusb_result result = mimusb::detail::rebuild_rules(device, control_timeout_ms);
    if (result != MIMUSB_OK) {
        device->device_rules = old_rules; // the board still holds the old table
        device->report_all = old_report_all;
    }
    return result;
}
MIMUSB_C_RESULT_CATCH("device.set_control_rules")

mimusb_result MIMUSB_CALL mimusb_device_set_rule_data(mimusb_device* device,
                                                      const size_t rule_index, const uint8_t* data,
                                                      const size_t length,
                                                      const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    return mimusb::detail::change_rule_data(device, nullptr, rule_index, data, length,
                                            control_timeout_ms);
}
MIMUSB_C_RESULT_CATCH("device.set_rule_data")

mimusb_result MIMUSB_CALL mimusb_device_set_loss_reporting(
    mimusb_device* device, const uint32_t enabled,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const uint32_t interval_ms, const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    if (!mimusb::detail::valid_boolean(enabled)) {
        set_error(MIMUSB_ERR_PARAM, "enabled", "enabled must be 0 or 1.");
        return MIMUSB_ERR_PARAM;
    }
    if (interval_ms > MIMUSB_LOSS_MAX_INTERVAL_MS || (enabled == 0U && interval_ms != 0U)) {
        set_error(MIMUSB_ERR_PARAM, "interval_ms",
                  "The interval is 0 to MIMUSB_LOSS_MAX_INTERVAL_MS, and 0 while disabled.");
        return MIMUSB_ERR_PARAM;
    }
    const std::uint8_t payload[3] = {static_cast<std::uint8_t>(enabled),
                                     static_cast<std::uint8_t>(interval_ms & 0xFFU),
                                     static_cast<std::uint8_t>(interval_ms >> 8)};
    const mimusb_result result =
        command_error(device->link.command(MIMUSB_T_LOSS_CONFIG, payload, sizeof(payload),
                                           control_timeout_ms, kCommandAttempts),
                      "device.set_loss_reporting", "The board did not accept the loss reporting.",
                      MIMUSB_T_LOSS_CONFIG);
    if (result == MIMUSB_OK)
        device->loss_reported.store(enabled != 0U, std::memory_order_release);
    return result;
}
MIMUSB_C_RESULT_CATCH("device.set_loss_reporting")

mimusb_result MIMUSB_CALL mimusb_device_control_reply(mimusb_device* device, const uint32_t token,
                                                      const mimusb_control_action action,
                                                      const uint8_t* data, const size_t length,
                                                      const uint32_t send_timeout_ms) try {
    mimusb::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, "device", "A live Device is required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(send_timeout_ms, "send_timeout_ms"); t != MIMUSB_OK)
        return t;
    if (token == 0U || token > 255U || action < MIMUSB_CONTROL_ACTION_LEAVE ||
        action > MIMUSB_CONTROL_ACTION_DROP ||
        (length != 0U && (data == nullptr || action != MIMUSB_CONTROL_ACTION_ANSWER))) {
        set_error(MIMUSB_ERR_PARAM, "reply",
                  "A token of 1 to 255, a known action, and data only with ANSWER are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (length > device->board.max_control_data_bytes) {
        set_error(MIMUSB_ERR_OVERFLOW, "reply", "The data is longer than the board takes.", 0,
                  MIMUSB_T_CONTROL_REPLY, 0, 0, 0, static_cast<std::uint32_t>(length));
        return MIMUSB_ERR_OVERFLOW;
    }
    std::int32_t os_error = 0;
    const tr::IoStatus status = device->link.send_control_reply(
        static_cast<std::uint8_t>(token), static_cast<std::uint8_t>(action - 1), data, length,
        send_timeout_ms, &os_error);
    switch (status) {
    case tr::IoStatus::ok:
        return MIMUSB_OK;
    case tr::IoStatus::timeout:
        set_error(MIMUSB_ERR_TIMEOUT, "reply", "The serial write made no progress in time.",
                  os_error, MIMUSB_T_CONTROL_REPLY);
        return MIMUSB_ERR_TIMEOUT;
    case tr::IoStatus::short_write:
        set_error(MIMUSB_ERR_SHORT_TRANSFER, "reply", "The serial write was cut short.", os_error,
                  MIMUSB_T_CONTROL_REPLY);
        return MIMUSB_ERR_SHORT_TRANSFER;
    case tr::IoStatus::closed:
        device->latched.store(MIMUSB_ERR_NO_DEVICE, std::memory_order_release);
        set_error(MIMUSB_ERR_NO_DEVICE, "reply", "The serial link failed or was unplugged.",
                  os_error, MIMUSB_T_CONTROL_REPLY);
        return MIMUSB_ERR_NO_DEVICE;
    case tr::IoStatus::error:
        break;
    }
    set_error(MIMUSB_ERR_IO, "reply", "The serial write failed.", os_error, MIMUSB_T_CONTROL_REPLY);
    return MIMUSB_ERR_IO;
}
MIMUSB_C_RESULT_CATCH("device.control_reply")

} // extern "C"
