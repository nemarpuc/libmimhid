// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Public C ABI: the endpoints of a Node opened with mimhid_node_open (writing to an IN endpoint,
 * the Halt feature, flushing, the policy of the board's queue), and the wait policy's count.
 */

#include "api/support.hpp"

#include <algorithm>
#include <array>
#include <chrono>

namespace tr = mimhid::transport;

using mimhid::detail::command_error;
using mimhid::detail::EndpointRuntime;
using mimhid::detail::kCommandAttempts;
using mimhid::detail::public_node_id;
using mimhid::detail::require_timeout;
using mimhid::detail::set_error;

namespace {

enum class Direction { none, in, out };

// The direction of `address` among the endpoints of the Node: none when it has no such endpoint.
Direction endpoint_direction(const mimhid_node* node, const std::uint8_t address) {
    if (node->generic) {
        for (const mimhid::detail::GenericInterface& itf : node->interfaces) {
            for (const mimhid_usb_endpoint& ep : itf.endpoints) {
                if (ep.address == address)
                    return (address & 0x80U) != 0U ? Direction::in : Direction::out;
            }
        }
        return Direction::none;
    }
    if (node->config.ep_in.address == address)
        return Direction::in;
    if (node->config.ep_out.address != 0U && node->config.ep_out.address == address)
        return Direction::out;
    return Direction::none;
}

EndpointRuntime* runtime_of(mimhid_node* node, const std::uint8_t address) {
    for (EndpointRuntime& ep : node->in_endpoints) {
        if (ep.address == address)
            return &ep;
    }
    return nullptr;
}

// The checks every endpoint call starts with.
mimhid_result check_call(mimhid_node* node, const char* field) {
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, field, "A live Node is required.");
        return MIMHID_ERR_PARAM;
    }
    return MIMHID_OK;
}

mimhid_result link_ready(mimhid_node* node, const char* field) {
    mimhid_device* device = node->device;
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, field, "The serial link is gone.", 0, 0,
                  public_node_id(node));
        return MIMHID_ERR_NO_DEVICE;
    }
    if (node->staged_only || device->batch_open ||
        device->reconfiguring.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_BUSY, field,
                  "The board has not applied this Node's configuration yet (batch open or "
                  "re-enumeration in progress).",
                  0, 0, public_node_id(node));
        return MIMHID_ERR_BUSY;
    }
    return MIMHID_OK;
}

void reset_count(mimhid_device* device, EndpointRuntime* ep) {
    {
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        ep->outstanding = 0U;
        ++ep->epoch;
    }
    device->endpoint_cv.notify_all();
}

mimhid_result stall_command(mimhid_node* node, const std::uint8_t address, const bool stall,
                            const std::uint32_t timeout_ms, const char* field) {
    mimhid::detail::clear_error();
    if (const mimhid_result r = check_call(node, field); r != MIMHID_OK)
        return r;
    if (!node->generic) {
        set_error(MIMHID_ERR_UNSUPPORTED, field,
                  "The Halt feature of a HID Node's endpoint is not offered in this release.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (endpoint_direction(node, address) == Direction::none) {
        set_error(MIMHID_ERR_PARAM, field, "The address is not an endpoint of this Node.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(timeout_ms, "control_timeout_ms"); t != MIMHID_OK)
        return t;
    if (const mimhid_result r = link_ready(node, field); r != MIMHID_OK)
        return r;
    mimhid_device* device = node->device;
    const std::uint8_t payload[2] = {address, stall ? std::uint8_t{1} : std::uint8_t{0}};
    const mimhid_result r = command_error(
        device->link.command(MIMHID_T_EP_STALL, payload, sizeof(payload), timeout_ms,
                             kCommandAttempts),
        field, "The board did not accept the request.", MIMHID_T_EP_STALL, public_node_id(node));
    if (r == MIMHID_OK) {
        if (EndpointRuntime* ep = runtime_of(node, address))
            reset_count(device, ep);
    }
    return r;
}

} // namespace

namespace mimhid::detail {

// The depth of the queue of a HID interface's IN endpoint: the least any live Node on it set.
std::uint8_t hid_interface_depth(mimhid_device* device, const std::uint8_t iface) {
    std::uint8_t depth = 0U;
    for (const mimhid_node* node : live_nodes(device)) {
        if (node->generic || node->iface != iface)
            continue;
        const std::uint8_t own = node->config.ep_in.policy.depth;
        depth = depth == 0U ? own : std::min(depth, own);
    }
    return depth;
}

} // namespace mimhid::detail

extern "C" {

mimhid_result MIMHID_CALL mimhid_node_endpoint_write(mimhid_node* node, const uint8_t address,
                                                     const uint8_t* data, const size_t length,
                                                     const uint32_t timeout_ms) try {
    mimhid::detail::clear_error();
    if (const mimhid_result r = check_call(node, "node.endpoint.write"); r != MIMHID_OK)
        return r;
    if (!node->generic) {
        set_error(MIMHID_ERR_UNSUPPORTED, "node.endpoint.write",
                  "A HID Node sends reports with its profile calls or mimhid_report_send.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    EndpointRuntime* ep = runtime_of(node, address);
    if (ep == nullptr || (data == nullptr && length != 0U)) {
        set_error(MIMHID_ERR_PARAM, "node.endpoint.write",
                  "The address must be an IN endpoint of the Node; data is null only with length "
                  "0.",
                  0, 0, public_node_id(node), 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    mimhid_device* device = node->device;
    const bool waits = ep->policy.on_full == MIMHID_FULL_WAIT;
    if (waits && timeout_ms == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, "timeout_ms",
                  "A write to an endpoint with the wait policy needs a timeout.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (const mimhid_result r = link_ready(node, "node.endpoint.write"); r != MIMHID_OK)
        return r;

    // Frames: a multiple of the packet size each, except the last, so the target sees the
    // transfer it would have seen whole. The library adds no zero-length packet.
    const std::size_t largest = std::max<std::size_t>(device->board.max_transfer_bytes, 1U);
    const std::size_t packet = std::max<std::size_t>(ep->max_packet_size, 1U);
    const std::size_t chunk = largest >= packet ? largest - largest % packet : largest;
    const std::size_t frames = length == 0U ? 1U : (length + chunk - 1U) / chunk;
    if (frames > 65535U) {
        set_error(MIMHID_ERR_OVERFLOW, "node.endpoint.write",
                  "The transfer needs more than 65535 frames.", 0, 0, public_node_id(node), 0,
                  65535U, static_cast<std::uint32_t>(std::min<std::size_t>(frames, 0xFFFFFFFFU)));
        return MIMHID_ERR_OVERFLOW;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    static const std::uint8_t kEmpty[1] = {0};
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const std::size_t offset = frame * chunk;
        const std::size_t size = length == 0U ? 0U : std::min(chunk, length - offset);
        const bool sent_any = frame != 0U;
        std::uint64_t epoch = 0U;
        if (waits) {
            std::unique_lock<std::mutex> lock(device->endpoint_mutex);
            epoch = ep->epoch;
            const std::uint32_t depth = ep->policy.depth;
            while (ep->epoch == epoch && ep->outstanding >= depth) {
                if (device->link.failed()) {
                    set_error(MIMHID_ERR_NO_DEVICE, "node.endpoint.write",
                              "The serial link is gone.", 0, 0, public_node_id(node));
                    return MIMHID_ERR_NO_DEVICE;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    const mimhid_result r =
                        sent_any ? MIMHID_ERR_SHORT_TRANSFER : MIMHID_ERR_TIMEOUT;
                    set_error(r, "node.endpoint.write",
                              sent_any ? "The wait ended after some frames of the transfer had "
                                         "been sent; the target gets a short transfer."
                                       : "The board's queue stayed full until the timeout; "
                                         "nothing was sent.",
                              0, 0, public_node_id(node), 0, address, static_cast<std::uint32_t>(offset));
                    return r;
                }
                // Slices, so a link that dies while waiting is noticed.
                device->endpoint_cv.wait_until(
                    lock, std::min(deadline, now + std::chrono::milliseconds(50)));
            }
            if (ep->epoch != epoch) {
                const mimhid_result r = sent_any ? MIMHID_ERR_SHORT_TRANSFER : MIMHID_ERR_BUSY;
                set_error(r, "node.endpoint.write",
                          "The endpoint was flushed, stalled, reconfigured or unmounted while "
                          "the write waited; the rest was not sent.",
                          0, 0, public_node_id(node), 0, address,
                          static_cast<std::uint32_t>(offset));
                return r;
            }
            ++ep->outstanding;
        }
        std::int32_t os_error = 0;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const std::uint32_t stall_ms =
            waits ? static_cast<std::uint32_t>(std::max<std::int64_t>(left.count(), 1))
                  : (timeout_ms == 0U ? 1U : timeout_ms);
        const tr::IoStatus status = device->link.send_report(
            address, size == 0U ? kEmpty : data + offset, size, stall_ms, &os_error);
        if (status == tr::IoStatus::ok)
            continue;
        if (waits) {
            // Nothing (or not all) of this frame reached the board: it will not report it done.
            std::lock_guard<std::mutex> lock(device->endpoint_mutex);
            if (ep->epoch == epoch && ep->outstanding > 0U)
                --ep->outstanding;
        }
        mimhid_result r = MIMHID_ERR_IO;
        switch (status) {
        case tr::IoStatus::timeout:
            r = sent_any ? MIMHID_ERR_SHORT_TRANSFER : MIMHID_ERR_TIMEOUT;
            break;
        case tr::IoStatus::short_write:
            r = MIMHID_ERR_SHORT_TRANSFER;
            break;
        case tr::IoStatus::closed:
            r = MIMHID_ERR_NO_DEVICE;
            device->latched.store(MIMHID_ERR_NO_DEVICE, std::memory_order_release);
            break;
        default:
            break;
        }
        set_error(r, "node.endpoint.write", "The serial write of a frame did not complete.",
                  os_error, 0, public_node_id(node), 0, address,
                  static_cast<std::uint32_t>(offset));
        return r;
    }
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.endpoint.write")

mimhid_result MIMHID_CALL mimhid_node_endpoint_stall(mimhid_node* node, const uint8_t address,
                                                     const uint32_t control_timeout_ms) try {
    return stall_command(node, address, true, control_timeout_ms, "node.endpoint.stall");
}
MIMHID_C_RESULT_CATCH("node.endpoint.stall")

mimhid_result MIMHID_CALL mimhid_node_endpoint_clear_stall(
    mimhid_node* node, const uint8_t address, const uint32_t control_timeout_ms) try {
    return stall_command(node, address, false, control_timeout_ms, "node.endpoint.clear_stall");
}
MIMHID_C_RESULT_CATCH("node.endpoint.clear_stall")

mimhid_result MIMHID_CALL mimhid_node_endpoint_flush(mimhid_node* node, const uint8_t address,
                                                     const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    const char* field = "node.endpoint.flush";
    if (const mimhid_result r = check_call(node, field); r != MIMHID_OK)
        return r;
    if (!node->generic) {
        set_error(MIMHID_ERR_UNSUPPORTED, field,
                  "Flushing the endpoint of a HID Node is not offered in this release.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    EndpointRuntime* ep = runtime_of(node, address);
    if (ep == nullptr) {
        set_error(MIMHID_ERR_PARAM, field, "The address must be an IN endpoint of the Node.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (const mimhid_result r = link_ready(node, field); r != MIMHID_OK)
        return r;
    const std::uint8_t payload[1] = {address};
    const mimhid_result r = command_error(
        node->device->link.command(MIMHID_T_EP_FLUSH, payload, sizeof(payload), control_timeout_ms,
                                   kCommandAttempts),
        field, "The board did not accept the request.", MIMHID_T_EP_FLUSH, public_node_id(node));
    if (r == MIMHID_OK)
        reset_count(node->device, ep);
    return r;
}
MIMHID_C_RESULT_CATCH("node.endpoint.flush")

mimhid_result MIMHID_CALL mimhid_node_endpoint_set_policy(
    mimhid_node* node, const uint8_t address, const mimhid_endpoint_policy* policy,
    const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    const char* field = "node.endpoint.set_policy";
    if (const mimhid_result r = check_call(node, field); r != MIMHID_OK)
        return r;
    if (policy == nullptr || endpoint_direction(node, address) != Direction::in) {
        set_error(MIMHID_ERR_PARAM, field,
                  "A policy and the address of an IN endpoint of the Node are required.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (policy->on_full == 0U || policy->depth == 0U) {
        set_error(MIMHID_ERR_UNSET_FIELD, field, "on_full and depth are required.");
        return MIMHID_ERR_UNSET_FIELD;
    }
    if (policy->on_full > MIMHID_FULL_WAIT) {
        set_error(MIMHID_ERR_PARAM, field,
                  "on_full is MIMHID_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.");
        return MIMHID_ERR_PARAM;
    }
    if (!node->generic && policy->on_full != MIMHID_FULL_DROP_NEWEST) {
        set_error(MIMHID_ERR_UNSUPPORTED, field,
                  "The IN endpoint of a HID Node takes MIMHID_FULL_DROP_NEWEST only in this "
                  "release.");
        return MIMHID_ERR_UNSUPPORTED;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    mimhid_device* device = node->device;
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, field, "The serial link is gone.");
        return MIMHID_ERR_NO_DEVICE;
    }
    const bool live = device->configured && !device->batch_open && !node->staged_only &&
                      !device->reconfiguring.load(std::memory_order_acquire);
    if (node->generic) {
        EndpointRuntime* ep = runtime_of(node, address);
        if (live) {
            const mimhid_result r = command_error(
                device->link.command(MIMHID_T_EP_CONFIG,
                                     std::array<std::uint8_t, 3>{address, policy->depth,
                                                                 policy->on_full}
                                         .data(),
                                     3, control_timeout_ms, kCommandAttempts),
                field, "The board did not accept the policy.", MIMHID_T_EP_CONFIG,
                public_node_id(node));
            if (r != MIMHID_OK)
                return r;
        }
        {
            std::lock_guard<std::mutex> lock(device->endpoint_mutex);
            ep->policy = *policy;
            ep->outstanding = 0U;
            ++ep->epoch;
        }
        device->endpoint_cv.notify_all();
        // The copy the composer reads at the next commit.
        for (mimhid::detail::GenericInterface& itf : node->interfaces) {
            for (mimhid_usb_endpoint& e : itf.endpoints) {
                if (e.address == address)
                    e.policy = *policy;
            }
        }
        device->dirty = device->dirty || !live;
        return MIMHID_OK;
    }
    const std::uint8_t previous = node->config.ep_in.policy.depth;
    node->config.ep_in.policy = *policy;
    if (live) {
        const std::uint8_t depth = mimhid::detail::hid_interface_depth(device, node->iface);
        const std::uint8_t payload[3] = {address, depth, MIMHID_EP_FULL_DROP_NEWEST};
        const mimhid_result r = command_error(
            device->link.command(MIMHID_T_EP_CONFIG, payload, sizeof(payload), control_timeout_ms,
                                 kCommandAttempts),
            field, "The board did not accept the queue depth.", MIMHID_T_EP_CONFIG,
            public_node_id(node));
        if (r != MIMHID_OK) {
            node->config.ep_in.policy.depth = previous; // the board keeps the depth it had
            return r;
        }
    }
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("node.endpoint.set_policy")

} // extern "C"
