// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * This file owns the private C-handle representations and shared invariants.
 * It contains no public ABI declarations beyond the opaque tag definitions.
 */

#include "classes/function_class.hpp"
#include "mimusb.h"
#include "transport/link.hpp"
#include "usb/board_rule.hpp"
#include "usb/descriptor_composer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mimusb::detail {
void set_error(mimusb_result result, const char* field, const char* reason,
               std::int32_t transport_status = 0, std::int32_t frame_type = 0,
               std::uint16_t node_id = 0, std::uint16_t report_id = 0, std::uint32_t offset = 0,
               std::uint32_t length = 0) noexcept;
void clear_error() noexcept;
mimusb_result finish_result(mimusb_result result, const char* field, const char* reason,
                            std::int32_t transport_status = 0, std::int32_t frame_type = 0,
                            std::uint16_t node_id = 0, std::uint16_t report_id = 0,
                            std::uint32_t offset = 0, std::uint32_t length = 0) noexcept;
const char* result_name(mimusb_result result) noexcept;
const char* frame_type_name(std::int32_t frame_type) noexcept;
void restore_error(const mimusb_error_detail& detail) noexcept;
bool retain_spec(struct mimusb_spec* spec) noexcept;
bool release_spec(struct mimusb_spec* spec) noexcept;
void log_message(const struct mimusb_device* device, mimusb_log_level level,
                 const char* message) noexcept;

bool valid_boolean(std::uint32_t value) noexcept;
// Defined inline so that static analysis sees that a null pointer never passes.
inline bool valid_struct(const void* value, const std::uint32_t actual_size,
                         const std::uint32_t expected_size, const char* field) noexcept {
    if (value == nullptr) {
        set_error(MIMUSB_ERR_PARAM, field, "The required structure pointer is null.");
        return false;
    }
    if (actual_size != expected_size) {
        set_error(MIMUSB_ERR_PARAM, field,
                  "struct_size must equal the size of the ABI structure used by this release.");
        return false;
    }
    return true;
}

} // namespace mimusb::detail

namespace mimusb::detail {

// One interface of a Node of a mimusb_spec_create_interfaces Spec, copied from the caller's
// configuration: its settings are bAlternateSetting 0, 1, 2, ... in order, each a sequence of
// items.
struct GenericInterface {
    std::uint8_t number{};
    std::uint8_t class_code{};
    std::uint8_t subclass{};
    std::uint8_t protocol{};
    std::string name;
    std::vector<usb::ComposerSetting> settings;

    // Calls `visit(endpoint)` for every endpoint item of every setting, in bus order.
    template <typename Visit> void for_each_endpoint(Visit&& visit) const {
        for (const usb::ComposerSetting& setting : settings) {
            for (const usb::ComposerItem& item : setting.items) {
                if (item.is_endpoint)
                    visit(item.endpoint);
            }
        }
    }
    template <typename Visit> void for_each_endpoint(Visit&& visit) {
        for (usb::ComposerSetting& setting : settings) {
            for (usb::ComposerItem& item : setting.items) {
                if (item.is_endpoint)
                    visit(item.endpoint);
            }
        }
    }
};

// What the library knows of an IN endpoint of a Node while it is open. The counters are
// guarded by the Device's endpoint_mutex. Nodes that share an endpoint (HID Nodes on one interface)
// each have an entry, but the one of the first live Node on that address is the one that counts.
struct EndpointRuntime {
    std::uint8_t address{};
    std::uint16_t max_packet_size{}; // an isochronous endpoint: the largest of its settings
    bool isochronous{}; // a transfer is one packet (USB 2.0 5.6): at most max_packet_size
    mimusb_endpoint_policy policy{};
    std::uint32_t outstanding{}; // frames written minus frames the board reported finished
    std::uint64_t epoch{};       // counts the resets of `outstanding`, which wake writers
};

} // namespace mimusb::detail

struct mimusb_spec {
    std::atomic<std::uint32_t> references{1U};
    mimusb_profile_kind kind{};
    // The class the Spec is of, and what that class keeps of it.
    const mimusb::detail::FunctionClass* function_class{};
    std::unique_ptr<mimusb::detail::SpecData> data;
};

struct mimusb_discovery {
    std::vector<mimusb_device_info> entries;
};

struct mimusb_device {
    // The updater thread sends on the link: it is joined before the members go.
    ~mimusb_device() {
        {
            std::lock_guard<std::mutex> lock(update_mutex);
            updater_stop = true;
        }
        update_cv.notify_all();
        if (updater.joinable())
            updater.join();
    }
    mimusb_device_config config{};
    mimusb_device_info info{};
    mimusb::transport::BoardInfo board{};
    mimusb::transport::Link link;
    std::uint32_t maximum_report_bytes{};
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string configuration;
    // Strings the caller added (mimusb_device_add_string), by index, in UTF-8.
    std::map<std::uint8_t, std::string> user_strings;

    // The Device given as bytes (mimusb_device_set_descriptors); while `descriptors_set`, no Node
    // is open and the composed configuration is not used. Guarded by nodes_mutex, except the IN
    // endpoint runtimes, whose counts endpoint_mutex guards like a Node's.
    struct ByteDevice {
        std::vector<std::uint8_t> device;
        std::vector<std::uint8_t> configuration;
        std::vector<std::uint8_t> bos; // empty: none
        // Interface number of each endpoint address, from the configuration.
        std::map<std::uint8_t, std::uint8_t> interface_of;
        // Runtime of every IN endpoint, in the order of the configuration. A vector that is never
        // resized while descriptors are set, so a pointer into it stays valid.
        std::vector<mimusb::detail::EndpointRuntime> in_endpoints;
    };
    bool descriptors_set{false};
    ByteDevice bytes;
    // Policies given by mimusb_device_endpoint_set_policy, by IN address; they outlive the
    // descriptors until mimusb_device_clear_descriptors, so they may be given first.
    std::map<std::uint8_t, mimusb_endpoint_policy> byte_policies;
    // The BOS of a composed Device (mimusb_device_set_bos); empty: none.
    std::vector<std::uint8_t> composed_bos;
    // Raw strings (mimusb_device_set_string), by (index, language), as UTF-16 code units.
    std::map<std::pair<std::uint8_t, std::uint16_t>, std::u16string> raw_strings;

    // The wait policy's counts (EndpointRuntime) and the writers that wait on them. A writer holds
    // the mutex only to look at a count, never while it waits for the board or writes.
    std::mutex endpoint_mutex;
    std::condition_variable endpoint_cv;

    // The control rules: what the board's table holds (the Nodes' own rules in node order, those
    // the classes write, then the Device's), and what the library needs of it. Guarded by
    // rules_mutex, which is never held while another mutex is taken. The reader thread finds the
    // rule a reported request matched in `table`, and a class changes the bytes of its live rules
    // in it (set_live_answer).
    std::mutex rules_mutex;
    std::vector<mimusb::usb::BoardRule> table;
    std::vector<mimusb::usb::BoardRule> device_rules; // set by mimusb_device_set_control_rules
    bool report_all{false};
    // The data stages the reader thread waits for on behalf of a class.
    std::vector<mimusb::detail::PendingData> pending; // reader thread only
    // Changes of the bytes of an ANSWER rule the reader thread asks for, done by `updater` (the
    // reader thread cannot wait for the board). Guarded by update_mutex.
    struct Update {
        std::uint16_t index{};
    };
    std::mutex update_mutex;
    std::condition_variable update_cv;
    std::deque<Update> updates;
    std::atomic<std::uint32_t> rule_timeout_ms{0}; // the control timeout of the last commit
    std::thread updater;
    bool updater_stop{false};

    // Nodes by node id (the index). A closed Node leaves a null entry that the next Node takes.
    // Event delivery reads this from the reader thread.
    std::mutex nodes_mutex;
    std::vector<struct mimusb_node*> slots;

    bool batch_open{false};
    bool configured{false};                  // the board holds a committed configuration
    bool dirty{false};                       // staged changes not yet committed
    std::atomic<bool> loss_reported{false};  // the board reports dropped reports
    std::atomic<bool> control_routed{false}; // the board holds control rules (the Device's own)
    std::atomic<bool> reconfiguring{false};
    std::atomic<bool> closing{false};
    std::atomic<mimusb_result> latched{MIMUSB_OK};
};

struct mimusb_node {
    mimusb_device* device{};
    mimusb_spec* spec{}; // retained while the Node lives
    // What the Node is on the bus, as its class made it at the placement: its interfaces (with
    // their endpoints and class descriptors), the association, and the numbers and endpoint
    // addresses it owns. The class may lay the interfaces out differently at a commit (HID Nodes
    // that share an interface), but a Node owns its numbers and endpoints whatever the layout.
    std::vector<mimusb::detail::GenericInterface> interfaces;
    std::vector<std::uint8_t> association;
    std::vector<std::uint8_t> interface_numbers;
    std::vector<std::uint8_t> endpoint_addresses;
    std::vector<mimusb::detail::EndpointRuntime> in_endpoints; // the IN endpoints
    // The Node's own rules: the extra_rules of its placement, then those its class made for it.
    std::vector<mimusb::usb::BoardRule> rules;
    mimusb::detail::RuleSlots rule_slots; // slot -> index in `rules`
    // The runtime of a class that keeps a state; null for one that does not.
    std::unique_ptr<mimusb::detail::NodeClass> runtime;
    std::uint16_t node_id{};
    std::uint8_t iface{}; // the first interface number of the Node
    bool closed{false};
    bool staged_only{false}; // opened inside a batch that has not ended
};
