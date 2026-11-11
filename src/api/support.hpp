// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * Helpers shared by the translation units of the C ABI, and the macros that keep exceptions
 * from crossing it.
 */

#include "api/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

#define MIMHID_C_RESULT_CATCH(api_name)                                                            \
    catch (const std::bad_alloc&) {                                                                \
        ::mimhid::detail::set_error(MIMHID_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
        return MIMHID_ERR_INTERNAL;                                                                \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimhid::detail::set_error(                                                               \
            MIMHID_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
        return MIMHID_ERR_INTERNAL;                                                                \
    }

#define MIMHID_C_VALUE_CATCH(api_name, fallback)                                                   \
    catch (const std::bad_alloc&) {                                                                \
        ::mimhid::detail::set_error(MIMHID_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
        return fallback;                                                                           \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimhid::detail::set_error(                                                               \
            MIMHID_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
        return fallback;                                                                           \
    }

#define MIMHID_C_VOID_CATCH(api_name)                                                              \
    catch (const std::bad_alloc&) {                                                                \
        ::mimhid::detail::set_error(MIMHID_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimhid::detail::set_error(                                                               \
            MIMHID_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
    }

namespace mimhid::detail {

namespace tr = ::mimhid::transport;

// How often a configuration frame is sent before the board is given up on; each try waits for the
// timeout the caller passed.
constexpr int kCommandAttempts = 3;

// The largest report this library sends: a high-speed interrupt endpoint packet.
constexpr std::size_t kMaximumReportBytes = MIMHID_HOST_MAX_REPORT_BYTES;

inline std::uint16_t report_id(const mimhid_node* node) noexcept {
    return node != nullptr && node->spec != nullptr && node->spec->layout.has_report_id
               ? node->spec->layout.report_id
               : 0U;
}

inline std::uint16_t public_node_id(const mimhid_node* node) noexcept {
    return node == nullptr ? std::uint16_t{0} : static_cast<std::uint16_t>(node->node_id + 1U);
}

// ---- device_ops.cpp: configuration checks, interface assignment, commit and board events ----

// UTF-8 to the UTF-16 a USB string descriptor carries (USB 2.0 9.6.7). False for malformed UTF-8,
// a control character or more than MIMHID_STRING_MAX code units; null is the empty string.
bool utf16_from_utf8(const char* text, std::u16string* units);
bool valid_usb_string(const char* text) noexcept;

std::uint32_t saturate_u32(std::uint64_t value) noexcept;

bool reserved_is_zero(std::uint32_t reserved, const char* field) noexcept;

mimhid_result validate_usb_identity(const mimhid_usb_device& usb_device,
                                    const mimhid_usb_config& usb_config) noexcept;

mimhid_result validate_usb_identity_on_board(const tr::BoardInfo& board,
                                             const mimhid_usb_device& usb_device) noexcept;

mimhid_result prepare_device_config(const mimhid_device_config* config,
                                    mimhid_device_config* effective) noexcept;

void collect_report_ids(const std::vector<std::uint8_t>& descriptor, std::bitset<256>* ids,
                        bool* has_ids);

// Maps a failed board command to a result and records the error.
mimhid_result command_error(const tr::CommandResult& result, const char* field, const char* reason,
                            std::uint8_t frame_type, std::uint16_t node_id = 0U) noexcept;

std::vector<mimhid_node*> live_nodes(mimhid_device* device);

// The most Nodes a Device holds: the node id plus one is the public id, and it is 16 bits.
constexpr std::size_t kMaximumNodes = 65534U;

// The lowest free node id, or false when the Device holds kMaximumNodes Nodes.
bool free_node_id(mimhid_device* device, std::uint16_t* node_id);
// Puts a Node in its slot (growing the table) and takes it out again.
void publish_node(mimhid_device* device, mimhid_node* node);
void unpublish_node(mimhid_device* device, const mimhid_node* node);

// The depth the board's queue of the Node's interface gets: the least depth any live Node on the
// interface asked for (they share the endpoint), 0 when none asked.
std::uint8_t interface_queue_depth(mimhid_device* device, std::uint8_t iface);

// A wait the caller passed to a call has no default: zero is refused.
mimhid_result require_timeout(std::uint32_t timeout_ms, const char* field) noexcept;

// The waits of a call that configures the board and re-enumerates its USB device.
struct CommitWaits {
    std::uint32_t control{};
    std::uint32_t reconnect{};
};

mimhid_result read_commit_timeouts(const mimhid_commit_timeouts* timeouts,
                                   CommitWaits* waits) noexcept;

mimhid_result commit_device(mimhid_device* device, std::uint32_t reconnect_delay_ms,
                            const CommitWaits& waits);

void deliver_event(mimhid_device* device, const tr::BoardEvent& board) noexcept;

mimhid_device_info describe_board(const std::string& port, const std::string& description,
                                  const tr::BoardInfo& board, std::uint32_t baud) noexcept;

// ---- c_api_node.cpp: the report state of a Node ----

bool has_non_neutral_state(const mimhid_node* node) noexcept;

void make_neutral(mimhid_node* node) noexcept;

bool touch_frame_in_progress(const mimhid_node* node) noexcept;

// ---- c_api_send.cpp: the serial write path ----

// Sends a node's pending reports until it is clean, for the close paths.
mimhid_result send_neutral(mimhid_node* node, std::uint32_t timeout_ms);

} // namespace mimhid::detail
