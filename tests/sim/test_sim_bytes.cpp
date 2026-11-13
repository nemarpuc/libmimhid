// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* A Device given as bytes (mimusb_device_set_descriptors) and the BOS descriptor, against the
 * firmware core: the descriptors, strings and BOS the target reads back, bulk both ways, a control
 * request answered by a rule of the Device, every refusal, the exclusion with Nodes in both
 * directions, and staging inside a batch. The target is the simulated one of fake_board.c. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace sim {

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes device_bytes(std::uint16_t bcd = 0x0210) {
    return {18,
            1,
            static_cast<std::uint8_t>(bcd),
            static_cast<std::uint8_t>(bcd >> 8),
            0,
            0,
            0,
            64,
            0x09,
            0x12,
            0x01,
            0x00,
            0x00,
            0x01,
            1,
            2,
            0,
            1};
}

// One vendor interface with a bulk IN 0x81 and a bulk OUT 0x01.
Bytes config_bytes() {
    return {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 0,    0, 2,  0xFF, 1,
            2, 0, 7,  5, 0x81, 2, 64, 0,    0,  7, 5, 0x01, 2, 64, 0,    0};
}

// A USB 2.0 Extension capability and the Microsoft OS 2.0 platform capability (vendor code 1).
Bytes bos_bytes() {
    return {5,    0x0F, 40,   0,    2,    7,    0x10, 0x02, 0x02, 0,    0,    0,    28,   0x10,
            0x05, 0,    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D,
            0x9E, 0x64, 0x8A, 0x9F, 0x00, 0x00, 0x03, 0x06, 0x42, 0x00, 0x01, 0x00};
}

struct Given {
    Bytes device, config, bos;
    mimusb_device_descriptors d{};
    explicit Given(Bytes dev = device_bytes(), Bytes cfg = config_bytes(), Bytes b = {})
        : device(std::move(dev)), config(std::move(cfg)), bos(std::move(b)) {
        refresh();
    }
    const mimusb_device_descriptors* get() {
        refresh();
        return &d;
    }
    void refresh() {
        d.struct_size = sizeof(d);
        d.device = device.data();
        d.device_length = device.size();
        d.configuration = config.data();
        d.configuration_length = config.size();
        d.bos = bos.empty() ? nullptr : bos.data();
        d.bos_length = bos.size();
    }
};

mimusb_result open_profile(mimusb_device* device, mimusb_spec* spec, mimusb_node** node) {
    test_hid_node_config config{};
    test_fill_node_config(&config, 0);
    return test_node_open_hid(device, spec, &config, &kCommit, node);
}

mimusb_endpoint_policy policy(std::uint8_t on_full, std::uint8_t depth) {
    mimusb_endpoint_policy p{};
    p.on_full = on_full;
    p.depth = depth;
    return p;
}

mimusb_result set_with_policy(mimusb_device* device, Given* g) {
    const mimusb_endpoint_policy p = policy(MIMUSB_FULL_DROP_NEWEST, 8);
    const mimusb_result r = mimusb_device_endpoint_set_policy(device, 0x81, &p, kControlMs);
    return r != MIMUSB_OK ? r : mimusb_device_set_descriptors(device, g->get(), &kCommit);
}

bool same(const std::uint8_t* got, const Bytes& want) {
    return got != nullptr && std::memcmp(got, want.data(), want.size()) == 0;
}

mimusb_control_rule vendor_answer(const Bytes& data) {
    mimusb_control_rule r{};
    r.request_type_mask = 0xFF;
    r.request_type_value = 0xC0;
    r.request_mask = 0xFF;
    r.request_value = 1;
    r.index_mask = 0xFFFF;
    r.index_value = 7;
    r.action = MIMUSB_CONTROL_ANSWER;
    r.data = data.data();
    r.length = data.size();
    return r;
}

void bytes_end_to_end() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_OK &&
                 limits.max_bos_bytes == MIMUSB_MAX_BOS_BYTES);

    // The policy of an IN endpoint comes first: without it the call names the endpoint.
    Given g(device_bytes(), config_bytes(), bos_bytes());
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "descriptors.endpoint.policy") == 0 &&
                 mimusb_last_error()->offset == 0x81U);
    // Strings as they go on the bus, in any language.
    const std::uint16_t manufacturer[] = {'A', 'c', 'm', 'e'};
    const std::uint16_t product[] = {'W', 'i', 'd', 'g', 'e', 't'};
    const std::uint16_t serial[] = {'0', '0', '1'};
    MIMUSB_CHECK(mimusb_device_set_string(device, 1, 0x0409, manufacturer, 4) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_string(device, 2, 0x0409, product, 6) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_string(device, 3, 0x0409, serial, 3) == MIMUSB_OK);
    // The Device's rules: the Microsoft OS 2.0 descriptor set, asked with the vendor code.
    const Bytes set = {10, 0, 0, 0, 0, 0, 3, 6, 0x42, 0};
    const mimusb_control_rule answer = vendor_answer(set);
    MIMUSB_CHECK(mimusb_device_set_control_rules(device, 0, &answer, 1, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(set_with_policy(device, &g) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // What the target reads: the bytes as given, byte for byte.
    std::uint16_t length = 0;
    MIMUSB_CHECK(same(fake_board_device_descriptor(), g.device));
    MIMUSB_CHECK(same(fake_board_config_descriptor(&length), g.config) &&
                 length == g.config.size());
    MIMUSB_CHECK(same(fake_board_bos_descriptor(&length), g.bos) && length == g.bos.size());
    char text[64];
    MIMUSB_CHECK(fake_board_string(1, text, sizeof(text)) != 0 && std::strcmp(text, "Acme") == 0);
    MIMUSB_CHECK(fake_board_string(2, text, sizeof(text)) != 0 && std::strcmp(text, "Widget") == 0);
    MIMUSB_CHECK(fake_board_string(3, text, sizeof(text)) != 0 && std::strcmp(text, "001") == 0);

    // Bulk IN, as written, in order; a zero-length packet.
    fake_transfer_t t;
    const std::uint8_t a[] = {'p', 'i', 'n', 'g'};
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, a, sizeof(a), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 4 &&
                 std::memcmp(t.data, a, 4) == 0);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, nullptr, 0, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 0);
    // Bulk OUT: an event of the Device (node 0) with the endpoint and its interface.
    const std::uint8_t b[] = {9, 8, 7};
    fake_board_target_out(0x01, b, sizeof(b));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.node_id == 0 &&
                 ev.endpoint_address == 0x01 && ev.interface_number == 0 &&
                 ev.data == Bytes(b, b + 3));
    // The Halt feature and a flush on the endpoints of the descriptors.
    MIMUSB_CHECK(mimusb_device_endpoint_stall(device, 0x81, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_endpoint_stalled(0x81));
    MIMUSB_CHECK(mimusb_device_endpoint_clear_stall(device, 0x81, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_endpoint_stalled(0x81));
    MIMUSB_CHECK(mimusb_device_endpoint_flush(device, 0x81, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_endpoint_flush(device, 0x01, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x82, a, 4, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x01, a, 4, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_endpoint_stall(device, 0x05, kControlMs) == MIMUSB_ERR_PARAM);
    // A policy that changes while running.
    const mimusb_endpoint_policy wait = policy(MIMUSB_FULL_WAIT, 2);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &wait, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, a, 4, 0) == MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, a, 4, kSendMs) == MIMUSB_OK);

    // The control request a rule of the Device answers (endpoint 0).
    fake_control_result_t r{};
    const std::uint8_t setup[8] = {0xC0, 1, 0, 0, 7, 0, 10, 0};
    fake_board_target_control(setup, nullptr, 0);
    MIMUSB_CHECK(fake_board_wait_control(&r, 2000) && r.outcome == FAKE_CONTROL_ANSWERED &&
                 r.length == set.size() && std::memcmp(r.data, set.data(), set.size()) == 0);

    // Exclusion: no Node beside the bytes, and the bytes call is refused for a Device with a Node
    // (checked after the descriptors are cleared).
    mimusb_node_config none;
    mimusb_node_config_init(&none);
    mimusb_node* node = nullptr;
    mimusb_usb_interface itf{};
    itf.number = 0;
    itf.class_code = 0xFF;
    none.interfaces = &itf;
    none.interface_count = 1;
    MIMUSB_CHECK(test_node_open_interfaces(device, &none, &kCommit, &node) == MIMUSB_ERR_BUSY &&
                 node == nullptr);
    mimusb_spec* spec = keyboard_spec(0);
    MIMUSB_CHECK(open_profile(device, spec, &node) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_set_bos(device, g.bos.data(), g.bos.size()) == MIMUSB_ERR_BUSY);

    // Cleared: the target loses the device, and the strings stay.
    MIMUSB_CHECK(mimusb_device_clear_descriptors(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_mounted());
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, a, 4, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_clear_descriptors(device, &kCommit) == MIMUSB_OK); // again: fine
    MIMUSB_CHECK(mimusb_device_clear_string(device, 1, 0x0409) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_clear_string(device, 1, 0x0409) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(open_profile(device, spec, &node) == MIMUSB_OK && node != nullptr);
    // With a Node open the bytes are refused the other way round.
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

void bytes_refusals_and_batch() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    const mimusb_endpoint_policy p = policy(MIMUSB_FULL_DROP_NEWEST, 4);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &p, kControlMs) == MIMUSB_OK);
    // Arguments of the endpoint policy call.
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x01, &p, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x80, &p, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, nullptr, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    const mimusb_endpoint_policy unset{};
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &unset, kControlMs) ==
                 MIMUSB_ERR_UNSET_FIELD);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &p, 0) == MIMUSB_ERR_UNSET_FIELD);
    // Arguments of the strings.
    const std::uint16_t unit[] = {'x'};
    MIMUSB_CHECK(mimusb_device_set_string(device, 0, 0x0409, unit, 1) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_string(device, 1, 0, unit, 1) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_string(device, 1, 0x0409, unit, 0) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_string(device, 1, 0x0409, nullptr, 1) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_string(device, 1, 0x0409, unit, 127) == MIMUSB_ERR_PARAM);

    // Every refusal names the field and the offset; nothing reaches the board.
    const unsigned connects = fake_board_connects();
    {
        Given g(device_bytes(0), config_bytes());
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) ==
                     MIMUSB_ERR_UNSET_FIELD);
        MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "descriptors.device") == 0 &&
                     mimusb_last_error()->offset == 2U);
    }
    {
        Bytes c = config_bytes();
        c[2] = 33; // wTotalLength
        Given g(device_bytes(), c);
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "descriptors.configuration") == 0 &&
                     mimusb_last_error()->offset == 2U);
    }
    {
        Bytes c = config_bytes();
        c[21] = 1; // isochronous
        Given g(device_bytes(), c);
#ifdef SIM_ISO
        // A board with isochronous endpoints takes it, and the bInterval of this one (0) is wrong.
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_last_error()->offset == 24U);
#else
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) ==
                     MIMUSB_ERR_UNSUPPORTED);
        MIMUSB_CHECK(mimusb_last_error()->offset == 21U);
#endif
    }
    {
        Given g(device_bytes(0x0200), config_bytes(), bos_bytes()); // a BOS below 0x0201
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "descriptors.device") == 0);
    }
    {
        Bytes b = bos_bytes();
        b[4] = 3; // bNumDeviceCaps
        Given g(device_bytes(), config_bytes(), b);
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "descriptors.bos") == 0 &&
                     mimusb_last_error()->offset == 4U);
    }
    {
        Given g;
        mimusb_device_descriptors bad = *g.get();
        bad.reserved = 1;
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, &bad, &kCommit) == MIMUSB_ERR_PARAM);
        bad = *g.get();
        bad.struct_size = 8;
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, &bad, &kCommit) == MIMUSB_ERR_PARAM);
        bad = *g.get();
        bad.device = nullptr;
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, &bad, &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, nullptr, &kCommit) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), nullptr) == MIMUSB_ERR_PARAM);
        const mimusb_commit_timeouts zero = {sizeof(zero), 0, 0, 0};
        MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &zero) ==
                     MIMUSB_ERR_UNSET_FIELD);
    }
    MIMUSB_CHECK(fake_board_connects() == connects);

    // Inside a batch the descriptors are only staged; the commit that ends it sends them.
    Given g;
    MIMUSB_CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, g.get(), &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_connects() == connects && !fake_board_mounted());
    const std::uint8_t a[] = {1};
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, a, 1, kSendMs) == MIMUSB_ERR_BUSY);
    MIMUSB_CHECK(mimusb_device_batch_end(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    std::uint16_t length = 0;
    MIMUSB_CHECK(same(fake_board_config_descriptor(&length), g.config) &&
                 fake_board_bos_descriptor(&length) == nullptr);
    MIMUSB_CHECK(fake_board_connects() == connects + 1);
    // Replacing them is a new commit: the new configuration, with the same endpoint policy.
    Bytes c = config_bytes();
    c.insert(c.end(), {7, 5, 0x82, 2, 64, 0, 0});
    c[2] = static_cast<std::uint8_t>(c.size());
    c[13] = 3;
    Given g2(device_bytes(), c);
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, g2.get(), &kCommit) ==
                 MIMUSB_ERR_UNSET_FIELD); // 0x82 has no policy yet
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x82, &p, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, g2.get(), &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    MIMUSB_CHECK(same(fake_board_config_descriptor(&length), g2.config));
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x82, a, 1, kSendMs) == MIMUSB_OK);
    fake_transfer_t t;
    MIMUSB_CHECK(fake_board_wait_transfer(0x82, &t, 1000) && t.length == 1);
    // The board refuses a pool its queues do not fit; the old configuration stays.
    const mimusb_endpoint_policy huge = policy(MIMUSB_FULL_DROP_OLDEST, 255);
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x82, &huge, kControlMs) != MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The BOS descriptor of a Device composed from Nodes.
void composed_bos() {
    mimusb_device_config options = base_options();
    options.usb_device.bcd_usb = 0x0210;
    mimusb_device* device = open_device(options);
    if (device == nullptr)
        return;
    const Bytes bos = bos_bytes();
    // Refused at once when it does not hold together.
    Bytes bad = bos;
    bad[2] = 41;
    MIMUSB_CHECK(mimusb_device_set_bos(device, bad.data(), bad.size()) == MIMUSB_ERR_PARAM &&
                 mimusb_last_error()->offset == 2U);
    MIMUSB_CHECK(mimusb_device_set_bos(device, nullptr, 5) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_device_set_bos(device, bos.data(), bos.size()) == MIMUSB_OK);
    mimusb_spec* spec = keyboard_spec(0);
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(open_profile(device, spec, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    std::uint16_t length = 0;
    MIMUSB_CHECK(same(fake_board_bos_descriptor(&length), bos) && length == bos.size());
    // Removed: the next commit has none (the request is stalled).
    MIMUSB_CHECK(mimusb_device_set_bos(device, nullptr, 0) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_bos_descriptor(&length) == nullptr);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);

    // A bcdUSB below 0x0201 refuses the commit.
    device = open_device(base_options());
    if (device == nullptr)
        return;
    spec = keyboard_spec(0);
    MIMUSB_CHECK(mimusb_device_set_bos(device, bos.data(), bos.size()) == MIMUSB_OK);
    node = nullptr;
    MIMUSB_CHECK(open_profile(device, spec, &node) == MIMUSB_ERR_PARAM && node == nullptr);
    MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "device.bos") == 0 &&
                 mimusb_last_error()->offset == 0x0200U);
    MIMUSB_CHECK(mimusb_device_set_bos(device, nullptr, 0) == MIMUSB_OK);
    MIMUSB_CHECK(open_profile(device, spec, &node) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(spec);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace

void test_byte_device() {
    bytes_end_to_end();
    bytes_refusals_and_batch();
    composed_bos();
}

} // namespace sim
