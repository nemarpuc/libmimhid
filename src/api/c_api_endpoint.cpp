// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Public C ABI: the endpoints of a Node of a mimusb_spec_create_interfaces Spec (writing to an IN
 * endpoint, the Halt feature, flushing, the policy of the board's queue), and the wait policy's
 * count.
 */

#include "api/support.hpp"

#include <algorithm>
#include <array>
#include <chrono>

namespace tr = mimusb::transport;

using mimusb::detail::command_error;
using mimusb::detail::EndpointRuntime;
using mimusb::detail::kCommandAttempts;
using mimusb::detail::public_node_id;
using mimusb::detail::require_timeout;
using mimusb::detail::set_error;

namespace {

enum class Direction { none, in, out };

// The direction of `address` among the endpoints of the Node: none when it has no such endpoint.
Direction endpoint_direction(const mimusb_node* node, const std::uint8_t address) {
    if (node->generic) {
        bool found = false;
        for (const mimusb::detail::GenericInterface& itf : node->interfaces) {
            itf.for_each_endpoint([&](const mimusb_usb_endpoint& ep) {
                if (ep.address == address)
                    found = true;
            });
        }
        if (!found)
            return Direction::none;
        return (address & 0x80U) != 0U ? Direction::in : Direction::out;
    }
    if (node->config.ep_in.address == address)
        return Direction::in;
    if (node->config.ep_out.address != 0U && node->config.ep_out.address == address)
        return Direction::out;
    return Direction::none;
}

EndpointRuntime* runtime_of(mimusb_node* node, const std::uint8_t address) {
    for (EndpointRuntime& ep : node->in_endpoints) {
        if (ep.address == address)
            return &ep;
    }
    return nullptr;
}

// The checks every endpoint call starts with.
mimusb_result check_call(mimusb_node* node, const char* field) {
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_PARAM, field, "A live Node is required.");
        return MIMUSB_ERR_PARAM;
    }
    return MIMUSB_OK;
}

mimusb_result link_ready(mimusb_node* node, const char* field) {
    mimusb_device* device = node->device;
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, field, "The serial link is gone.", 0, 0,
                  public_node_id(node));
        return MIMUSB_ERR_NO_DEVICE;
    }
    if (node->staged_only || device->batch_open ||
        device->reconfiguring.load(std::memory_order_acquire)) {
        set_error(MIMUSB_ERR_BUSY, field,
                  "The board has not applied this Node's configuration yet (batch open or "
                  "re-enumeration in progress).",
                  0, 0, public_node_id(node));
        return MIMUSB_ERR_BUSY;
    }
    return MIMUSB_OK;
}

void reset_count(mimusb_device* device, EndpointRuntime* ep) {
    {
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        ep->outstanding = 0U;
        ++ep->epoch;
    }
    device->endpoint_cv.notify_all();
}

mimusb_result stall_command(mimusb_node* node, const std::uint8_t address, const bool stall,
                            const std::uint32_t timeout_ms, const char* field) {
    mimusb::detail::clear_error();
    if (const mimusb_result r = check_call(node, field); r != MIMUSB_OK)
        return r;
    if (endpoint_direction(node, address) == Direction::none) {
        set_error(MIMUSB_ERR_PARAM, field, "The address is not an endpoint of this Node.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(timeout_ms, "control_timeout_ms"); t != MIMUSB_OK)
        return t;
    if (const mimusb_result r = link_ready(node, field); r != MIMUSB_OK)
        return r;
    mimusb_device* device = node->device;
    const std::uint8_t payload[2] = {address, stall ? std::uint8_t{1} : std::uint8_t{0}};
    const mimusb_result r = command_error(
        device->link.command(MIMUSB_T_EP_STALL, payload, sizeof(payload), timeout_ms,
                             kCommandAttempts),
        field, "The board did not accept the request.", MIMUSB_T_EP_STALL, public_node_id(node));
    if (r == MIMUSB_OK) {
        if (EndpointRuntime* ep = runtime_of(node, address))
            reset_count(device, ep);
    }
    return r;
}

} // namespace

namespace mimusb::detail {

// The depth of the queue of a HID interface's IN endpoint: the least any live Node on it set.
std::uint8_t hid_interface_depth(mimusb_device* device, const std::uint8_t iface) {
    std::uint8_t depth = 0U;
    for (const mimusb_node* node : live_nodes(device)) {
        if (node->generic || node->iface != iface)
            continue;
        const std::uint8_t own = node->config.ep_in.policy.depth;
        depth = depth == 0U ? own : std::min(depth, own);
    }
    return depth;
}

} // namespace mimusb::detail

namespace mimusb::detail {

// The runtime of the IN endpoint a Node writes to: its own; for Nodes sharing a profile interface
// with the wait policy the one the first live Node of it keeps (the board has one queue for the
// interface, and its count of finished transfers goes to every Node's runtime of that address,
// which each clamp at zero).
EndpointRuntime* write_runtime(mimusb_node* node, const std::uint8_t address) {
    EndpointRuntime* own = nullptr;
    for (EndpointRuntime& ep : node->in_endpoints) {
        if (ep.address == address)
            own = &ep;
    }
    // Only a wait policy counts transfers, so only it needs the one count of the interface; the
    // other policies keep the report path free of the walk (and its lock).
    if (own != nullptr && !node->generic && own->policy.on_full == MIMUSB_FULL_WAIT) {
        for (mimusb_node* other : live_nodes(node->device)) {
            if (!other->generic && other->iface == node->iface && !other->in_endpoints.empty())
                return &other->in_endpoints.front();
        }
    }
    return own;
}

// One transfer to an IN endpoint in frames, as mimusb_node_endpoint_write describes; the shared
// path of the generic Nodes and of the reports of the profile Nodes.
mimusb_result endpoint_write(mimusb_device* device, const std::uint16_t node_id,
                             EndpointRuntime* ep, const std::uint8_t address,
                             // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                             const std::uint8_t* data, const std::size_t length,
                             const std::uint32_t timeout_ms) {
    const bool waits = ep->policy.on_full == MIMUSB_FULL_WAIT;
    if (waits && timeout_ms == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "timeout_ms",
                  "A write to an endpoint with the wait policy needs a timeout.");
        return MIMUSB_ERR_UNSET_FIELD;
    }

    // An isochronous transfer is one packet, sent in one frame of the bus (USB 2.0 5.6): the
    // library does not split it into several, which would be several frames of the bus.
    if (ep->isochronous && length > ep->max_packet_size) {
        set_error(MIMUSB_ERR_PARAM, "node.endpoint.write",
                  "An isochronous transfer is one packet: at most the endpoint's packet size "
                  "(the largest of its settings); that size is in length.",
                  0, 0, node_id, 0, address, ep->max_packet_size);
        return MIMUSB_ERR_PARAM;
    }
    // Frames: a multiple of the packet size each, except the last, so the target sees the
    // transfer it would have seen whole. The library adds no zero-length packet.
    const std::size_t largest = std::max<std::size_t>(device->board.max_transfer_bytes, 1U);
    const std::size_t packet = std::max<std::size_t>(ep->max_packet_size, 1U);
    const std::size_t chunk = largest >= packet ? largest - largest % packet : largest;
    const std::size_t frames = length == 0U ? 1U : (length + chunk - 1U) / chunk;
    if (frames > 65535U) {
        set_error(MIMUSB_ERR_OVERFLOW, "node.endpoint.write",
                  "The transfer needs more than 65535 frames.", 0, 0, node_id, 0, 65535U,
                  static_cast<std::uint32_t>(std::min<std::size_t>(frames, 0xFFFFFFFFU)));
        return MIMUSB_ERR_OVERFLOW;
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
                    set_error(MIMUSB_ERR_NO_DEVICE, "node.endpoint.write",
                              "The serial link is gone.", 0, 0, node_id);
                    return MIMUSB_ERR_NO_DEVICE;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    const mimusb_result r =
                        sent_any ? MIMUSB_ERR_SHORT_TRANSFER : MIMUSB_ERR_TIMEOUT;
                    set_error(r, "node.endpoint.write",
                              sent_any ? "The wait ended after some frames of the transfer had "
                                         "been sent; the target gets a short transfer."
                                       : "The board's queue stayed full until the timeout; "
                                         "nothing was sent.",
                              0, 0, node_id, 0, address, static_cast<std::uint32_t>(offset));
                    return r;
                }
                // Slices, so a link that dies while waiting is noticed.
                device->endpoint_cv.wait_until(
                    lock, std::min(deadline, now + std::chrono::milliseconds(50)));
            }
            if (ep->epoch != epoch) {
                const mimusb_result r = sent_any ? MIMUSB_ERR_SHORT_TRANSFER : MIMUSB_ERR_BUSY;
                set_error(r, "node.endpoint.write",
                          "The endpoint was flushed, stalled, reconfigured or unmounted while "
                          "the write waited; the rest was not sent.",
                          0, 0, node_id, 0, address, static_cast<std::uint32_t>(offset));
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
        mimusb_result r = MIMUSB_ERR_IO;
        switch (status) {
        case tr::IoStatus::timeout:
            r = sent_any ? MIMUSB_ERR_SHORT_TRANSFER : MIMUSB_ERR_TIMEOUT;
            break;
        case tr::IoStatus::short_write:
            r = MIMUSB_ERR_SHORT_TRANSFER;
            break;
        case tr::IoStatus::closed:
            r = MIMUSB_ERR_NO_DEVICE;
            device->latched.store(MIMUSB_ERR_NO_DEVICE, std::memory_order_release);
            break;
        default:
            break;
        }
        set_error(r, "node.endpoint.write", "The serial write of a frame did not complete.",
                  os_error, 0, node_id, 0, address, static_cast<std::uint32_t>(offset));
        return r;
    }
    return MIMUSB_OK;
}

} // namespace mimusb::detail

extern "C" {

mimusb_result MIMUSB_CALL mimusb_node_endpoint_write(mimusb_node* node, const uint8_t address,
                                                     const uint8_t* data, const size_t length,
                                                     const uint32_t timeout_ms) try {
    mimusb::detail::clear_error();
    if (const mimusb_result r = check_call(node, "node.endpoint.write"); r != MIMUSB_OK)
        return r;
    EndpointRuntime* ep = mimusb::detail::write_runtime(node, address);
    if (ep == nullptr || (data == nullptr && length != 0U)) {
        set_error(MIMUSB_ERR_PARAM, "node.endpoint.write",
                  "The address must be an IN endpoint of the Node; data is null only with length "
                  "0.",
                  0, 0, public_node_id(node), 0, address, 0);
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result r = link_ready(node, "node.endpoint.write"); r != MIMUSB_OK)
        return r;
    return mimusb::detail::endpoint_write(node->device, public_node_id(node), ep, address, data,
                                          length, timeout_ms);
}
MIMUSB_C_RESULT_CATCH("node.endpoint.write")

mimusb_result MIMUSB_CALL mimusb_node_endpoint_stall(mimusb_node* node, const uint8_t address,
                                                     const uint32_t control_timeout_ms) try {
    return stall_command(node, address, true, control_timeout_ms, "node.endpoint.stall");
}
MIMUSB_C_RESULT_CATCH("node.endpoint.stall")

mimusb_result MIMUSB_CALL mimusb_node_endpoint_clear_stall(mimusb_node* node, const uint8_t address,
                                                           const uint32_t control_timeout_ms) try {
    return stall_command(node, address, false, control_timeout_ms, "node.endpoint.clear_stall");
}
MIMUSB_C_RESULT_CATCH("node.endpoint.clear_stall")

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimusb_result MIMUSB_CALL mimusb_node_endpoint_flush(mimusb_node* node, const uint8_t address,
                                                     const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    const char* field = "node.endpoint.flush";
    if (const mimusb_result r = check_call(node, field); r != MIMUSB_OK)
        return r;
    EndpointRuntime* ep = runtime_of(node, address);
    if (ep == nullptr) {
        set_error(MIMUSB_ERR_PARAM, field, "The address must be an IN endpoint of the Node.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    if (const mimusb_result r = link_ready(node, field); r != MIMUSB_OK)
        return r;
    const std::uint8_t payload[1] = {address};
    const mimusb_result r = command_error(
        node->device->link.command(MIMUSB_T_EP_FLUSH, payload, sizeof(payload), control_timeout_ms,
                                   kCommandAttempts),
        field, "The board did not accept the request.", MIMUSB_T_EP_FLUSH, public_node_id(node));
    if (r == MIMUSB_OK)
        reset_count(node->device, ep);
    return r;
}
MIMUSB_C_RESULT_CATCH("node.endpoint.flush")

mimusb_result MIMUSB_CALL mimusb_node_endpoint_set_policy(mimusb_node* node, const uint8_t address,
                                                          const mimusb_endpoint_policy* policy,
                                                          const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    const char* field = "node.endpoint.set_policy";
    if (const mimusb_result r = check_call(node, field); r != MIMUSB_OK)
        return r;
    if (policy == nullptr || endpoint_direction(node, address) != Direction::in) {
        set_error(MIMUSB_ERR_PARAM, field,
                  "A policy and the address of an IN endpoint of the Node are required.", 0, 0,
                  public_node_id(node), 0, address, 0);
        return MIMUSB_ERR_PARAM;
    }
    if (policy->on_full == 0U || policy->depth == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, field, "on_full and depth are required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (policy->on_full > MIMUSB_FULL_WAIT) {
        set_error(MIMUSB_ERR_PARAM, field,
                  "on_full is MIMUSB_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    mimusb_device* device = node->device;
    if (device->link.failed()) {
        set_error(MIMUSB_ERR_NO_DEVICE, field, "The serial link is gone.");
        return MIMUSB_ERR_NO_DEVICE;
    }
    const bool live = device->configured && !device->batch_open && !node->staged_only &&
                      !device->reconfiguring.load(std::memory_order_acquire);
    if (node->generic) {
        EndpointRuntime* ep = runtime_of(node, address);
        if (live) {
            const mimusb_result r = command_error(
                device->link.command(
                    MIMUSB_T_EP_CONFIG,
                    std::array<std::uint8_t, 3>{address, policy->depth, policy->on_full}.data(), 3,
                    control_timeout_ms, kCommandAttempts),
                field, "The board did not accept the policy.", MIMUSB_T_EP_CONFIG,
                public_node_id(node));
            if (r != MIMUSB_OK)
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
        for (mimusb::detail::GenericInterface& itf : node->interfaces) {
            itf.for_each_endpoint([&](mimusb_usb_endpoint& e) {
                if (e.address == address)
                    e.policy = *policy;
            });
        }
        device->dirty = device->dirty || !live;
        return MIMUSB_OK;
    }
    // A profile Node: the interface's queue takes the least depth of its Nodes.
    const mimusb_endpoint_policy previous = node->config.ep_in.policy;
    node->config.ep_in.policy = *policy;
    if (live) {
        const std::uint8_t depth = mimusb::detail::hid_interface_depth(device, node->iface);
        const std::uint8_t payload[3] = {address, depth, policy->on_full};
        const mimusb_result r =
            command_error(device->link.command(MIMUSB_T_EP_CONFIG, payload, sizeof(payload),
                                               control_timeout_ms, kCommandAttempts),
                          field, "The board did not accept the policy.", MIMUSB_T_EP_CONFIG,
                          public_node_id(node));
        if (r != MIMUSB_OK) {
            node->config.ep_in.policy = previous; // the board keeps the policy it had
            return r;
        }
    }
    EndpointRuntime* ep = runtime_of(node, address);
    {
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        if (ep != nullptr) {
            ep->policy = *policy;
            ep->outstanding = 0U;
            ++ep->epoch;
        }
    }
    device->endpoint_cv.notify_all();
    device->dirty = device->dirty || !live;
    return MIMUSB_OK;
}
MIMUSB_C_RESULT_CATCH("node.endpoint.set_policy")

} // extern "C"
