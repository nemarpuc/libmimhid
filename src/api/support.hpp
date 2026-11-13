// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * Helpers shared by the translation units of the C ABI, and the macros that keep exceptions
 * from crossing it.
 */

#include "api/internal.hpp"
#include "usb/descriptor_composer.hpp"

#include <cstddef>
#include <cstdint>
#include <new>

#define MIMUSB_C_RESULT_CATCH(api_name)                                                            \
    catch (const std::bad_alloc&) {                                                                \
        ::mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
        return MIMUSB_ERR_INTERNAL;                                                                \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimusb::detail::set_error(                                                               \
            MIMUSB_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
        return MIMUSB_ERR_INTERNAL;                                                                \
    }

#define MIMUSB_C_VALUE_CATCH(api_name, fallback)                                                   \
    catch (const std::bad_alloc&) {                                                                \
        ::mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
        return fallback;                                                                           \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimusb::detail::set_error(                                                               \
            MIMUSB_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
        return fallback;                                                                           \
    }

#define MIMUSB_C_VOID_CATCH(api_name)                                                              \
    catch (const std::bad_alloc&) {                                                                \
        ::mimusb::detail::set_error(MIMUSB_ERR_INTERNAL, api_name,                                 \
                                    "Memory allocation failed inside the public C ABI call.");     \
    }                                                                                              \
    catch (...) {                                                                                  \
        ::mimusb::detail::set_error(                                                               \
            MIMUSB_ERR_INTERNAL, api_name,                                                         \
            "An internal C++ operation failed inside the public C ABI call.");                     \
    }

namespace mimusb::detail {

namespace tr = ::mimusb::transport;

// How often a configuration frame is sent before the board is given up on; each try waits for the
// timeout the caller passed.
constexpr int kCommandAttempts = 3;

// The largest report this library sends: a high-speed interrupt endpoint packet.
constexpr std::size_t kMaximumReportBytes = MIMUSB_HOST_MAX_REPORT_BYTES;

inline std::uint16_t public_node_id(const mimusb_node* node) noexcept {
    return node == nullptr ? std::uint16_t{0} : static_cast<std::uint16_t>(node->node_id + 1U);
}

// ---- device_ops.cpp: configuration checks, interface assignment, commit and board events ----

// UTF-8 to the UTF-16 a USB string descriptor carries (USB 2.0 9.6.7). False for malformed UTF-8,
// a control character or more than MIMUSB_STRING_MAX code units; null is the empty string.
bool utf16_from_utf8(const char* text, std::u16string* units);
bool valid_usb_string(const char* text) noexcept;

std::uint32_t saturate_u32(std::uint64_t value) noexcept;

bool reserved_is_zero(std::uint32_t reserved, const char* field) noexcept;

mimusb_result validate_usb_identity(const mimusb_usb_device& usb_device,
                                    const mimusb_usb_config& usb_config) noexcept;

mimusb_result validate_usb_identity_on_board(const tr::BoardInfo& board,
                                             const mimusb_usb_device& usb_device) noexcept;

mimusb_result prepare_device_config(const mimusb_device_config* config,
                                    mimusb_device_config* effective) noexcept;

// Maps a failed board command to a result and records the error.
mimusb_result command_error(const tr::CommandResult& result, const char* field, const char* reason,
                            std::uint8_t frame_type, std::uint16_t node_id = 0U) noexcept;

std::vector<mimusb_node*> live_nodes(mimusb_device* device);

// The most Nodes a Device holds: the node id plus one is the public id, and it is 16 bits.
constexpr std::size_t kMaximumNodes = 65534U;

// The lowest free node id, or false when the Device holds kMaximumNodes Nodes.
bool free_node_id(mimusb_device* device, std::uint16_t* node_id);
// Puts a Node in its slot (growing the table) and takes it out again.
void publish_node(mimusb_device* device, mimusb_node* node);
void unpublish_node(mimusb_device* device, const mimusb_node* node);

// The depth of the queue of a HID interface's IN endpoint: the least any live Node on it set.
std::uint8_t shared_depth(const std::vector<mimusb_node*>& nodes, std::uint8_t address);
// The runtime (count of transfers the board has not finished) of the IN endpoint `address` a Node
// writes to; null when the Node has no such endpoint.
EndpointRuntime* write_runtime(mimusb_node* node, std::uint8_t address);
// The frames of one transfer to an IN endpoint, waiting for room when the policy is WAIT. The
// caller has checked the link. Shared by mimusb_node_endpoint_write and the profile Nodes.
mimusb_result endpoint_write(mimusb_device* device, std::uint16_t node_id, EndpointRuntime* ep,
                             std::uint8_t address, const std::uint8_t* data, std::size_t length,
                             std::uint32_t timeout_ms);

// What the board takes, as the composer and the descriptor checks want it.
usb::ComposerLimits composer_limits(const mimusb_device* device);

// Zeroes the wait counts of every IN endpoint of the generic Nodes and wakes the writers.
void reset_endpoint_counts(mimusb_device* device);

// The checks of the composer on the interfaces of one Node alone (partial: no gap rule), made
// before the Node is staged. MIMUSB_OK, or the error, recorded.
mimusb_result precheck_groups(const mimusb_device* device, std::vector<usb::ComposerGroup> groups);

// A wait the caller passed to a call has no default: zero is refused.
mimusb_result require_timeout(std::uint32_t timeout_ms, const char* field) noexcept;

// The waits of a call that configures the board and re-enumerates its USB device.
struct CommitWaits {
    std::uint32_t control{};
    std::uint32_t reconnect{};
};

mimusb_result read_commit_timeouts(const mimusb_commit_timeouts* timeouts,
                                   CommitWaits* waits) noexcept;

mimusb_result commit_device(mimusb_device* device, std::uint32_t reconnect_delay_ms,
                            const CommitWaits& waits);

// ---- control rules (device_ops.cpp) ----

// Checks the caller's rules and copies them with their bytes.
mimusb_result copy_control_rules(const mimusb_device* device, const mimusb_control_rule* rules,
                                 std::size_t count, const char* field,
                                 std::vector<usb::BoardRule>* out);
// Sends the whole table again (Nodes' rules, the library's, the Device's) and makes it active.
mimusb_result rebuild_rules(mimusb_device* device, std::uint32_t control_timeout_ms);
// RULE_DATA for rule number `table_index` of the table, retried while the board is busy.
mimusb_result send_rule_data(mimusb_device* device, std::size_t table_index,
                             const std::uint8_t* data, std::size_t length,
                             std::uint32_t control_timeout_ms);
// mimusb_device_set_rule_data (node null) and mimusb_node_set_rule_data.
mimusb_result change_rule_data(mimusb_device* device, mimusb_node* node, std::size_t rule_index,
                               const std::uint8_t* data, std::size_t length,
                               std::uint32_t control_timeout_ms);
// Has the board answer `bytes` for the live rules of `owner` named `tag` on `iface` (the reader
// thread may call this; the updater thread sends them).
void set_live_answer(mimusb_device* device, const FunctionClass* owner, std::uint8_t tag,
                     std::uint8_t iface, const std::vector<std::uint8_t>& bytes);
void start_rule_updater(mimusb_device* device);
void stop_rule_updater(mimusb_device* device) noexcept;

void deliver_event(mimusb_device* device, const tr::BoardEvent& board) noexcept;

mimusb_device_info describe_board(const std::string& port, const std::string& description,
                                  const tr::BoardInfo& board, std::uint32_t baud) noexcept;

// What a Node's class needs done before it goes (mimusb_node_close, mimusb_device_close).
inline mimusb_result before_close(mimusb_node* node, const std::uint32_t timeout_ms) {
    return node->runtime != nullptr ? node->runtime->before_close(timeout_ms) : MIMUSB_OK;
}

} // namespace mimusb::detail
