// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Public C ABI: a Device given as bytes (the descriptors of a real device the target is to see),
 * its raw strings, the BOS descriptor of a composed Device, and the endpoints of a Device given as
 * bytes. A Device given as bytes has no Node: the library reads no class, and the caller names an
 * endpoint by its address.
 */

#include "api/support.hpp"
#include "usb/descriptor_walker.hpp"

#include <algorithm>
#include <array>

namespace tr = mimhid::transport;
namespace usb = mimhid::usb;

using mimhid::detail::command_error;
using mimhid::detail::commit_device;
using mimhid::detail::CommitWaits;
using mimhid::detail::EndpointRuntime;
using mimhid::detail::kCommandAttempts;
using mimhid::detail::live_nodes;
using mimhid::detail::read_commit_timeouts;
using mimhid::detail::require_timeout;
using mimhid::detail::set_error;

namespace {

mimhid_result live_device(mimhid_device* device, const char* field) {
    if (device == nullptr || device->closing.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_PARAM, field, "A live Device is required.");
        return MIMHID_ERR_PARAM;
    }
    return MIMHID_OK;
}

void report(const usb::ComposerError& e) {
    set_error(e.result, e.field, e.reason, 0, 0, 0, 0, e.offset, e.length);
}

// Every endpoint call that sends something to the board: the link is up, and the board runs the
// configuration (it is not staged in a batch and not re-enumerating).
mimhid_result link_ready(mimhid_device* device, const char* field) {
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, field, "The serial link is gone.");
        return MIMHID_ERR_NO_DEVICE;
    }
    if (device->batch_open || device->reconfiguring.load(std::memory_order_acquire)) {
        set_error(MIMHID_ERR_BUSY, field,
                  "The board has not applied the descriptors yet (batch open or re-enumeration in "
                  "progress).");
        return MIMHID_ERR_BUSY;
    }
    return MIMHID_OK;
}

// The runtime of an IN endpoint of the descriptors; null when they have none such.
EndpointRuntime* runtime_of(mimhid_device* device, const std::uint8_t address) {
    if (!device->descriptors_set)
        return nullptr;
    for (EndpointRuntime& ep : device->bytes.in_endpoints) {
        if (ep.address == address)
            return &ep;
    }
    return nullptr;
}

bool has_endpoint(const mimhid_device* device, const std::uint8_t address) {
    return device->descriptors_set && device->bytes.interface_of.count(address) != 0U;
}

void reset_count(mimhid_device* device, EndpointRuntime* ep) {
    {
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        ep->outstanding = 0U;
        ++ep->epoch;
    }
    device->endpoint_cv.notify_all();
}

mimhid_result stall_command(mimhid_device* device, const std::uint8_t address, const bool stall,
                            const std::uint32_t timeout_ms, const char* field) {
    mimhid::detail::clear_error();
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    if (!has_endpoint(device, address)) {
        set_error(MIMHID_ERR_PARAM, field,
                  "The address is not an endpoint of the descriptors (none set, or none such).", 0,
                  0, 0, 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(timeout_ms, "control_timeout_ms"); t != MIMHID_OK)
        return t;
    if (const mimhid_result r = link_ready(device, field); r != MIMHID_OK)
        return r;
    const std::uint8_t payload[2] = {address, stall ? std::uint8_t{1} : std::uint8_t{0}};
    const mimhid_result r =
        command_error(device->link.command(MIMHID_T_EP_STALL, payload, sizeof(payload), timeout_ms,
                                           kCommandAttempts),
                      field, "The board did not accept the request.", MIMHID_T_EP_STALL);
    if (r == MIMHID_OK) {
        if (EndpointRuntime* ep = runtime_of(device, address))
            reset_count(device, ep);
    }
    return r;
}

} // namespace

extern "C" {

mimhid_result MIMHID_CALL mimhid_device_set_string(mimhid_device* device, const uint8_t index,
                                                   const uint16_t language_id,
                                                   const uint16_t* units,
                                                   const size_t unit_count) try {
    mimhid::detail::clear_error();
    const char* field = "device.set_string";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    if (units == nullptr || index == 0U || language_id == 0U || unit_count == 0U ||
        unit_count > MIMHID_STRING_MAX) {
        set_error(MIMHID_ERR_PARAM, field,
                  "units must not be null, index (1 to 255) and language_id are nonzero and "
                  "unit_count is 1 to 126 (USB 2.0 9.6.7).");
        return MIMHID_ERR_PARAM;
    }
    const auto key = std::make_pair(index, language_id);
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    if (device->raw_strings.count(key) == 0U &&
        device->raw_strings.size() >= device->board.max_strings) {
        set_error(MIMHID_ERR_OVERFLOW, field,
                  "The strings exceed the board's max_strings (mimhid_board_limits).", 0, 0, 0, 0,
                  device->board.max_strings, 0);
        return MIMHID_ERR_OVERFLOW;
    }
    device->raw_strings[key].assign(reinterpret_cast<const char16_t*>(units), unit_count);
    device->dirty = true;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.set_string")

mimhid_result MIMHID_CALL mimhid_device_clear_string(mimhid_device* device, const uint8_t index,
                                                     const uint16_t language_id) try {
    mimhid::detail::clear_error();
    if (const mimhid_result r = live_device(device, "device.clear_string"); r != MIMHID_OK)
        return r;
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    if (device->raw_strings.erase(std::make_pair(index, language_id)) == 0U) {
        set_error(MIMHID_ERR_PARAM, "device.clear_string",
                  "No string was given with this index and language by "
                  "mimhid_device_set_string.");
        return MIMHID_ERR_PARAM;
    }
    device->dirty = true;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.clear_string")

mimhid_result MIMHID_CALL mimhid_device_set_bos(mimhid_device* device, const uint8_t* bytes,
                                                const size_t length) try {
    mimhid::detail::clear_error();
    const char* field = "device.set_bos";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    if (bytes == nullptr && length != 0U) {
        set_error(MIMHID_ERR_PARAM, field, "bytes is null with a nonzero length.");
        return MIMHID_ERR_PARAM;
    }
    if (device->descriptors_set) {
        set_error(MIMHID_ERR_BUSY, field,
                  "The Device is given as bytes: its BOS descriptor is in "
                  "mimhid_device_descriptors.");
        return MIMHID_ERR_BUSY;
    }
    if (length != 0U) {
        usb::ComposerError error;
        if (usb::check_bos(bytes, length, mimhid::detail::composer_limits(device), "bos", &error) !=
            MIMHID_OK) {
            report(error);
            return error.result;
        }
    }
    device->composed_bos.assign(bytes, bytes + length);
    device->dirty = true;
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.set_bos")

mimhid_result MIMHID_CALL
mimhid_device_set_descriptors(mimhid_device* device, const mimhid_device_descriptors* descriptors,
                              const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    const char* field = "device.set_descriptors";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, "device", "The serial link failed or was unplugged.");
        return MIMHID_ERR_NO_DEVICE;
    }
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    if (!mimhid::detail::valid_struct(
            descriptors, descriptors == nullptr ? 0U : descriptors->struct_size,
            static_cast<std::uint32_t>(sizeof(mimhid_device_descriptors)), "descriptors"))
        return MIMHID_ERR_PARAM;
    if (!mimhid::detail::reserved_is_zero(descriptors->reserved, "descriptors.reserved"))
        return MIMHID_ERR_PARAM;
    if (!live_nodes(device).empty()) {
        set_error(MIMHID_ERR_BUSY, field,
                  "A Node is open: a Device is composed from Nodes or given as bytes, not both.");
        return MIMHID_ERR_BUSY;
    }
    if ((descriptors->device == nullptr && descriptors->device_length != 0U) ||
        (descriptors->configuration == nullptr && descriptors->configuration_length != 0U) ||
        (descriptors->bos == nullptr && descriptors->bos_length != 0U)) {
        set_error(MIMHID_ERR_PARAM, field, "A pointer is null with a nonzero length.");
        return MIMHID_ERR_PARAM;
    }

    const usb::ComposerLimits limits = mimhid::detail::composer_limits(device);
    usb::WalkedDescriptors walked;
    usb::ComposerError error;
    if (usb::walk_descriptors(descriptors->device, descriptors->device_length,
                              descriptors->configuration, descriptors->configuration_length,
                              descriptors->bos_length == 0U ? nullptr : descriptors->bos,
                              descriptors->bos_length, limits, &walked, &error) != MIMHID_OK) {
        report(error);
        return error.result;
    }

    // Every IN endpoint has the policy it was given; the queues of all of them fit the pool.
    std::vector<EndpointRuntime> runtimes;
    std::map<std::uint8_t, std::uint8_t> interface_of;
    std::size_t pool_slots = 0U;
    {
        std::lock_guard<std::mutex> lock(device->nodes_mutex);
        for (const usb::WalkedEndpoint& ep : walked.endpoints) {
            interface_of[ep.address] = ep.interface_number;
            if ((ep.address & 0x80U) == 0U)
                continue;
            const auto policy = device->byte_policies.find(ep.address);
            if (policy == device->byte_policies.end()) {
                set_error(MIMHID_ERR_UNSET_FIELD, "descriptors.endpoint.policy",
                          "An IN endpoint needs a policy before the descriptors are set "
                          "(mimhid_device_endpoint_set_policy); its address is in offset.",
                          0, 0, 0, 0, ep.address, ep.offset);
                return MIMHID_ERR_UNSET_FIELD;
            }
            EndpointRuntime runtime;
            runtime.address = ep.address;
            runtime.max_packet_size = ep.max_packet_size;
            runtime.policy = policy->second;
            runtimes.push_back(runtime);
            pool_slots += usb::queue_slots(policy->second.depth, policy->second.on_full);
        }
        if (device->raw_strings.size() > device->board.max_strings) {
            set_error(MIMHID_ERR_OVERFLOW, "device.strings",
                      "The strings exceed the board's max_strings.", 0, 0, 0, 0,
                      device->board.max_strings,
                      static_cast<std::uint32_t>(device->raw_strings.size()));
            return MIMHID_ERR_OVERFLOW;
        }
    }
    if (pool_slots > limits.endpoint_queue_slots) {
        set_error(MIMHID_ERR_OVERFLOW, "descriptors.endpoint.policy",
                  "The queues of the IN endpoints need more slots than the board's pool "
                  "(mimhid_board_limits.endpoint_queue_slots); the slots needed are in length.",
                  0, 0, 0, 0, static_cast<std::uint32_t>(limits.endpoint_queue_slots),
                  static_cast<std::uint32_t>(pool_slots));
        return MIMHID_ERR_OVERFLOW;
    }

    // Stage, then commit like a Node open does; a refused commit puts back what was there.
    mimhid_device::ByteDevice previous_bytes;
    const bool previous_set = device->descriptors_set;
    {
        std::lock_guard<std::mutex> nodes(device->nodes_mutex);
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        previous_bytes = std::move(device->bytes);
        device->bytes = mimhid_device::ByteDevice{};
        device->bytes.device.assign(descriptors->device,
                                    descriptors->device + descriptors->device_length);
        device->bytes.configuration.assign(descriptors->configuration,
                                           descriptors->configuration +
                                               descriptors->configuration_length);
        if (descriptors->bos_length != 0U)
            device->bytes.bos.assign(descriptors->bos, descriptors->bos + descriptors->bos_length);
        device->bytes.interface_of = std::move(interface_of);
        device->bytes.in_endpoints = std::move(runtimes);
        device->descriptors_set = true;
    }
    device->dirty = true;
    if (device->batch_open)
        return MIMHID_OK;
    const mimhid_result committed = commit_device(device, 0U, waits);
    if (committed != MIMHID_OK) {
        {
            std::lock_guard<std::mutex> nodes(device->nodes_mutex);
            std::lock_guard<std::mutex> lock(device->endpoint_mutex);
            device->bytes = std::move(previous_bytes);
            device->descriptors_set = previous_set;
        }
        if (device->configured)
            commit_device(device, 0U, waits);
        return committed;
    }
    mimhid::detail::log_message(device, MIMHID_LOG_INFO, "descriptors set");
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.set_descriptors")

mimhid_result MIMHID_CALL
mimhid_device_clear_descriptors(mimhid_device* device, const mimhid_commit_timeouts* timeouts) try {
    mimhid::detail::clear_error();
    if (const mimhid_result r = live_device(device, "device.clear_descriptors"); r != MIMHID_OK)
        return r;
    CommitWaits waits;
    if (const mimhid_result r = read_commit_timeouts(timeouts, &waits); r != MIMHID_OK)
        return r;
    const bool was_set = device->descriptors_set;
    {
        std::lock_guard<std::mutex> nodes(device->nodes_mutex);
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        device->bytes = mimhid_device::ByteDevice{};
        device->descriptors_set = false;
        device->byte_policies.clear();
    }
    device->endpoint_cv.notify_all();
    if (!was_set)
        return MIMHID_OK;
    device->dirty = true;
    if (device->batch_open)
        return MIMHID_OK;
    return commit_device(device, 0U, waits);
}
MIMHID_C_RESULT_CATCH("device.clear_descriptors")

mimhid_result MIMHID_CALL mimhid_device_endpoint_set_policy(mimhid_device* device,
                                                            const uint8_t address,
                                                            const mimhid_endpoint_policy* policy,
                                                            const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    const char* field = "device.endpoint.set_policy";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    if (policy == nullptr || (address & 0x80U) == 0U || (address & 0x70U) != 0U ||
        (address & 0x0FU) == 0U) {
        set_error(MIMHID_ERR_PARAM, field,
                  "A policy and an IN address (0x81 to 0x8F) are required.", 0, 0, 0, 0, address,
                  0);
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
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (device->link.failed()) {
        set_error(MIMHID_ERR_NO_DEVICE, field, "The serial link is gone.");
        return MIMHID_ERR_NO_DEVICE;
    }
    // The board holds a queue for this endpoint only when the committed descriptors have it.
    const bool live = runtime_of(device, address) != nullptr && device->configured &&
                      !device->batch_open && !device->reconfiguring.load(std::memory_order_acquire);
    if (live) {
        const std::array<std::uint8_t, 3> payload{address, policy->depth, policy->on_full};
        const mimhid_result r =
            command_error(device->link.command(MIMHID_T_EP_CONFIG, payload.data(), payload.size(),
                                               control_timeout_ms, kCommandAttempts),
                          field, "The board did not accept the policy.", MIMHID_T_EP_CONFIG);
        if (r != MIMHID_OK)
            return r;
    }
    {
        std::lock_guard<std::mutex> nodes(device->nodes_mutex);
        std::lock_guard<std::mutex> lock(device->endpoint_mutex);
        device->byte_policies[address] = *policy;
        if (EndpointRuntime* ep = runtime_of(device, address)) {
            ep->policy = *policy;
            ep->outstanding = 0U;
            ++ep->epoch;
        }
    }
    device->endpoint_cv.notify_all();
    device->dirty = device->dirty || (runtime_of(device, address) != nullptr && !live);
    return MIMHID_OK;
}
MIMHID_C_RESULT_CATCH("device.endpoint.set_policy")

mimhid_result MIMHID_CALL mimhid_device_endpoint_write(mimhid_device* device, const uint8_t address,
                                                       const uint8_t* data, const size_t length,
                                                       const uint32_t timeout_ms) try {
    mimhid::detail::clear_error();
    const char* field = "device.endpoint.write";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    EndpointRuntime* ep = runtime_of(device, address);
    if (ep == nullptr || (data == nullptr && length != 0U)) {
        set_error(MIMHID_ERR_PARAM, field,
                  "The address must be an IN endpoint of the descriptors; data is null only with "
                  "length 0.",
                  0, 0, 0, 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result r = link_ready(device, field); r != MIMHID_OK)
        return r;
    return mimhid::detail::endpoint_write(device, 0U, ep, address, data, length, timeout_ms);
}
MIMHID_C_RESULT_CATCH("device.endpoint.write")

mimhid_result MIMHID_CALL mimhid_device_endpoint_stall(mimhid_device* device, const uint8_t address,
                                                       const uint32_t control_timeout_ms) try {
    return stall_command(device, address, true, control_timeout_ms, "device.endpoint.stall");
}
MIMHID_C_RESULT_CATCH("device.endpoint.stall")

mimhid_result MIMHID_CALL mimhid_device_endpoint_clear_stall(
    mimhid_device* device, const uint8_t address, const uint32_t control_timeout_ms) try {
    return stall_command(device, address, false, control_timeout_ms, "device.endpoint.clear_stall");
}
MIMHID_C_RESULT_CATCH("device.endpoint.clear_stall")

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
mimhid_result MIMHID_CALL mimhid_device_endpoint_flush(mimhid_device* device, const uint8_t address,
                                                       const uint32_t control_timeout_ms) try {
    mimhid::detail::clear_error();
    const char* field = "device.endpoint.flush";
    if (const mimhid_result r = live_device(device, field); r != MIMHID_OK)
        return r;
    EndpointRuntime* ep = runtime_of(device, address);
    if (ep == nullptr) {
        set_error(MIMHID_ERR_PARAM, field, "The address must be an IN endpoint of the descriptors.",
                  0, 0, 0, 0, address, 0);
        return MIMHID_ERR_PARAM;
    }
    if (const mimhid_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMHID_OK)
        return t;
    if (const mimhid_result r = link_ready(device, field); r != MIMHID_OK)
        return r;
    const std::uint8_t payload[1] = {address};
    const mimhid_result r =
        command_error(device->link.command(MIMHID_T_EP_FLUSH, payload, sizeof(payload),
                                           control_timeout_ms, kCommandAttempts),
                      field, "The board did not accept the request.", MIMHID_T_EP_FLUSH);
    if (r == MIMHID_OK)
        reset_count(device, ep);
    return r;
}
MIMHID_C_RESULT_CATCH("device.endpoint.flush")

} // extern "C"
