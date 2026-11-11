// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Nodes of any class (mimhid_node_open) against the firmware core: bulk and interrupt endpoints,
 * the IN policies, packets from the target, the Halt feature, strings, an interface association
 * beside HID Nodes, and every check of the open call. The target is the simulated one of
 * fake_board.c: it polls the IN endpoints unless reports are held. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimhid_proto.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace sim {

namespace {

mimhid_usb_endpoint endpoint(std::uint8_t address, std::uint8_t type, std::uint8_t on_full = 0,
                             std::uint8_t depth = 0) {
    mimhid_usb_endpoint ep{};
    ep.address = address;
    ep.transfer_type = type;
    ep.max_packet_size = type == MIMHID_TRANSFER_BULK ? 64 : 16;
    ep.interval = type == MIMHID_TRANSFER_INTERRUPT ? 4 : 0;
    if ((address & 0x80U) != 0U) {
        ep.policy.on_full = on_full != 0 ? on_full : MIMHID_FULL_WAIT;
        ep.policy.depth = depth != 0 ? depth : 4;
    }
    return ep;
}

// One vendor-class interface (class 0xFF) with the given endpoints.
struct Vendor {
    std::vector<mimhid_usb_endpoint> eps;
    mimhid_usb_interface itf{};
    mimhid_node_config config;
    explicit Vendor(std::vector<mimhid_usb_endpoint> endpoints, std::uint8_t number = 0)
        : eps(std::move(endpoints)) {
        itf.number = number;
        itf.class_code = 0xFF;
        itf.subclass = 1;
        itf.protocol = 2;
        itf.endpoints = eps.data();
        itf.endpoint_count = eps.size();
        mimhid_node_config_init(&config);
        config.interfaces = &itf;
        config.interface_count = 1;
    }
    void refresh() {
        config.interfaces = &itf; // after a copy it would point at the other object
        itf.endpoints = eps.data();
        itf.endpoint_count = eps.size();
    }
};

mimhid_result open_vendor(mimhid_device* device, Vendor* v, mimhid_node** node) {
    v->refresh();
    return mimhid_node_open(device, &v->config, &kCommit, node);
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
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    Vendor v({endpoint(0x81, MIMHID_TRANSFER_BULK, MIMHID_FULL_DROP_NEWEST, 8),
              endpoint(0x01, MIMHID_TRANSFER_BULK)});
    mimhid_node* node = nullptr;
    MIMHID_CHECK(open_vendor(device, &v, &node) == MIMHID_OK && node != nullptr);
    if (node == nullptr) {
        MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
        return;
    }
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // The interface the target reads: class bytes as given, two endpoints.
    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMHID_CHECK(length == 9 + 9 + 7 + 7 && c[4] == 1 && c[9 + 4] == 2 && c[9 + 5] == 0xFF &&
                 c[9 + 6] == 1 && c[9 + 7] == 2 && c[18] == 7 && c[18 + 1] == 5 &&
                 c[18 + 2] == 0x81 && c[18 + 3] == 2 && c[25 + 2] == 0x01);

    // In order, as written.
    fake_transfer_t t;
    const std::uint8_t a[] = {'h', 'e', 'l', 'l', 'o'};
    const std::uint8_t b[] = {1, 2, 3};
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && bytes(t) == std::vector<std::uint8_t>(a, a + 5));
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && bytes(t) == std::vector<std::uint8_t>(b, b + 3));

    // A zero-length packet (USB 2.0 5.8.3).
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, nullptr, 0, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 0);

    // An exact multiple of the packet size: the library adds no zero-length packet.
    std::vector<std::uint8_t> full(128);
    for (std::size_t i = 0; i < full.size(); ++i)
        full[i] = static_cast<std::uint8_t>(i);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, full.data(), full.size(), kSendMs) ==
                 MIMHID_OK);
    std::vector<std::uint8_t> got;
    while (got.size() < 128 && fake_board_wait_transfer(0x81, &t, 1000)) {
        MIMHID_CHECK(t.length == 64);
        got.insert(got.end(), t.data, t.data + t.length);
    }
    MIMHID_CHECK(got == full && !fake_board_wait_transfer(0x81, &t, 150));

    // Longer than max_transfer_bytes (64 here): split on packet boundaries, in order.
    mimhid_board_limits limits;
    mimhid_board_limits_init(&limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK &&
                 limits.max_transfer_bytes == 64);
    std::vector<std::uint8_t> big(200);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 3U);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, big.data(), big.size(), kSendMs) ==
                 MIMHID_OK);
    got.clear();
    unsigned parts = 0;
    while (got.size() < big.size() && fake_board_wait_transfer(0x81, &t, 1000)) {
        got.insert(got.end(), t.data, t.data + t.length);
        ++parts;
    }
    MIMHID_CHECK(got == big && parts == 4);

    // Bytes from the target arrive as events, in order; a zero-length packet too.
    for (std::uint8_t i = 0; i < 5; ++i) {
        const std::uint8_t packet[3] = {i, static_cast<std::uint8_t>(i + 1), 9};
        fake_board_target_out(0x01, packet, sizeof(packet));
    }
    fake_board_target_out(0x01, nullptr, 0);
    for (std::uint8_t i = 0; i < 5; ++i) {
        MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x01 &&
                     ev.interface_number == 0 && ev.node_id == mimhid_node_id(node) &&
                     ev.data.size() == 3 && ev.data[0] == i);
    }
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.data.empty());

    // OUT packets burst while the board's link is busy with other writes: none is lost.
    for (unsigned i = 0; i < 40; ++i) {
        const std::uint8_t packet[2] = {static_cast<std::uint8_t>(i), 0x5A};
        fake_board_target_out(0x01, packet, sizeof(packet));
    }
    for (unsigned i = 0; i < 40; ++i) {
        MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev, 3000) && ev.data.size() == 2 &&
                     ev.data[0] == i);
    }

    // The program stalls an IN endpoint: what is written waits; clearing lets it go.
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x81, kControlMs) == MIMHID_OK &&
                 fake_board_endpoint_stalled(0x81));
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(!fake_board_wait_transfer(0x81, &t, 150));
    MIMHID_CHECK(mimhid_node_endpoint_clear_stall(node, 0x81, kControlMs) == MIMHID_OK &&
                 !fake_board_endpoint_stalled(0x81));
    // Stalling dropped what waited (the write above was dropped by the stall order or queued
    // after it; either way the endpoint works again).
    fake_board_clear_transfers();
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && bytes(t) == std::vector<std::uint8_t>(b, b + 3));
    // OUT endpoint: stalled, then accepting again.
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x01, kControlMs) == MIMHID_OK &&
                 fake_board_endpoint_stalled(0x01));
    MIMHID_CHECK(mimhid_node_endpoint_clear_stall(node, 0x01, kControlMs) == MIMHID_OK);
    const std::uint8_t again[1] = {0x77};
    fake_board_target_out(0x01, again, 1);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.data.size() == 1 &&
                 ev.data[0] == 0x77);

    // The target sets and clears the Halt feature (USB 2.0 9.4.5): the event says which.
    fake_board_target_halt(0x81, 1);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_HALT, &ev) && ev.value == 1 &&
                 ev.endpoint_address == 0x81 && ev.node_id == mimhid_node_id(node));
    fake_board_target_halt(0x81, 0);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_HALT, &ev) && ev.value == 0);
    fake_board_target_halt(0x01, 1);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_HALT, &ev) && ev.value == 1 &&
                 ev.endpoint_address == 0x01);
    fake_board_target_halt(0x01, 0);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_HALT, &ev) && ev.value == 0);

    // A flush drops what waits on the board; the transfer on the bus is read.
    fake_board_clear_transfers();
    fake_board_hold_reports(1);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, a, sizeof(a), kSendMs) == MIMHID_OK);
    MIMHID_CHECK(wait_busy(0x81));
    for (int i = 0; i < 3; ++i)
        MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, b, sizeof(b), kSendMs) == MIMHID_OK);
    sleep_ms(50);
    MIMHID_CHECK(mimhid_node_endpoint_flush(node, 0x81, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_flush(node, 0x01, kControlMs) == MIMHID_ERR_PARAM);
    fake_board_hold_reports(0);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 5);
    MIMHID_CHECK(!fake_board_wait_transfer(0x81, &t, 150));

    // Calls that do not apply.
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x01, a, 1, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x85, a, 1, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x81, nullptr, 1, kSendMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x85, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_stall(node, 0x81, 0) == MIMHID_ERR_UNSET_FIELD);
    MIMHID_CHECK(mimhid_report_send(node, a, 1, kSendMs) == MIMHID_ERR_UNSUPPORTED);

    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The three policies with a target that does not poll, and the wait policy with one that does.
void policies() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    MIMHID_CHECK(mimhid_device_set_loss_reporting(device, 1, 0, kControlMs) == MIMHID_OK);
    Vendor v({endpoint(0x81, MIMHID_TRANSFER_BULK, MIMHID_FULL_DROP_NEWEST, 2),
              endpoint(0x82, MIMHID_TRANSFER_BULK, MIMHID_FULL_DROP_OLDEST, 2),
              endpoint(0x83, MIMHID_TRANSFER_BULK, MIMHID_FULL_WAIT, 2)});
    mimhid_node* node = nullptr;
    MIMHID_CHECK(open_vendor(device, &v, &node) == MIMHID_OK);
    if (node == nullptr) {
        MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
        return;
    }
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));

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
        MIMHID_CHECK(mimhid_node_endpoint_write(node, address, first.data(), 1, kSendMs) ==
                     MIMHID_OK);
        MIMHID_CHECK(wait_busy(address));
        for (const char c : {'B', 'C', 'D', 'E'}) {
            const auto d = one(c);
            MIMHID_CHECK(mimhid_node_endpoint_write(node, address, d.data(), 1, kSendMs) ==
                         MIMHID_OK);
        }
    }
    sleep_ms(100);
    fake_board_hold_reports(0);
    MIMHID_CHECK(drain(0x81) == "ABC");  // the newest, D and E, were dropped
    MIMHID_CHECK(drain(0x82) == "ADE");  // the oldest that was not being sent, B then C, dropped
    // The losses are reported with the endpoint.
    unsigned lost81 = 0, lost82 = 0;
    while (wait_event(MIMHID_EVENT_TRANSFER_LOST, &ev, 300)) {
        if (ev.endpoint_address == 0x81)
            lost81 += ev.value;
        if (ev.endpoint_address == 0x82)
            lost82 += ev.value;
        MIMHID_CHECK(ev.node_id == mimhid_node_id(node));
    }
    MIMHID_CHECK(lost81 == 2 && lost82 == 2);

    // Wait: nothing is dropped, the third write blocks and times out (nothing sent).
    fake_board_hold_reports(1);
    const auto a = one('A');
    const auto b = one('B');
    const auto c = one('C');
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x83, a.data(), 1, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x83, b.data(), 1, kSendMs) == MIMHID_OK);
    const auto begin = std::chrono::steady_clock::now();
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x83, c.data(), 1, 200) == MIMHID_ERR_TIMEOUT);
    MIMHID_CHECK(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(180));
    MIMHID_CHECK(mimhid_node_endpoint_write(node, 0x83, c.data(), 1, 0) == MIMHID_ERR_UNSET_FIELD);
    // A blocked writer is woken by a flush and gets BUSY, with nothing sent.
    mimhid_result blocked = MIMHID_OK;
    std::thread writer([&] { blocked = mimhid_node_endpoint_write(node, 0x83, c.data(), 1, 5000); });
    sleep_ms(150);
    MIMHID_CHECK(mimhid_node_endpoint_flush(node, 0x83, kControlMs) == MIMHID_OK);
    writer.join();
    MIMHID_CHECK(blocked == MIMHID_ERR_BUSY);
    fake_board_hold_reports(0);
    MIMHID_CHECK(drain(0x83) == "A"); // the one on the bus was read; B was flushed

    // Wait with a polling target: a long burst loses nothing and stays in order.
    fake_board_clear_transfers();
    constexpr unsigned kBurst = 1500;
    std::thread reader_done;
    mimhid_result burst = MIMHID_OK;
    std::thread burst_writer([&] {
        for (unsigned i = 0; i < kBurst && burst == MIMHID_OK; ++i) {
            const std::uint8_t d[4] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8), 0, 0};
            burst = mimhid_node_endpoint_write(node, 0x83, d, sizeof(d), 5000);
        }
    });
    unsigned seen = 0;
    fake_transfer_t t;
    while (seen < kBurst && fake_board_wait_transfer(0x83, &t, 3000)) {
        MIMHID_CHECK(t.length == 4 && (t.data[0] | (t.data[1] << 8)) == static_cast<int>(seen));
        ++seen;
    }
    burst_writer.join();
    MIMHID_CHECK(burst == MIMHID_OK && seen == kBurst);

    // Changing a policy: refused when the queue cannot hold it, accepted when it can.
    mimhid_endpoint_policy p{MIMHID_FULL_DROP_NEWEST, 255};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMHID_ERR_OVERFLOW);
    p = {MIMHID_FULL_DROP_OLDEST, 1};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMHID_OK);
    p = {0, 1};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMHID_ERR_UNSET_FIELD);
    p = {4, 1};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x81, &p, kControlMs) == MIMHID_ERR_PARAM);
    p = {MIMHID_FULL_WAIT, 1};
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x01, &p, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(nullptr, 0x81, &p, kControlMs) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_endpoint_set_policy(node, 0x81, &p, 0) == MIMHID_ERR_UNSET_FIELD);

    MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// Every check of the open call. Each refusal leaves the Device as it was.
void open_checks() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_node* node = nullptr;
    const auto expect = [&](Vendor& v, mimhid_result want, const char* field = nullptr,
                            int line = __builtin_LINE()) {
        node = nullptr;
        const mimhid_result r = open_vendor(device, &v, &node);
        MIMHID_CHECK(r == want && (want == MIMHID_OK || node == nullptr));
        if (r != want)
            std::fprintf(stderr, "open check at line %d: %s, wanted %s (%s)\n", line,
                         mimhid_result_name(r), mimhid_result_name(want), mimhid_last_error()->reason);
        if (field != nullptr && r != MIMHID_OK)
            MIMHID_CHECK(std::strstr(mimhid_last_error()->field, field) != nullptr);
        if (node != nullptr)
            MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    };
    const auto base = [] {
        return Vendor({endpoint(0x81, MIMHID_TRANSFER_BULK), endpoint(0x01, MIMHID_TRANSFER_BULK)});
    };

    MIMHID_CHECK(mimhid_node_open(device, nullptr, &kCommit, &node) == MIMHID_ERR_PARAM);
    MIMHID_CHECK(mimhid_node_open(nullptr, nullptr, &kCommit, &node) == MIMHID_ERR_PARAM);
    {
        Vendor v = base();
        v.config.struct_size = 4;
        expect(v, MIMHID_ERR_PARAM);
        v = base();
        v.config.reserved = 1;
        expect(v, MIMHID_ERR_PARAM);
        v = base();
        v.config.interface_count = 0;
        expect(v, MIMHID_ERR_PARAM);
        v = base();
        MIMHID_CHECK(mimhid_node_open(device, &v.config, nullptr, &node) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_node_open(device, &v.config, &kCommit, nullptr) == MIMHID_ERR_PARAM);
    }
    // Class: 0 is unset; 0x03 is the profile calls'.
    {
        Vendor v = base();
        v.itf.class_code = 0;
        expect(v, MIMHID_ERR_UNSET_FIELD, "class_code");
        v = base();
        v.itf.class_code = 0x03;
        expect(v, MIMHID_ERR_UNSUPPORTED, "class_code");
        v = base();
        v.itf.class_code = 0x02; // any other class is the caller's
        expect(v, MIMHID_OK);
    }
    // The numbers are 0, 1, 2, ... without a gap: the missing one is named.
    {
        Vendor v({endpoint(0x81, MIMHID_TRANSFER_BULK)}, 1);
        expect(v, MIMHID_ERR_PARAM, "number");
        MIMHID_CHECK(mimhid_last_error()->offset == 0);
    }
    // Endpoint addresses (USB 2.0 9.6.6).
    for (const std::uint8_t bad : {0x00, 0x80, 0x90, 0xC1, 0x41, 0x10}) {
        Vendor v({endpoint(bad, MIMHID_TRANSFER_BULK)});
        expect(v, MIMHID_ERR_PARAM, "address");
    }
    if (SIM_HID_ITFS < 15) {
        const std::uint8_t above = static_cast<std::uint8_t>(SIM_HID_ITFS + 1);
        Vendor v({endpoint(static_cast<std::uint8_t>(0x80U | above), MIMHID_TRANSFER_BULK)});
        expect(v, MIMHID_ERR_OVERFLOW, "address");
        Vendor w({endpoint(above, MIMHID_TRANSFER_BULK)});
        expect(w, MIMHID_ERR_OVERFLOW, "address");
    }
    {
        Vendor v({endpoint(0x81, MIMHID_TRANSFER_BULK), endpoint(0x81, MIMHID_TRANSFER_BULK)});
        expect(v, MIMHID_ERR_PARAM, "address");
        v = Vendor({endpoint(0x81, MIMHID_TRANSFER_BULK), endpoint(0x01, MIMHID_TRANSFER_BULK)});
        v.eps[1].reserved8 = 1;
        expect(v, MIMHID_ERR_PARAM);
    }
    // Transfer type.
    {
        Vendor v = base();
        v.eps[0].transfer_type = 0;
        expect(v, MIMHID_ERR_UNSET_FIELD, "transfer_type");
        v = base();
        v.eps[0].transfer_type = 1; // isochronous: the board has none
        expect(v, MIMHID_ERR_UNSUPPORTED, "transfer_type");
        v = base();
        v.eps[0].transfer_type = 0; // control
        v.eps[0].transfer_type = 4;
        expect(v, MIMHID_ERR_PARAM, "transfer_type");
    }
    // Packet size.
    {
        Vendor v = base();
        v.eps[0].max_packet_size = 0;
        expect(v, MIMHID_ERR_UNSET_FIELD, "max_packet_size");
        v = base();
        v.eps[0].max_packet_size = 65;
        expect(v, MIMHID_ERR_OVERFLOW, "max_packet_size");
        v = base();
        v.eps[0].max_packet_size = 48; // bulk is 8, 16, 32 or 64 at full speed (USB 2.0 5.8.3)
        expect(v, MIMHID_ERR_PARAM, "max_packet_size");
        v = base();
        v.eps[0].max_packet_size = 8;
        expect(v, MIMHID_OK);
    }
    // Interrupt interval; policies.
    {
        Vendor v({endpoint(0x81, MIMHID_TRANSFER_INTERRUPT)});
        v.eps[0].interval = 0;
        expect(v, MIMHID_ERR_UNSET_FIELD, "interval");
        v = Vendor({endpoint(0x81, MIMHID_TRANSFER_INTERRUPT)});
        expect(v, MIMHID_OK);
        v = base();
        v.eps[0].policy = {};
        expect(v, MIMHID_ERR_UNSET_FIELD, "policy");
        v = base();
        v.eps[0].policy.depth = 0;
        expect(v, MIMHID_ERR_UNSET_FIELD, "policy");
        v = base();
        v.eps[0].policy.on_full = 9;
        expect(v, MIMHID_ERR_PARAM, "policy");
        v = base();
        v.eps[1].policy = {MIMHID_FULL_WAIT, 2};
        expect(v, MIMHID_ERR_PARAM, "policy");
        // More queue than the board's pool holds.
        v = base();
        v.eps[0].policy = {MIMHID_FULL_DROP_OLDEST, 255};
        expect(v, MIMHID_ERR_OVERFLOW, "policy");
    }
    // Class descriptors: bLength at least 2 and inside the bytes.
    {
        const std::uint8_t good[] = {5, 0x24, 0, 0x10, 0x01};
        const std::uint8_t zero_length[] = {1, 0x24};
        const std::uint8_t too_long[] = {9, 0x24, 0};
        Vendor v = base();
        v.itf.class_descriptors = good;
        v.itf.class_descriptors_length = sizeof(good);
        expect(v, MIMHID_OK);
        v.itf.class_descriptors = zero_length;
        v.itf.class_descriptors_length = sizeof(zero_length);
        expect(v, MIMHID_ERR_PARAM, "class_descriptors");
        v.itf.class_descriptors = too_long;
        v.itf.class_descriptors_length = sizeof(too_long);
        expect(v, MIMHID_ERR_PARAM, "class_descriptors");
        v.itf.class_descriptors = nullptr;
        v.itf.class_descriptors_length = 3;
        expect(v, MIMHID_ERR_PARAM);
    }
    // Association: 8 bytes of type 0x0B naming this Node's interfaces.
    {
        Vendor v = base();
        const std::uint8_t short_assoc[] = {8, 0x0B, 0};
        v.config.association = short_assoc;
        v.config.association_length = sizeof(short_assoc);
        expect(v, MIMHID_ERR_PARAM, "association");
        const std::uint8_t wrong_type[] = {8, 0x04, 0, 1, 0xFF, 0, 0, 0};
        v.config.association = wrong_type;
        v.config.association_length = sizeof(wrong_type);
        expect(v, MIMHID_ERR_PARAM, "association");
        const std::uint8_t wrong_count[] = {8, 0x0B, 0, 2, 0xFF, 0, 0, 0};
        v.config.association = wrong_count;
        v.config.association_length = sizeof(wrong_count);
        expect(v, MIMHID_ERR_PARAM, "association");
        const std::uint8_t wrong_first[] = {8, 0x0B, 3, 1, 0xFF, 0, 0, 0};
        v.config.association = wrong_first;
        v.config.association_length = sizeof(wrong_first);
        expect(v, MIMHID_ERR_PARAM, "association");
        const std::uint8_t right[] = {8, 0x0B, 0, 1, 0xFF, 0, 0, 0};
        v.config.association = right;
        v.config.association_length = sizeof(right);
        expect(v, MIMHID_OK);
    }
    // An endpoint address used by another Node.
    {
        Vendor first = base();
        mimhid_node* held = nullptr;
        MIMHID_CHECK(open_vendor(device, &first, &held) == MIMHID_OK);
        Vendor second({endpoint(0x81, MIMHID_TRANSFER_BULK)}, 1);
        expect(second, MIMHID_ERR_PARAM, "address");
        Vendor third({endpoint(0x82, MIMHID_TRANSFER_BULK)}, 1);
        expect(third, MIMHID_OK);
        if (held != nullptr)
            MIMHID_CHECK(mimhid_node_close(held, &kCommit, kDrainMs) == MIMHID_OK);
    }
    // The strings.
    {
        mimhid_device_config o = base_options();
        (void)o;
        uint8_t index = 0;
        MIMHID_CHECK(mimhid_device_add_string(device, nullptr, &index) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_device_add_string(device, "x", nullptr) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_device_add_string(device, "a\nb", &index) == MIMHID_ERR_PARAM);
        MIMHID_CHECK(mimhid_device_remove_string(device, 9) == MIMHID_ERR_PARAM);
    }
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// An index from mimhid_device_add_string, written into a class descriptor, is a string the target
// reads; an interface name takes another index.
void strings() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    uint8_t first = 0, second = 0;
    MIMHID_CHECK(mimhid_device_add_string(device, "Control Interface", &first) == MIMHID_OK &&
                 first == 5);
    MIMHID_CHECK(mimhid_device_add_string(device, "Ünï \xF0\x9F\x98\x80", &second) == MIMHID_OK &&
                 second == 6);
    const std::uint8_t desc[] = {5, 0x24, 0x01, first, 0};
    Vendor v({endpoint(0x81, MIMHID_TRANSFER_BULK)});
    v.itf.name = "Data";
    v.itf.class_descriptors = desc;
    v.itf.class_descriptors_length = sizeof(desc);
    mimhid_node* node = nullptr;
    MIMHID_CHECK(open_vendor(device, &v, &node) == MIMHID_OK);
    char text[64];
    MIMHID_CHECK(fake_board_string(5, text, sizeof(text)) && std::strcmp(text, "Control Interface") == 0);
    MIMHID_CHECK(fake_board_string(6, text, sizeof(text)) && std::strcmp(text, "Ünï \xF0\x9F\x98\x80") == 0);
    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    // The Interface descriptor's iInterface is the next free index; the class descriptor the
    // caller wrote follows it unchanged.
    MIMHID_CHECK(c[9 + 8] == 7 && std::memcmp(c + 18, desc, sizeof(desc)) == 0);
    MIMHID_CHECK(fake_board_string(7, text, sizeof(text)) && std::strcmp(text, "Data") == 0);
    // Removing a string takes it off the next commit.
    MIMHID_CHECK(mimhid_device_remove_string(device, second) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_commit(device, 0, &kCommit) == MIMHID_OK);
    // The name of the interface takes the lowest index nothing else has: 6 is free now.
    MIMHID_CHECK(fake_board_string(6, text, sizeof(text)) && std::strcmp(text, "Data") == 0 &&
                 fake_board_string(7, text, sizeof(text)) == 0);
    uint8_t again = 0;
    MIMHID_CHECK(mimhid_device_add_string(device, "again", &again) == MIMHID_OK && again == 6);
    if (node != nullptr)
        MIMHID_CHECK(mimhid_node_close(node, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

#if SIM_HID_ITFS >= 4
// The shape of CDC-ACM (USB CDC 1.2 PSTN): a communications interface with an interrupt IN
// endpoint, a data interface with bulk IN and OUT, held by an association, beside two HID Nodes.
void association_beside_hid() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_spec* kbd = keyboard_spec(0);
    mimhid_spec* mouse = mouse_spec(0);
    mimhid_hid_node_config nc{};
    test_fill_node_config(&nc, 0);
    mimhid_node* k = open_node(device, kbd, &nc);
    test_fill_node_config(&nc, 1);
    mimhid_node* m = open_node(device, mouse, &nc);

    const std::uint8_t header[] = {5, 0x24, 0x00, 0x10, 0x01};
    const std::uint8_t call[] = {5, 0x24, 0x01, 0x00, 0x03};
    const std::uint8_t acm[] = {4, 0x24, 0x02, 0x02};
    const std::uint8_t onion[] = {5, 0x24, 0x06, 0x02, 0x03};
    std::vector<std::uint8_t> comm_class;
    for (const auto* d : {header, call, onion})
        comm_class.insert(comm_class.end(), d, d + 5);
    comm_class.insert(comm_class.end(), acm, acm + 4);
    const mimhid_usb_endpoint comm_eps[] = {endpoint(0x83, MIMHID_TRANSFER_INTERRUPT, MIMHID_FULL_DROP_NEWEST, 2)};
    const mimhid_usb_endpoint data_eps[] = {endpoint(0x84, MIMHID_TRANSFER_BULK, MIMHID_FULL_WAIT, 4),
                                            endpoint(0x04, MIMHID_TRANSFER_BULK)};
    mimhid_usb_interface itfs[2] = {};
    itfs[0].number = 2;
    itfs[0].class_code = 0x02;
    itfs[0].subclass = 0x02;
    itfs[0].protocol = 0x01;
    itfs[0].class_descriptors = comm_class.data();
    itfs[0].class_descriptors_length = comm_class.size();
    itfs[0].endpoints = comm_eps;
    itfs[0].endpoint_count = 1;
    itfs[1].number = 3;
    itfs[1].class_code = 0x0A;
    itfs[1].endpoints = data_eps;
    itfs[1].endpoint_count = 2;
    const std::uint8_t association[] = {8, 0x0B, 2, 2, 0x02, 0x02, 0x01, 0};
    mimhid_node_config config;
    mimhid_node_config_init(&config);
    config.interfaces = itfs;
    config.interface_count = 2;
    config.association = association;
    config.association_length = sizeof(association);
    mimhid_node* cdc = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, &config, &kCommit, &cdc) == MIMHID_OK && cdc != nullptr);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));

    std::uint16_t length = 0;
    const std::uint8_t* c = fake_board_config_descriptor(&length);
    MIMHID_CHECK(c[4] == 4); // bNumInterfaces
    // The association sits right before interface 2, after the two HID interfaces' descriptors.
    bool found = false;
    for (std::uint16_t i = 9; i < length; i += c[i]) {
        if (c[i + 1] == 0x0B) {
            found = std::memcmp(c + i, association, 8) == 0 && c[i + 8 + 1] == 4 && c[i + 8 + 2] == 2;
        }
    }
    MIMHID_CHECK(found && fake_board_interface_count() == 4);

    // The HID Nodes work as before; the CDC Node moves data both ways.
    fake_board_clear_reports();
    MIMHID_CHECK(mimhid_keyboard_key(k, 0x04, 1) == MIMHID_OK && mimhid_node_submit(k, kSendMs) == MIMHID_OK);
    fake_report_t r;
    MIMHID_CHECK(next_report(&r) && r.iface == 0);
    const std::uint8_t line[] = {0xA1, 0x20, 0, 0, 2, 0, 0, 0};
    MIMHID_CHECK(mimhid_node_endpoint_write(cdc, 0x83, line, sizeof(line), kSendMs) == MIMHID_OK);
    fake_transfer_t t;
    MIMHID_CHECK(fake_board_wait_transfer(0x83, &t, 1000) && t.length == sizeof(line));
    const std::uint8_t text[] = "AT\r\n";
    MIMHID_CHECK(mimhid_node_endpoint_write(cdc, 0x84, text, 4, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x84, &t, 1000) && t.length == 4 && t.data[0] == 'A');
    fake_board_target_out(0x04, text, 4);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x04 &&
                 ev.interface_number == 3 && ev.node_id == mimhid_node_id(cdc));

    // Closing the CDC Node leaves the HID ones (interfaces 0 and 1) as they were.
    MIMHID_CHECK(mimhid_node_close(cdc, &kCommit, kDrainMs) == MIMHID_OK);
    MIMHID_CHECK(fake_board_interface_count() == 2);
    if (m != nullptr)
        MIMHID_CHECK(mimhid_node_close(m, &kCommit, kDrainMs) == MIMHID_OK);
    if (k != nullptr)
        MIMHID_CHECK(mimhid_node_close(k, &kCommit, kDrainMs) == MIMHID_OK);
    mimhid_spec_release(kbd);
    mimhid_spec_release(mouse);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}
#endif

} // namespace

void test_generic_nodes() {
    vendor_bulk();
    policies();
    open_checks();
    strings();
#if SIM_HID_ITFS >= 4
    association_beside_hid();
#endif
}

} // namespace sim
