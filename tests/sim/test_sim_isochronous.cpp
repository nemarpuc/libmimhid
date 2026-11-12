// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Alternate settings and isochronous endpoints of a Device given as bytes, against the firmware
 * core built with isochronous endpoints (SIM_ISO: MIMUSB_ISO_BYTES 1023, as the RP2040 build has).
 * A streaming interface has setting 0 with no endpoint and settings 1 and 2 with an isochronous IN
 * endpoint of 192 and 384 bytes (an audio-class endpoint descriptor of 9 bytes), and another has
 * setting 1 with an isochronous OUT endpoint. The target selects settings with SET_INTERFACE, and
 * the board does what the target does and nothing else: it starts and stops the endpoints, tells
 * the program, and sends what the program wrote. */
#include "fill_values.h"
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
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_board_limits limits{};
    limits.struct_size = sizeof(limits);
    MIMUSB_CHECK(mimusb_device_limits(device, &limits) == MIMUSB_OK);
    MIMUSB_CHECK((limits.transfer_types & (1U << MIMUSB_TRANSFER_ISOCHRONOUS)) != 0U &&
                 limits.max_transfer_bytes == 1023U && limits.max_endpoint_bytes == 64U);

    const Bytes dev = device_bytes();
    const Bytes cfg = config_bytes();
    mimusb_device_descriptors d{};
    d.struct_size = sizeof(d);
    d.device = dev.data();
    d.device_length = dev.size();
    d.configuration = cfg.data();
    d.configuration_length = cfg.size();
    const mimusb_endpoint_policy keep = {MIMUSB_FULL_DROP_OLDEST, 4};
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &keep, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, &d, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // The target reads the descriptors as given; both interfaces start in setting 0.
    std::uint16_t length = 0;
    const std::uint8_t* got = fake_board_config_descriptor(&length);
    MIMUSB_CHECK(length == cfg.size() && std::memcmp(got, cfg.data(), cfg.size()) == 0);
    MIMUSB_CHECK(selected_setting(0) == 0 && selected_setting(1) == 0);
    MIMUSB_CHECK(fake_board_endpoint_packet(0x81) == 0U && fake_board_endpoint_packet(0x03) == 0U &&
                 fake_board_endpoint_packet(0x02) == 64U);

    // Nothing is on the bus in setting 0: what the program writes waits.
    fake_transfer_t t;
    Bytes audio(192);
    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] = static_cast<std::uint8_t>(i);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, audio.data(), audio.size(), kSendMs) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_wait_transfer(0x81, &t, 150));

    // Setting 1: the endpoint is on the bus at 192 bytes, the board tells the program, and the
    // packet that waited goes out. What was armed on the controller when the target left a setting
    // is taken off it (the RP2040's driver stops when the next transfer is armed on top of it), at
    // the stop and at the start.
    const unsigned cleared = fake_board_clear_endpoint_count(0x81);
    MIMUSB_CHECK(select_setting(0, 1));
    MIMUSB_CHECK(fake_board_clear_endpoint_count(0x81) == cleared + 1U);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.interface_number == 0U &&
                 ev.value == 1U && ev.node_id == 0U);
    MIMUSB_CHECK(selected_setting(0) == 1 && fake_board_endpoint_packet(0x81) == 192U);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 192U &&
                 std::memcmp(t.data, audio.data(), 192) == 0);
    // A packet each frame of the bus, exactly as written, in order; a short one is a short one.
    for (std::uint8_t n = 1; n <= 3; ++n) {
        Bytes p(n == 3 ? 10 : 192, n);
        MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, p.data(), p.size(), kSendMs) ==
                     MIMUSB_OK);
        MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == p.size() &&
                     t.data[0] == n);
    }
    // An isochronous transfer is one packet: the library refuses more than the endpoint's packet.
    Bytes more(385, 0);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, more.data(), more.size(), kSendMs) ==
                 MIMUSB_ERR_PARAM);
    MIMUSB_CHECK(std::strcmp(mimusb_last_error()->field, "node.endpoint.write") == 0);

    // Setting 2: the same endpoint with the bigger packet; the setting before it is left.
    const unsigned before_two = fake_board_clear_endpoint_count(0x81);
    MIMUSB_CHECK(select_setting(0, 2));
    MIMUSB_CHECK(fake_board_clear_endpoint_count(0x81) ==
                 before_two + 2U); // setting 1 stopped, 2 started
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 2U);
    MIMUSB_CHECK(selected_setting(0) == 2 && fake_board_endpoint_packet(0x81) == 384U);
    Bytes big(384, 0x5A);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, big.data(), big.size(), kSendMs) ==
                 MIMUSB_OK);
    MIMUSB_CHECK(fake_board_wait_transfer(0x81, &t, 1000) && t.length == 384U);

    // Back to setting 0: the endpoint stops; what the program writes waits again, and the same
    // setting selected twice is told twice.
    MIMUSB_CHECK(select_setting(0, 0));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 0U);
    MIMUSB_CHECK(mimusb_device_endpoint_write(device, 0x81, audio.data(), 8, kSendMs) == MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_wait_transfer(0x81, &t, 150));
    MIMUSB_CHECK(select_setting(0, 0));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 0U);

    // A setting the interface does not have, and an interface that is not there, are stalled and
    // change nothing.
    MIMUSB_CHECK(select_setting(0, 3, FAKE_CONTROL_STALLED));
    MIMUSB_CHECK(select_setting(5, 0, FAKE_CONTROL_STALLED));
    MIMUSB_CHECK(selected_setting(0) == 0);
    MIMUSB_CHECK(!wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev, 100));

    // An isochronous OUT endpoint: nothing in setting 0 of its interface (the bulk one is open all
    // along), the packets of the target in setting 1, as they came.
    const Bytes in = {1, 2, 3, 4, 5};
    MIMUSB_CHECK(fake_board_endpoint_packet(0x03) == 0U);
    MIMUSB_CHECK(select_setting(1, 1));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.interface_number == 1U &&
                 ev.value == 1U);
    MIMUSB_CHECK(fake_board_endpoint_packet(0x03) == 100U &&
                 fake_board_endpoint_packet(0x02) == 64U);
    fake_board_target_out(0x03, in.data(), in.size());
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x03U &&
                 ev.interface_number == 1U && ev.data == in);
    fake_board_target_out(0x02, in.data(), 3);
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x02U &&
                 ev.data.size() == 3U);

    // A bus reset in the target (the device is taken off and put back) starts in setting 0 again.
    MIMUSB_CHECK(mimusb_device_clear_descriptors(device, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(!fake_board_mounted());
    // (Clearing forgets the policies too.)
    MIMUSB_CHECK(mimusb_device_endpoint_set_policy(device, 0x81, &keep, kControlMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_set_descriptors(device, &d, &kCommit) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    MIMUSB_CHECK(selected_setting(0) == 0 && selected_setting(1) == 0);
    MIMUSB_CHECK(fake_board_endpoint_packet(0x81) == 0U && fake_board_endpoint_packet(0x03) == 0U);

    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// The refusals that are about the board's limits: a Node has no isochronous endpoint, and the
// packet memory of the controller bounds a configuration of them.
void refusals() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_usb_endpoint ep{};
    ep.address = 0x81;
    ep.transfer_type = MIMUSB_TRANSFER_ISOCHRONOUS;
    ep.max_packet_size = 64;
    ep.interval = 1;
    ep.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    ep.policy.depth = 2;
    const mimusb_usb_item item = mimusb_usb_item_make_endpoint(ep);
    // An isochronous endpoint in setting 0 carries bandwidth from the configuration on: refused
    // (USB 2.0 5.6.3). In setting 1, behind an empty setting 0, it is accepted.
    const mimusb_usb_setting only_default[] = {{&item, 1}};
    const mimusb_usb_setting with_alternate[] = {{nullptr, 0}, {&item, 1}};
    mimusb_usb_interface itf{};
    itf.number = 0;
    itf.class_code = 0xFF;
    itf.settings = only_default;
    itf.setting_count = 1;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = &itf;
    config.interface_count = 1;
    mimusb_node* node = nullptr;
    MIMUSB_CHECK(mimusb_node_open(device, &config, &kCommit, &node) == MIMUSB_ERR_PARAM &&
                 node == nullptr);
    itf.settings = with_alternate;
    itf.setting_count = 2;
    MIMUSB_CHECK(mimusb_node_open(device, &config, &kCommit, &node) == MIMUSB_OK && node != nullptr);
    if (node != nullptr)
        MIMUSB_CHECK(mimusb_node_close(node, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

// An audio function opened as a Node, beside a keyboard Node: a control interface, and a
// streaming interface whose setting 0 has no endpoint, setting 1 an isochronous OUT endpoint (the
// 9-byte audio form, adaptive, with its class-specific endpoint descriptor after it) and setting 2
// an isochronous IN endpoint (asynchronous). The target reads the bytes the library composed,
// selects settings, and the events name the Node.
void node_streaming() {
    mimusb_device* device = open_device(base_options());
    if (device == nullptr)
        return;
    mimusb_spec* kbd = keyboard_spec(0);
    mimusb_hid_node_config nc{};
    test_fill_node_config(&nc, 0);
    mimusb_node* keyboard = open_node(device, kbd, &nc);

    const std::uint8_t ac_header[] = {9, 0x24, 1, 0, 1, 9, 0, 1, 2};
    const std::uint8_t as_general[] = {7, 0x24, 1, 1, 1, 1, 0};
    const std::uint8_t cs_endpoint[] = {7, 0x25, 1, 0, 0, 0, 0};
    mimusb_usb_endpoint out{};
    out.address = 0x03;
    out.transfer_type = MIMUSB_TRANSFER_ISOCHRONOUS;
    out.sync_type = MIMUSB_ISO_SYNC_ADAPTIVE;
    out.usage_type = MIMUSB_ISO_USAGE_DATA;
    out.audio_form = 1;
    out.interval = 1;
    out.max_packet_size = 192;
    mimusb_usb_endpoint in = out;
    in.address = 0x82;
    in.sync_type = MIMUSB_ISO_SYNC_ASYNCHRONOUS;
    in.max_packet_size = 384;
    in.policy = {MIMUSB_FULL_DROP_OLDEST, 4};
    const mimusb_usb_item control_items[] = {
        mimusb_usb_item_make_descriptor(ac_header, sizeof(ac_header))};
    const mimusb_usb_item alt1[] = {mimusb_usb_item_make_descriptor(as_general, sizeof(as_general)),
                                    mimusb_usb_item_make_endpoint(out),
                                    mimusb_usb_item_make_descriptor(cs_endpoint, sizeof(cs_endpoint))};
    const mimusb_usb_item alt2[] = {mimusb_usb_item_make_descriptor(as_general, sizeof(as_general)),
                                    mimusb_usb_item_make_endpoint(in),
                                    mimusb_usb_item_make_descriptor(cs_endpoint, sizeof(cs_endpoint))};
    const mimusb_usb_setting control_settings[] = {{control_items, 1}};
    const mimusb_usb_setting stream_settings[] = {{nullptr, 0}, {alt1, 3}, {alt2, 3}};
    mimusb_usb_interface itfs[2] = {};
    itfs[0].number = 1;
    itfs[0].class_code = 0x01;
    itfs[0].subclass = 0x01;
    itfs[0].settings = control_settings;
    itfs[0].setting_count = 1;
    itfs[1].number = 2;
    itfs[1].class_code = 0x01;
    itfs[1].subclass = 0x02;
    itfs[1].settings = stream_settings;
    itfs[1].setting_count = 3;
    mimusb_node_config config;
    mimusb_node_config_init(&config);
    config.interfaces = itfs;
    config.interface_count = 2;
    mimusb_node* audio = nullptr;
    MIMUSB_CHECK(mimusb_node_open(device, &config, &kCommit, &audio) == MIMUSB_OK &&
                 audio != nullptr);
    MIMUSB_CHECK(mimusb_device_wait_ready(device, 2000) == MIMUSB_OK);
    EventRecord ev;
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_MOUNTED, &ev));
    fake_board_clear_transfers();

    // The bytes the target reads from interface 1 on: settings in order, each with its Interface
    // descriptor and its items in the order given; 9-byte endpoint descriptors whose bmAttributes
    // carry the synchronization type.
    const Bytes expected = {
        9, 4, 1, 0, 0, 1, 1, 0, 0, 9, 0x24, 1, 0, 1, 9, 0, 1, 2,        // interface 1
        9, 4, 2, 0, 0, 1, 2, 0, 0,                                      // interface 2, setting 0
        9, 4, 2, 1, 1, 1, 2, 0, 0, 7, 0x24, 1, 1, 1, 1, 0,              // setting 1
        9, 5, 0x03, 0x09, 0xC0, 0x00, 1, 0, 0, 7, 0x25, 1, 0, 0, 0, 0, // iso OUT, CS_ENDPOINT
        9, 4, 2, 2, 1, 1, 2, 0, 0, 7, 0x24, 1, 1, 1, 1, 0,              // setting 2
        9, 5, 0x82, 0x05, 0x80, 0x01, 1, 0, 0, 7, 0x25, 1, 0, 0, 0, 0}; // iso IN, CS_ENDPOINT
    std::uint16_t length = 0;
    const std::uint8_t* got = fake_board_config_descriptor(&length);
    std::size_t at = 9;
    while (at + 3U < length && !(got[at + 1] == 4 && got[at + 2] == 1 && got[at + 3] == 0))
        at += got[at];
    MIMUSB_CHECK(length - at == expected.size() &&
                 std::memcmp(got + at, expected.data(), expected.size()) == 0);
    MIMUSB_CHECK(got[4] == 3); // bNumInterfaces: the keyboard's and the two of the function
    MIMUSB_CHECK(selected_setting(2) == 0 && fake_board_endpoint_packet(0x03) == 0U &&
                 fake_board_endpoint_packet(0x82) == 0U);

    // Setting 1: the OUT endpoint opens at 192 bytes, the event names the Node and its interface,
    // and a packet from the target arrives as data of that Node.
    MIMUSB_CHECK(select_setting(2, 1));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.interface_number == 2U &&
                 ev.value == 1U && ev.node_id == mimusb_node_id(audio));
    MIMUSB_CHECK(fake_board_endpoint_packet(0x03) == 192U && fake_board_endpoint_packet(0x82) == 0U);
    Bytes frame(192, 0x5A);
    fake_board_target_out(0x03, frame.data(), frame.size());
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_ENDPOINT_DATA, &ev) && ev.endpoint_address == 0x03U &&
                 ev.interface_number == 2U && ev.node_id == mimusb_node_id(audio) &&
                 ev.data.size() == 192U);

    // Setting 2: the IN endpoint opens at 384 bytes and a write goes out as one packet; a write
    // longer than the packet is refused.
    // (The simulated controller keeps the size of an endpoint that stopped: a stop shows as the
    // endpoint being cleared on the controller.)
    const unsigned cleared_out = fake_board_clear_endpoint_count(0x03);
    MIMUSB_CHECK(select_setting(2, 2));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 2U &&
                 ev.node_id == mimusb_node_id(audio));
    MIMUSB_CHECK(fake_board_clear_endpoint_count(0x03) > cleared_out &&
                 fake_board_endpoint_packet(0x82) == 384U);
    Bytes pcm(384);
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<std::uint8_t>(i * 3U);
    MIMUSB_CHECK(mimusb_node_endpoint_write(audio, 0x82, pcm.data(), pcm.size(), kSendMs) ==
                 MIMUSB_OK);
    fake_transfer_t t;
    MIMUSB_CHECK(fake_board_wait_transfer(0x82, &t, 1000) && t.length == 384U &&
                 std::memcmp(t.data, pcm.data(), 384) == 0);
    Bytes too_long(385);
    MIMUSB_CHECK(mimusb_node_endpoint_write(audio, 0x82, too_long.data(), too_long.size(),
                                            kSendMs) == MIMUSB_ERR_PARAM);

    // Back to setting 0: nothing is on the bus; the keyboard still works.
    const unsigned cleared_in = fake_board_clear_endpoint_count(0x82);
    MIMUSB_CHECK(select_setting(2, 0));
    MIMUSB_CHECK(wait_event(MIMUSB_EVENT_INTERFACE_ALTERNATE, &ev) && ev.value == 0U &&
                 ev.node_id == mimusb_node_id(audio));
    MIMUSB_CHECK(fake_board_clear_endpoint_count(0x82) > cleared_in);
    fake_board_clear_reports();
    MIMUSB_CHECK(mimusb_keyboard_key(keyboard, 0x04, 1) == MIMUSB_OK &&
                 mimusb_node_submit(keyboard, kSendMs) == MIMUSB_OK);
    fake_report_t report;
    MIMUSB_CHECK(next_report(&report) && report.iface == 0);

    MIMUSB_CHECK(mimusb_node_close(audio, &kCommit, kDrainMs) == MIMUSB_OK);
    MIMUSB_CHECK(fake_board_interface_count() == 1);
    if (keyboard != nullptr)
        MIMUSB_CHECK(mimusb_node_close(keyboard, &kCommit, kDrainMs) == MIMUSB_OK);
    mimusb_spec_release(kbd);
    MIMUSB_CHECK(mimusb_device_close(device, kControlMs, kDrainMs) == MIMUSB_OK);
}

} // namespace

void test_isochronous() {
    streaming();
    refusals();
    node_streaming();
}

} // namespace sim

#endif // SIM_ISO
