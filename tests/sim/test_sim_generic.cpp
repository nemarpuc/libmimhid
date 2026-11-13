// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Nodes of any class (mimusb_node_open) against the firmware core: bulk and interrupt endpoints,
 * the IN policies, packets from the target, the Halt feature, strings, an interface association
 * beside HID Nodes, and every check of the open call. The target is the simulated one of
 * fake_board.c: it polls the IN endpoints unless reports are held. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace sim {

namespace {

mimusb_usb_endpoint endpoint(std::uint8_t address, std::uint8_t type, std::uint8_t on_full = 0,
                             std::uint8_t depth = 0) {
    mimusb_usb_endpoint ep{};
    ep.address = address;
    ep.transfer_type = type;
    ep.max_packet_size = type == MIMUSB_TRANSFER_BULK ? 64 : 16;
    ep.interval = type == MIMUSB_TRANSFER_INTERRUPT ? 4 : 0;
    if ((address & 0x80U) != 0U) {
        ep.policy.on_full = on_full != 0 ? on_full : MIMUSB_FULL_WAIT;
        ep.policy.depth = depth != 0 ? depth : 4;
    }
    return ep;
}

// One vendor-class interface (class 0xFF) with the given endpoints.
struct Vendor {
    std::vector<mimusb_usb_endpoint> eps;
    std::vector<std::uint8_t> class_bytes; // one whole class descriptor, before the endpoints
    std::vector<mimusb_usb_item> items;
    mimusb_usb_setting setting{};
    mimusb_usb_interface itf{};
    mimusb_node_config config;
    explicit Vendor(std::vector<mimusb_usb_endpoint> endpoints, std::uint8_t number = 0)
        : eps(std::move(endpoints)) {
        itf.number = number;
        itf.class_code = 0xFF;
        itf.subclass = 1;
        itf.protocol = 2;
        mimusb_node_config_init(&config);
        config.interface_count = 1;
        refresh();
    }
    void refresh() {
        // After a copy the pointers would point at the other object.
        items.clear();
        if (!class_bytes.empty())
            items.push_back(
                mimusb_usb_item_make_descriptor(class_bytes.data(), class_bytes.size()));
        for (const mimusb_usb_endpoint& ep : eps)
            items.push_back(mimusb_usb_item_make_endpoint(ep));
        setting.items = items.data();
        setting.item_count = items.size();
        itf.settings = &setting;
        itf.setting_count = 1;
        config.interfaces = &itf;
    }
};

mimusb_result open_vendor(mimusb_device* device, Vendor* v, mimusb_node** node) {
    v->refresh();
    return test_node_open_interfaces(device, &v->config, &kCommit, node);
}

std::vector<std::uint8_t> bytes(const fake_transfer_t& t) {
    return std::vector<std::uint8_t>(t.data, t.data + t.length);
}

bool wait_busy(std::uint8_t address, int timeout_ms = 1000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (fake_board_endpoint_busy(address))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Bulk IN and OUT end to end, packets, zero-length, long writes, stall, halt, flush.
void vendor_bulk() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    Vendor v({endpoint(0x81, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_DROP_NEWEST, 8),
              endpoint(0x01, MIMUSB_TRANSFER_BULK)});
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(open_vendor(device, &v, &node) == MIMUSB_OK && node != nullptr);
    if (node == nullptr) {
        MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
        return;
    }
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // The interface the target reads: class bytes as given, two endpoints.
    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMUSB_CHECK(length == 9 + 9 + 7 + 7 && c[4] == 1 && c[9 + 4] == 2 && c[9 + 5] == 0xFF &&
                 c[9 + 6] == 1 && c[9 + 7] == 2 && c[18] == 7 && c[18 + 1] == 5 &&
                 c[18 + 2] == 0x81 && c[18 + 3] == 2 && c[25 + 2] == 0x01);

    // In order, as written.
    fake_transfer_t t;
    const std::uint8_t a[] = {'h', 'e', 'l', 'l', 'o'};
    const std::uint8_t b[] = {1, 2, 3};
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) &&
                 bytes(t) == std::vector<std::uint8_t>(a, a + 5));
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) &&
                 bytes(t) == std::vector<std::uint8_t>(b, b + 3));

    // A zero-length packet (USB 2.0 5.8.3).
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, nullptr, 0, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 0);

    // An exact multiple of the packet size: the library adds no zero-length packet.
    std::vector<std::uint8_t> full(128);
    for (std::size_t i = 0; i < full.size(); ++i)
        full[i] = static_cast<std::uint8_t>(i);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, full.data(), full.size(), kSendMs) ==
                 MIMUSB_OK);
    std::vector<std::uint8_t> got;
    while (got.size() < 128 && fake_board_wait_transfer(0x81, &t, 1000)) {
        MIMUSB_CHECK(t.length == 64);
        got.insert(got.end(), t.data, t.data + t.length);
    }
    MIMUSB_CHECK(got == full && !fake_board_wait_transfer(0x81, &t, 150));

    // Longer than max_transfer_bytes (64 here): split on packet boundaries, in order.
    mimusb_board_limits limits;
    mimusb_board_limits_init(&limits);
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_OK &&
                 limits.max_transfer_bytes == 64);
    std::vector<std::uint8_t> big(200);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 3U);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, big.data(), big.size(), kSendMs) ==
                 MIMUSB_OK);
    got.clear();
    unsigned parts = 0;
    while (got.size() < big.size() && fake_board_wait_transfer(0x81, &t, 1000)) {
        got.insert(got.end(), t.data, t.data + t.length);
        ++parts;
    }
    MIMUSB_CHECK(got == big && parts == 4);

    // Bytes from the target arrive as events, in order; a zero-length packet too.
    for (std::uint8_t i = 0; i < 5; ++i) {
        const std::uint8_t packet[3] = {i, static_cast<std::uint8_t>(i + 1), 9};
        fake_board_target_out(0x01, packet, sizeof(packet));
    }
    fake_board_target_out(0x01, nullptr, 0);
    for (std::uint8_t i = 0; i < 5; ++i) {
        MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x01 &&
                     ev.interface_number == 0 && ev.node_id == mimusb_node_id(node) &&
                     ev.data.size() == 3 && ev.data[0] == i);
    }
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.data.empty());

    // OUT packets burst while the board's link is busy with other writes: none is lost.
    for (unsigned i = 0; i < 40; ++i) {
        const std::uint8_t packet[2] = {static_cast<std::uint8_t>(i), 0x5A};
        fake_board_target_out(0x01, packet, sizeof(packet));
    }
    for (unsigned i = 0; i < 40; ++i) {
        MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev, 3000) && ev.data.size() == 2 &&
                     ev.data[0] == i);
    }

    // The program stalls an IN endpoint: what is written waits; clearing lets it go.
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x81, kControlMs) == MIMUSB_OK &&
                 fake_board_endpoint_stalled(0x81));
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_wait_transfer(0x81, &t, 150));
    MIMUSB_CHECK(mimusb_node_endpoint_clear_stall(node, 0x81, kControlMs) == MIMUSB_OK &&
                 !fake_board_endpoint_stalled(0x81));
    // Stalling dropped what waited (the write above was dropped by the stall order or queued
    // after it; either way the endpoint works again).
    fake_board_clear_transfers();
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) &&
                 bytes(t) == std::vector<std::uint8_t>(b, b + 3));
    // OUT endpoint: stalled, then accepting again.
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x01, kControlMs) == MIMUSB_OK &&
                 fake_board_endpoint_stalled(0x01));
    MIMUSB_CHECK(mimusb_node_endpoint_clear_stall(node, 0x01, kControlMs) == MIMUSB_OK);
    const std::uint8_t again[1] = {0x77};
    fake_board_target_out(0x01, again, 1);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.data.size() == 1 &&
                 ev.data[0] == 0x77);

    // The target sets and clears the Halt feature (USB 2.0 9.4.5): the event says which.
    fake_board_target_halt(0x81, 1);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_HALT, &ev) && ev.value == 1 &&
                 ev.endpoint_address == 0x81 && ev.node_id == mimusb_node_id(node));
    fake_board_target_halt(0x81, 0);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_HALT, &ev) && ev.value == 0);
    fake_board_target_halt(0x01, 1);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_HALT, &ev) && ev.value == 1 &&
                 ev.endpoint_address == 0x01);
    fake_board_target_halt(0x01, 0);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_HALT, &ev) && ev.value == 0);

    // A flush drops what waits on the board; the transfer on the bus is read.
    fake_board_clear_transfers();
    fake_board_hold_reports(1);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(wait_busy(0x81));
    for (int i = 0; i < 3; ++i)
        MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMUSB_OK);
    sleep_ms(50);
    MIMUSB_CHECK(mimusb_node_endpoint_flush(node, 0x81, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_flush(node, 0x01, kControlMs) == MIMUSB_ERR_PARAM);
    fake_board_hold_reports(0);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 5);
    MIMUSB_CHECK(!fake_board_wait_transfer(0x81, &t, 150));

    // Calls that do not apply.
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x01, a, 1, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x85, a, 1, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x81, nullptr, 1, kSendMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x85, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_stall(node, 0x81, 0) == MIMUSB_ERR_UNSET_FIELD);

    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The three policies with a target that does not poll, and the wait policy with one that does.
void policies() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    MIMUSB_CHECK(mimusb_device_set_loss_reporting(device, 1, 0, kControlMs) == MIMUSB_OK);
    Vendor v({endpoint(0x81, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_DROP_NEWEST, 2),
              endpoint(0x82, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_DROP_OLDEST, 2),
              endpoint(0x83, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_WAIT, 2)});
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(open_vendor(device, &v, &node) == MIMUSB_OK);
    if (node == nullptr) {
        MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
        return;
    }
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));

    const auto one = [](char c) { return std::vector<std::uint8_t>{static_cast<std::uint8_t>(c)}; };
    const auto drain = [&](std::uint8_t address) {
        std::string out;
        fake_transfer_t t;
        while (fake_board_wait_transfer(address, &t, 200))
            out.push_back(static_cast<char>(t.data[0]));
        return out;
    };
    // The first transfer goes to the bus and waits there (the target does not poll); the queue
    // holds `depth` more (docs/LIMITS.md).
    fake_board_clear_transfers();
    fake_board_hold_reports(1);
    for (const std::uint8_t address : {std::uint8_t{0x81}, std::uint8_t{0x82}}) {
        const auto first = one('A');
        MIMUSB_CHECK(mimusb_node_endpoint_write(node, address, first.data(), 1, kSendMs) ==
                     MIMUSB_OK);
        MIMUSB_CHECK(wait_busy(address));
        for (const char c : {'B', 'C', 'D', 'E'}) {
            const auto d = one(c);
            MIMUSB_CHECK(mimusb_node_endpoint_write(node, address, d.data(), 1, kSendMs) ==
                         MIMUSB_OK);
        }
    }
    sleep_ms(100);
    fake_board_hold_reports(0);
    MIMUSB_CHECK(drain(0x81) == "ABC"); // the newest, D and E, were dropped
    MIMUSB_CHECK(drain(0x82) == "ADE"); // the oldest that was not being sent, B then C, dropped
    // The losses are reported with the endpoint.
    unsigned lost81 = 0, lost82 = 0;
    while (wait_event(MIMUSB_EVENT_TRANSFER_LOST, &ev, 300)) {
        if (ev.endpoint_address == 0x81)
            lost81 += ev.value;
        if (ev.endpoint_address == 0x82)
            lost82 += ev.value;
        MIMUSB_CHECK(ev.node_id == mimusb_node_id(node));
    }
    MIMUSB_CHECK(lost81 == 2 && lost82 == 2);

    // Wait: nothing is dropped, the third write blocks and times out (nothing sent).
    fake_board_hold_reports(1);
    const auto a = one('A');
    const auto b = one('B');
    const auto c = one('C');
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x83, a.data(), 1, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x83, b.data(), 1, kSendMs) == MIMUSB_OK);
    const auto begin = std::chrono::steady_clock::now();
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x83, c.data(), 1, 200) == MIMUSB_ERR_TIMEOUT);
    MIMUSB_CHECK(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(180));
    MIMUSB_CHECK(mimusb_node_endpoint_write(node, 0x83, c.data(), 1, 0) == MIMUSB_ERR_UNSET_FIELD);
    // A blocked writer is woken by a flush and gets BUSY, with nothing sent.
    mimusb_result blocked = MIMUSB_OK;
    std::thread writer(
        [&] { blocked = mimusb_node_endpoint_write(node, 0x83, c.data(), 1, 5000); });
    sleep_ms(150);
    MIMUSB_CHECK(mimusb_node_endpoint_flush(node, 0x83, kControlMs) == MIMUSB_OK);
    writer.join();
    MIMUSB_CHECK(blocked == MIMUSB_ERR_BUSY);
    fake_board_hold_reports(0);
    MIMUSB_CHECK(drain(0x83) == "A"); // the one on the bus was read; B was flushed

    // Wait with a polling target: a long burst loses nothing and stays in order.
    fake_board_clear_transfers();
    constexpr unsigned kBurst = 1500;
    std::thread reader_done;
    mimusb_result burst = MIMUSB_OK;
    std::thread burst_writer([&] {
        for (unsigned i = 0; i < kBurst && burst == MIMUSB_OK; ++i) {
            const std::uint8_t d[4] = {static_cast<std::uint8_t>(i),
                                       static_cast<std::uint8_t>(i >> 8), 0, 0};
            burst = mimusb_node_endpoint_write(node, 0x83, d, sizeof(d), 5000);
        }
    });
    unsigned seen = 0;
    fake_transfer_t t;
    while (seen < kBurst && fake_board_wait_transfer(0x83, &t, 3000)) {
        MIMUSB_CHECK(t.length == 4 && (t.data[0] | (t.data[1] << 8)) == static_cast<int>(seen));
        ++seen;
    }
    burst_writer.join();
    MIMUSB_CHECK(burst == MIMUSB_OK && seen == kBurst);

    // Changing a policy: refused when the queue cannot hold it, accepted when it can.
    mimusb_endpoint_policy p{MIMUSB_FULL_DROP_NEWEST, 255};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x81, &p, kControlMs) ==
                 MIMUSB_ERR_OVERFLOW);
    p = {MIMUSB_FULL_DROP_OLDEST, 1};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMUSB_OK);
    p = {0, 1};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x81, &p, kControlMs) ==
                 MIMUSB_ERR_UNSET_FIELD);
    p = {4, 1};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMUSB_ERR_PARAM);
    p = {MIMUSB_FULL_WAIT, 1};
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x01, &p, kControlMs) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(nullptr, 0x81, &p, kControlMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(mimusb_node_endpoint_set_policy(node, 0x81, &p, 0) == MIMUSB_ERR_UNSET_FIELD);

    MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// Every check of the open call. Each refusal leaves the Device as it was.
void open_checks() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_node* node = nullptr;
    const auto expect = [&](Vendor& v, mimusb_result want, const char* field = nullptr,
                            int line = __builtin_LINE()) {
        node = nullptr;
        const mimusb_result r = open_vendor(device, &v, &node);
        MIMUSB_CHECK(r == want && (want == MIMUSB_OK || node == nullptr));
        if (r != want)
            std::fprintf(stderr, "open check at line %d: %s, wanted %s (%s)\n", line,
                         mimusb_result_name(r), mimusb_result_name(want),
                         mimusb_last_error()->reason);
        if (field != nullptr && r != MIMUSB_OK)
            MIMUSB_CHECK(std::strstr(mimusb_last_error()->field, field) != nullptr);
        if (node != nullptr)
            MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    };
    const auto base = [] {
        return Vendor({endpoint(0x81, MIMUSB_TRANSFER_BULK), endpoint(0x01, MIMUSB_TRANSFER_BULK)});
    };

    MIMUSB_CHECK(test_node_open_interfaces(device, nullptr, &kCommit, &node) == MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(test_node_open_interfaces(nullptr, nullptr, &kCommit, &node) == MIMUSB_ERR_PARAM);
    {
        Vendor v = base();
        v.refresh();
        v.config.struct_size = 4;
        expect(v, MIMUSB_ERR_PARAM);
        v = base();
        v.refresh();
        v.config.reserved = 1;
        expect(v, MIMUSB_ERR_PARAM);
        v = base();
        v.refresh();
        v.config.interface_count = 0;
        expect(v, MIMUSB_ERR_PARAM);
        v = base();
        v.refresh();
        MIMUSB_CHECK(test_node_open_interfaces(device, &v.config, nullptr, &node) ==
                     MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(test_node_open_interfaces(device, &v.config, &kCommit, nullptr) ==
                     MIMUSB_ERR_PARAM);
    }
    // Class: 0 is unset; any other is the caller's.
    {
        Vendor v = base();
        v.refresh();
        v.itf.class_code = 0;
        expect(v, MIMUSB_ERR_UNSET_FIELD, "class_code");
        v = base();
        v.refresh();
        v.itf.class_code = 0x03; // a hand-written HID interface is the caller's too
        expect(v, MIMUSB_OK);
        v = base();
        v.refresh();
        v.itf.class_code = 0x02; // any other class is the caller's
        expect(v, MIMUSB_OK);
    }
    // The numbers are 0, 1, 2, ... without a gap: the missing one is named.
    {
        Vendor v({endpoint(0x81, MIMUSB_TRANSFER_BULK)}, 1);
        expect(v, MIMUSB_ERR_PARAM, "number");
        MIMUSB_CHECK(mimusb_last_error()->offset == 0);
    }
    // Endpoint addresses (USB 2.0 9.6.6).
    for (const std::uint8_t bad : {0x00, 0x80, 0x90, 0xC1, 0x41, 0x10}) {
        Vendor v({endpoint(bad, MIMUSB_TRANSFER_BULK)});
        expect(v, MIMUSB_ERR_PARAM, "address");
    }
    if (SIM_ENDPOINTS < 15) {
        const std::uint8_t above = static_cast<std::uint8_t>(SIM_ENDPOINTS + 1);
        Vendor v({endpoint(static_cast<std::uint8_t>(0x80U | above), MIMUSB_TRANSFER_BULK)});
        expect(v, MIMUSB_ERR_OVERFLOW, "address");
        Vendor w({endpoint(above, MIMUSB_TRANSFER_BULK)});
        expect(w, MIMUSB_ERR_OVERFLOW, "address");
    }
    {
        Vendor v({endpoint(0x81, MIMUSB_TRANSFER_BULK), endpoint(0x81, MIMUSB_TRANSFER_BULK)});
        expect(v, MIMUSB_ERR_PARAM, "address");
        v = Vendor({endpoint(0x81, MIMUSB_TRANSFER_BULK), endpoint(0x01, MIMUSB_TRANSFER_BULK)});
        v.eps[1].audio_form = 2;
        expect(v, MIMUSB_ERR_PARAM);
    }
    // Transfer type.
    {
        Vendor v = base();
        v.refresh();
        v.eps[0].transfer_type = 0;
        expect(v, MIMUSB_ERR_UNSET_FIELD, "transfer_type");
        v = base();
        v.refresh();
        v.eps[0].transfer_type = 1; // isochronous: the board has none
        expect(v, MIMUSB_ERR_UNSUPPORTED, "transfer_type");
        v = base();
        v.refresh();
        v.eps[0].transfer_type = 0; // control
        v.eps[0].transfer_type = 4;
        expect(v, MIMUSB_ERR_PARAM, "transfer_type");
    }
    // Packet size.
    {
        Vendor v = base();
        v.refresh();
        v.eps[0].max_packet_size = 0;
        expect(v, MIMUSB_ERR_UNSET_FIELD, "max_packet_size");
        v = base();
        v.refresh();
        v.eps[0].max_packet_size = 65;
        expect(v, MIMUSB_ERR_OVERFLOW, "max_packet_size");
        v = base();
        v.refresh();
        v.eps[0].max_packet_size = 48; // bulk is 8, 16, 32 or 64 at full speed (USB 2.0 5.8.3)
        expect(v, MIMUSB_ERR_PARAM, "max_packet_size");
        v = base();
        v.refresh();
        v.eps[0].max_packet_size = 8;
        expect(v, MIMUSB_OK);
    }
    // Interrupt interval; policies.
    {
        Vendor v({endpoint(0x81, MIMUSB_TRANSFER_INTERRUPT)});
        v.eps[0].interval = 0;
        expect(v, MIMUSB_ERR_UNSET_FIELD, "interval");
        v = Vendor({endpoint(0x81, MIMUSB_TRANSFER_INTERRUPT)});
        expect(v, MIMUSB_OK);
        v = base();
        v.refresh();
        v.eps[0].policy = {};
        expect(v, MIMUSB_ERR_UNSET_FIELD, "policy");
        v = base();
        v.refresh();
        v.eps[0].policy.depth = 0;
        expect(v, MIMUSB_ERR_UNSET_FIELD, "policy");
        v = base();
        v.refresh();
        v.eps[0].policy.on_full = 9;
        expect(v, MIMUSB_ERR_PARAM, "policy");
        v = base();
        v.refresh();
        v.eps[1].policy = {MIMUSB_FULL_WAIT, 2};
        expect(v, MIMUSB_ERR_PARAM, "policy");
        // More queue than the board's pool holds: a queue of depth 255 that drops the oldest takes
        // 2 * 255 + 1 slots, which one endpoint of a pool of 511 or more can have, so two do not fit
        // (the pool is the port's, MIMUSB_EP_POOL_SLOTS).
        static_assert(2 * (2 * 255 + 1) > MIMUSB_EP_POOL_SLOTS, "two full queues exceed the pool");
        v = Vendor({endpoint(0x81, MIMUSB_TRANSFER_BULK), endpoint(0x82, MIMUSB_TRANSFER_BULK)});
        v.refresh();
        v.eps[0].policy = {MIMUSB_FULL_DROP_OLDEST, 255};
        v.eps[1].policy = {MIMUSB_FULL_DROP_OLDEST, 255};
        expect(v, MIMUSB_ERR_OVERFLOW, "policy");
    }
    // Class descriptors: bLength at least 2 and inside the bytes.
    {
        const std::uint8_t good[] = {5, 0x24, 0, 0x10, 0x01};
        const std::uint8_t zero_length[] = {1, 0x24};
        const std::uint8_t too_long[] = {9, 0x24, 0};
        Vendor v = base();
        v.refresh();
        v.class_bytes.assign(good, good + sizeof(good));
        expect(v, MIMUSB_OK);
        v.class_bytes.assign(zero_length, zero_length + sizeof(zero_length));
        expect(v, MIMUSB_ERR_PARAM, "descriptor");
        v.class_bytes.assign(too_long, too_long + sizeof(too_long));
        expect(v, MIMUSB_ERR_PARAM, "descriptor");
    }
    // Association: 8 bytes of type 0x0B naming this Node's interfaces.
    {
        Vendor v = base();
        v.refresh();
        const std::uint8_t short_assoc[] = {8, 0x0B, 0};
        v.config.association = short_assoc;
        v.config.association_length = sizeof(short_assoc);
        expect(v, MIMUSB_ERR_PARAM, "association");
        const std::uint8_t wrong_type[] = {8, 0x04, 0, 1, 0xFF, 0, 0, 0};
        v.config.association = wrong_type;
        v.config.association_length = sizeof(wrong_type);
        expect(v, MIMUSB_ERR_PARAM, "association");
        const std::uint8_t wrong_count[] = {8, 0x0B, 0, 2, 0xFF, 0, 0, 0};
        v.config.association = wrong_count;
        v.config.association_length = sizeof(wrong_count);
        expect(v, MIMUSB_ERR_PARAM, "association");
        const std::uint8_t wrong_first[] = {8, 0x0B, 3, 1, 0xFF, 0, 0, 0};
        v.config.association = wrong_first;
        v.config.association_length = sizeof(wrong_first);
        expect(v, MIMUSB_ERR_PARAM, "association");
        const std::uint8_t right[] = {8, 0x0B, 0, 1, 0xFF, 0, 0, 0};
        v.config.association = right;
        v.config.association_length = sizeof(right);
        expect(v, MIMUSB_OK);
    }
    // An endpoint address used by another Node.
    {
        Vendor first = base();
        mimusb_node* held = nullptr;
        MIMUSB_CHECK(open_vendor(device, &first, &held) == MIMUSB_OK);
        Vendor second({endpoint(0x81, MIMUSB_TRANSFER_BULK)}, 1);
        expect(second, MIMUSB_ERR_PARAM, "address");
        Vendor third({endpoint(0x82, MIMUSB_TRANSFER_BULK)}, 1);
        expect(third, MIMUSB_OK);
        if (held != nullptr)
            MIMUSB_CHECK(mimusb_node_close(held, &kCommit, kDrainMs) == MIMUSB_OK);
    }
    // The strings.
    {
        mimusb_device_config o = base_options();
        (void)o;
        uint8_t index = 0;
        MIMUSB_CHECK(mimusb_device_add_string(device, nullptr, &index) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_device_add_string(device, "x", nullptr) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_device_add_string(device, "a\nb", &index) == MIMUSB_ERR_PARAM);
        MIMUSB_CHECK(mimusb_device_remove_string(device, 9) == MIMUSB_ERR_PARAM);
    }
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// An index from mimusb_device_add_string, written into a class descriptor, is a string the target
// reads; an interface name takes another index.
void strings() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    uint8_t first = 0, second = 0;
    MIMUSB_CHECK(mimusb_device_add_string(device, "Control Interface", &first) == MIMUSB_OK &&
                 first == 5);
    MIMUSB_CHECK(mimusb_device_add_string(device, "Ünï \xF0\x9F\x98\x80", &second) == MIMUSB_OK &&
                 second == 6);
    const std::uint8_t desc[] = {5, 0x24, 0x01, first, 0};
    Vendor v({endpoint(0x81, MIMUSB_TRANSFER_BULK)});
    v.itf.name = "Data";
    v.class_bytes.assign(desc, desc + sizeof(desc));
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(open_vendor(device, &v, &node) == MIMUSB_OK);
    char text[64];
    MIMUSB_CHECK(fake_board_string(5, text, sizeof(text)) &&
                 std::strcmp(text, "Control Interface") == 0);
    MIMUSB_CHECK(fake_board_string(6, text, sizeof(text)) &&
                 std::strcmp(text, "Ünï \xF0\x9F\x98\x80") == 0);
    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    // The Interface descriptor's iInterface is the next free index; the class descriptor the
    // caller wrote follows it unchanged.
    MIMUSB_CHECK(c[9 + 8] == 7 && std::memcmp(c + 18, desc, sizeof(desc)) == 0);
    MIMUSB_CHECK(fake_board_string(7, text, sizeof(text)) && std::strcmp(text, "Data") == 0);
    // Removing a string takes it off the next commit.
    MIMUSB_CHECK(mimusb_device_remove_string(device, second) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_commit(device, 0, &kCommit) == MIMUSB_OK);
    // The name of the interface takes the lowest index nothing else has: 6 is free now.
    MIMUSB_CHECK(fake_board_string(6, text, sizeof(text)) && std::strcmp(text, "Data") == 0 &&
                 fake_board_string(7, text, sizeof(text)) == 0);
    uint8_t again = 0;
    MIMUSB_CHECK(mimusb_device_add_string(device, "again", &again) == MIMUSB_OK && again == 6);
    if (node != nullptr)
        MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

#if SIM_ENDPOINTS >= 4
// The shape of CDC-ACM (USB CDC 1.2 PSTN): a communications interface with an interrupt IN
// endpoint, a data interface with bulk IN and OUT, held by an association, beside two HID Nodes.
void association_beside_hid() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(0);
    mimusb_spec* mouse = mouse_spec(0);
    test_hid_node_config nc{};
    test_fill_node_config(&nc, 0);
    mimusb_node* k = open_node(device, kbd, &nc);
    test_fill_node_config(&nc, 1);
    mimusb_node* m = open_node(device, mouse, &nc);

    mimusb_cdc_acm_descriptors cdc_bytes{};
    MIMUSB_CHECK(mimusb_cdc_acm_descriptors_make(0x0110, 2, 0x00, 0x02, &cdc_bytes) == MIMUSB_OK);
    const std::uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; // 115200 8N1
    mimusb_control_rule cdc_rules[3];
    MIMUSB_CHECK(mimusb_cdc_acm_rules(2, line_coding, cdc_rules) == MIMUSB_OK);
    const mimusb_usb_endpoint comm_ep =
        endpoint(0x83, MIMUSB_TRANSFER_INTERRUPT, MIMUSB_FULL_DROP_NEWEST, 2);
    const mimusb_usb_item comm_items[] = {
        mimusb_usb_item_make_descriptor(cdc_bytes.header, sizeof(cdc_bytes.header)),
        mimusb_usb_item_make_descriptor(cdc_bytes.call_management,
                                        sizeof(cdc_bytes.call_management)),
        mimusb_usb_item_make_descriptor(cdc_bytes.acm, sizeof(cdc_bytes.acm)),
        mimusb_usb_item_make_descriptor(cdc_bytes.union_, sizeof(cdc_bytes.union_)),
        mimusb_usb_item_make_endpoint(comm_ep)};
    const mimusb_usb_item data_items[] = {
        mimusb_usb_item_make_endpoint(endpoint(0x84, MIMUSB_TRANSFER_BULK, MIMUSB_FULL_WAIT, 4)),
        mimusb_usb_item_make_endpoint(endpoint(0x04, MIMUSB_TRANSFER_BULK))};
    const mimusb_usb_setting comm_setting = {comm_items, 5};
    const mimusb_usb_setting data_setting = {data_items, 2};
    mimusb_usb_interface itfs[2] = {};
    itfs[0].number = 2;
    itfs[0].class_code = 0x02;
    itfs[0].subclass = 0x02;
    itfs[0].protocol = 0x01;
    itfs[0].settings = &comm_setting;
    itfs[0].setting_count = 1;
    itfs[1].number = 3;
    itfs[1].class_code = 0x0A;
    itfs[1].settings = &data_setting;
    itfs[1].setting_count = 1;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = itfs;
    config.interface_count = 2;
    config.association = cdc_bytes.association;
    config.association_length = sizeof(cdc_bytes.association);
    config.rules = cdc_rules;
    config.rule_count = 3;
    mimusb_node* cdc = nullptr;
    MIMUSB_CHECK(test_node_open_interfaces(device, &config, &kCommit, &cdc) == MIMUSB_OK &&
                 cdc != nullptr);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));

    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMUSB_CHECK(c[4] == 4); // bNumInterfaces
    // The association sits right before interface 2, after the two HID interfaces' descriptors.
    bool found = false;
    for (std::uint16_t i = 9; i < length; i += c[i]) {
        if (c[i + 1] == 0x0B) {
            found = std::memcmp(c + i, cdc_bytes.association, 8) == 0 && c[i + 8 + 1] == 4 &&
                    c[i + 8 + 2] == 2;
        }
    }
    MIMUSB_CHECK(found && fake_board_interface_count() == 4);

    // The HID Nodes work as before; the CDC Node moves data both ways.
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_keyboard_key(k, 0x04, 1) == MIMUSB_OK &&
                 mimusb_node_submit(k, kSendMs) == MIMUSB_OK);
    fake_report_t r;
    MIMUSB_CHECK(next_report(&r) && r.iface == 0);
    const std::uint8_t line[] = {0xA1, 0x20, 0, 0, 2, 0, 0, 0};
    MIMUSB_CHECK(mimusb_node_endpoint_write(cdc, 0x83, line, sizeof(line), kSendMs) == MIMUSB_OK);
    fake_transfer_t t;
    MIMUSB_CHECK(fake_board_wait_transfer(0x83, &t, 1000) && t.length == sizeof(line));
    const std::uint8_t text[] = "AT\r\n";
    MIMUSB_CHECK(mimusb_node_endpoint_write(cdc, 0x84, text, 4, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x84, &t, 1000) && t.length == 4 && t.data[0] == 'A');
    fake_board_target_out(0x04, text, 4);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x04 &&
                 ev.interface_number == 3 && ev.node_id == mimusb_node_id(cdc));

    // The line-coding rules: the target reads the coding the program gave, and a new one it sets
    // is accepted and reported with its data stage.
    {
        const std::uint8_t get[8] = {0xA1, 0x21, 0, 0, 2, 0, 7, 0};
        fake_board_target_control(get, nullptr, 0);
        fake_control_result_t res{};
        MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 &&
                     res.outcome == FAKE_CONTROL_ANSWERED && res.length == 7 &&
                     std::memcmp(res.data, line_coding, 7) == 0);
        const std::uint8_t set[8] = {0x21, 0x20, 0, 0, 2, 0, 7, 0};
        const std::uint8_t next[7] = {0x00, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x08}; // 19200 8N1
        fake_board_target_control(set, next, sizeof(next));
        MIMUSB_CHECK(fake_board_wait_control(&res, 2000) != 0 && res.outcome == FAKE_CONTROL_ACKED);
        MIMUSB_CHECK(wait_event(MIMUSB_EVENT_CONTROL_DATA, &ev) && ev.data.size() == 7 &&
                     std::memcmp(ev.data.data(), next, 7) == 0);
    }

    // Closing the CDC Node leaves the HID ones (interfaces 0 and 1) as they were.
    MIMUSB_CHECK(mimusb_node_close(cdc, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 2);
    if (m != nullptr)
        MIMUSB_CHECK(mimusb_node_close(m, &kCommit, kDrainMs) == MIMUSB_OK);
    if (k != nullptr)
        MIMUSB_CHECK(mimusb_node_close(k, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
    mimusb_spec_release(mouse);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}
#endif

} // namespace

void test_generic_nodes() {
    vendor_bulk();
    policies();
    open_checks();
    strings();
#if SIM_ENDPOINTS >= 4
    association_beside_hid();
#endif
}

} // namespace sim
