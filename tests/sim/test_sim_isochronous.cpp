// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Alternate settings and isochronous endpoints of a Device given as bytes, against the firmware
 * core built with isochronous endpoints (SIM_ISO: MIMHID_ISO_BYTES 1023, as the RP2040 build has).
 * A streaming interface has setting 0 with no endpoint and settings 1 and 2 with an isochronous IN
 * endpoint of 192 and 384 bytes (an audio-class endpoint descriptor of 9 bytes), and another has
 * setting 1 with an isochronous OUT endpoint. The target selects settings with SET_INTERFACE, and
 * the board does what the target does and nothing else: it starts and stops the endpoints, tells
 * the program, and sends what the program wrote. */
#include "sim_support.hpp"

#ifdef SIM_ISO

#include <cstring>
#include <vector>

namespace sim {

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes device_bytes() {
    return {18, 1, 0x10, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x02, 0x00, 0x00, 0x01, 0, 0, 0, 1};
}

// Interface 0: setting 0 (no endpoint), 1 (iso IN 0x81, 192 bytes), 2 (the same address, 384
// bytes). Interface 1: setting 0 (a bulk OUT 0x02), 1 (that endpoint and an iso OUT 0x03, 100
// bytes).
Bytes config_bytes() {
    Bytes c = {9, 2, 0, 0, 2, 1, 0, 0x80, 50};
    const Bytes rest = {
        9, 4, 0,    0,    0,    0xFF, 1, 2, 0, // interface 0, setting 0
        9, 4, 0,    1,    1,    0xFF, 1, 2, 0, // setting 1
        9, 5, 0x81, 0x05, 0xC0, 0x00, 1, 0, 0, // iso IN, 192 bytes
        9, 4, 0,    2,    1,    0xFF, 1, 2, 0, // setting 2
        9, 5, 0x81, 0x05, 0x80, 0x01, 1, 0, 0, // iso IN, 384 bytes
        9, 4, 1,    0,    1,    0xFF, 1, 2, 0, // interface 1, setting 0
        7, 5, 0x02, 0x02, 64,   0,    0,       // bulk OUT 0x02
        9, 4, 1,    1,    2,    0xFF, 1, 2, 0, // setting 1
        7, 5, 0x02, 0x02, 64,   0,    0,       // the bulk endpoint again
        9, 5, 0x03, 0x05, 100,  0x00, 1, 0, 0, // iso OUT, 100 bytes
    };
    c.insert(c.end(), rest.begin(), rest.end());
    c[2] = static_cast<std::uint8_t>(c.size());
    return c;
}

// The outcome of the control request the board's driver took last.
bool control_outcome(int outcome, fake_control_result_t* r) {
    return fake_board_wait_control(r, 2000) && r->outcome == outcome;
}

bool select_setting(std::uint8_t itf, std::uint8_t setting, int outcome = FAKE_CONTROL_ACKED) {
    fake_control_result_t r;
    fake_board_target_set_interface(itf, setting);
    return control_outcome(outcome, &r);
}

int selected_setting(std::uint8_t itf) {
    fake_control_result_t r;
    fake_board_target_get_interface(itf);
    return control_outcome(FAKE_CONTROL_ANSWERED, &r) && r.length == 1U ? r.data[0] : -1;
}

void streaming() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMHID_CHECK(mimhid_device_limits(device, &limits) == MIMHID_OK);
    MIMHID_CHECK((limits.transfer_types & (1U << MIMHID_TRANSFER_ISOCHRONOUS)) != 0U &&
                 limits.max_transfer_bytes == 1023U && limits.max_endpoint_bytes == 64U);

    const Bytes dev = device_bytes();
    const Bytes cfg = config_bytes();
    mimhid_device_descriptors d{};
    d.struct_size = sizeof(d);
    d.device = dev.data();
    d.device_length = dev.size();
    d.configuration = cfg.data();
    d.configuration_length = cfg.size();
    const mimhid_endpoint_policy keep = {MIMHID_FULL_DROP_OLDEST, 4};
    MIMHID_CHECK(mimhid_device_endpoint_set_policy(device, 0x81, &keep, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_set_descriptors(device, &d, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    EventRecord ev;
    MIMHID_CHECK(wait_event(MIMHID_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // The target reads the descriptors as given; both interfaces start in setting 0.
    std::uint16_t length = 0;
    const std::uint8_t* got = fake_board_config_descriptor(&length);
    MIMHID_CHECK(length == cfg.size() && std::memcmp(got, cfg.data(), cfg.size()) == 0);
    MIMHID_CHECK(selected_setting(0) == 0 && selected_setting(1) == 0);
    MIMHID_CHECK(fake_board_endpoint_packet(0x81) == 0U && fake_board_endpoint_packet(0x03) == 0U &&
                 fake_board_endpoint_packet(0x02) == 64U);

    // Nothing is on the bus in setting 0: what the program writes waits.
    fake_transfer_t t;
    Bytes audio(192);
    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] = static_cast<std::uint8_t>(i);
    MIMHID_CHECK(mimhid_device_endpoint_write(device, 0x81, audio.data(), audio.size(), kSendMs) ==
                 MIMHID_OK);
    MIMHID_CHECK(!fake_board_wait_transfer(0x81, &t, 150));

    // Setting 1: the endpoint is on the bus at 192 bytes, the board tells the program, and the
    // packet that waited goes out. What was armed on the controller when the target left a setting
    // is taken off it (the RP2040's driver stops when the next transfer is armed on top of it), at
    // the stop and at the start.
    const unsigned cleared = fake_board_clear_endpoint_count(0x81);
    MIMHID_CHECK(select_setting(0, 1));
    MIMHID_CHECK(fake_board_clear_endpoint_count(0x81) == cleared + 1U);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev) && ev.interface_number == 0U &&
                 ev.value == 1U && ev.node_id == 0U);
    MIMHID_CHECK(selected_setting(0) == 1 && fake_board_endpoint_packet(0x81) == 192U);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 192U &&
                 std::memcmp(t.data, audio.data(), 192) == 0);
    // A packet each frame of the bus, exactly as written, in order; a short one is a short one.
    for (std::uint8_t n = 1; n <= 3; ++n) {
        Bytes p(n == 3 ? 10 : 192, n);
        MIMHID_CHECK(mimhid_device_endpoint_write(device, 0x81, p.data(), p.size(), kSendMs) ==
                     MIMHID_OK);
        MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == p.size() &&
                     t.data[0] == n);
    }
    // An isochronous transfer is one packet: the library refuses more than the endpoint's packet.
    Bytes more(385, 0);
    MIMHID_CHECK(mimhid_device_endpoint_write(device, 0x81, more.data(), more.size(), kSendMs) ==
                 MIMHID_ERR_PARAM);
    MIMHID_CHECK(std::strcmp(mimhid_last_error()->field, "node.endpoint.write") == 0);

    // Setting 2: the same endpoint with the bigger packet; the setting before it is left.
    const unsigned before_two = fake_board_clear_endpoint_count(0x81);
    MIMHID_CHECK(select_setting(0, 2));
    MIMHID_CHECK(fake_board_clear_endpoint_count(0x81) ==
                 before_two + 2U); // setting 1 stopped, 2 started
    MIMHID_CHECK(wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 2U);
    MIMHID_CHECK(selected_setting(0) == 2 && fake_board_endpoint_packet(0x81) == 384U);
    Bytes big(384, 0x5A);
    MIMHID_CHECK(mimhid_device_endpoint_write(device, 0x81, big.data(), big.size(), kSendMs) ==
                 MIMHID_OK);
    MIMHID_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 384U);

    // Back to setting 0: the endpoint stops; what the program writes waits again, and the same
    // setting selected twice is told twice.
    MIMHID_CHECK(select_setting(0, 0));
    MIMHID_CHECK(wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 0U);
    MIMHID_CHECK(mimhid_device_endpoint_write(device, 0x81, audio.data(), 8, kSendMs) == MIMHID_OK);
    MIMHID_CHECK(!fake_board_wait_transfer(0x81, &t, 150));
    MIMHID_CHECK(select_setting(0, 0));
    MIMHID_CHECK(wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 0U);

    // A setting the interface does not have, and an interface that is not there, are stalled and
    // change nothing.
    MIMHID_CHECK(select_setting(0, 3, FAKE_CONTROL_STALLED));
    MIMHID_CHECK(select_setting(5, 0, FAKE_CONTROL_STALLED));
    MIMHID_CHECK(selected_setting(0) == 0);
    MIMHID_CHECK(!wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev, 100));

    // An isochronous OUT endpoint: nothing in setting 0 of its interface (the bulk one is open all
    // along), the packets of the target in setting 1, as they came.
    const Bytes in = {1, 2, 3, 4, 5};
    MIMHID_CHECK(fake_board_endpoint_packet(0x03) == 0U);
    MIMHID_CHECK(select_setting(1, 1));
    MIMHID_CHECK(wait_event(MIMHID_EVENT_INTERFACE_ALTERNATE, &ev) && ev.interface_number == 1U &&
                 ev.value == 1U);
    MIMHID_CHECK(fake_board_endpoint_packet(0x03) == 100U &&
                 fake_board_endpoint_packet(0x02) == 64U);
    fake_board_target_out(0x03, in.data(), in.size());
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x03U &&
                 ev.interface_number == 1U && ev.data == in);
    fake_board_target_out(0x02, in.data(), 3);
    MIMHID_CHECK(wait_event(MIMHID_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x02U &&
                 ev.data.size() == 3U);

    // A bus reset in the target (the device is taken off and put back) starts in setting 0 again.
    const unsigned closed = fake_board_close_all_count();
    const unsigned resets = fake_board_bus_reset_count();
    MIMHID_CHECK(mimhid_device_clear_descriptors(device, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(!fake_board_mounted());
    // The detach gave the controller's packet memory back and made the stack forget the
    // configuration, without waiting for a bus reset that a target that missed it never sends.
    MIMHID_CHECK(fake_board_close_all_count() == closed + 1U &&
                 fake_board_bus_reset_count() == resets + 1U);
    // (Clearing forgets the policies too.)
    MIMHID_CHECK(mimhid_device_endpoint_set_policy(device, 0x81, &keep, kControlMs) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_set_descriptors(device, &d, &kCommit) == MIMHID_OK);
    MIMHID_CHECK(mimhid_device_wait_ready(device, 2000) == MIMHID_OK);
    MIMHID_CHECK(selected_setting(0) == 0 && selected_setting(1) == 0);
    MIMHID_CHECK(fake_board_endpoint_packet(0x81) == 0U && fake_board_endpoint_packet(0x03) == 0U);

    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

// The refusals that are about the board's limits: a Node has no isochronous endpoint, and the
// packet memory of the controller bounds a configuration of them.
void refusals() {
    mimhid_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimhid_usb_endpoint ep{};
    ep.address = 0x81;
    ep.transfer_type = MIMHID_TRANSFER_ISOCHRONOUS;
    ep.max_packet_size = 64;
    ep.interval = 1;
    ep.policy.on_full = MIMHID_FULL_DROP_NEWEST;
    ep.policy.depth = 2;
    mimhid_usb_interface itf{};
    itf.number = 0;
    itf.class_code = 0xFF;
    itf.endpoints = &ep;
    itf.endpoint_count = 1;
    mimhid_node_config config;
    mimhid_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    mimhid_node* node = nullptr;
    MIMHID_CHECK(mimhid_node_open(device, &config, &kCommit, &node) == MIMHID_ERR_UNSUPPORTED &&
                 node == nullptr);
    MIMHID_CHECK(mimhid_device_close(device, kControlMs, kDrainMs) == MIMHID_OK);
}

} // namespace

void test_isochronous() {
    streaming();
    refusals();
}

} // namespace sim

#endif // SIM_ISO
