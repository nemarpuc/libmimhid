// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A USB MIDI 1.0 device (an Audio Control interface and a MIDIStreaming interface with one Embedded
// MIDI IN Jack, one External MIDI IN Jack, one Embedded MIDI OUT Jack, one External MIDI OUT Jack
// and the two bulk endpoints; USB Device Class Definition for MIDI Devices 1.0) on a real board
// whose target is this machine. The program is the instrument: it sends every event packet it
// receives back.
//
//   hw_midi
//
// A: the kernel's snd-usb-audio makes a sound card and a rawmidi device for the Device, when that
//    driver is loaded here.
// B: with libusb: 4,000 event packets of 4 bytes (250 transfers of 16 events) are written to the
//    bulk OUT endpoint, and the same 4,000 must come back on the bulk IN endpoint, in order.
#include "hw_util.h"
#include <dirent.h>

enum { EP_IN = 0x81, EP_OUT = 0x01, EVENTS = 4000 };

static mimusb_node* g_node;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static unsigned g_received;
enum { QUEUE = 512 };
static uint8_t g_queue[QUEUE][64];
static size_t g_queue_len[QUEUE];
static unsigned g_head, g_tail;
static int g_stop;

// USB MIDI 1.0 4: a packet is 4 bytes (cable number and code index, then three MIDI bytes); a
// transfer carries whole packets, so what arrives is sent back as it came. The event callback only
// queues it: a write waits for the board's completion events, which this same thread delivers.
static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA && e->endpoint_address == EP_OUT && e->length > 0 &&
        e->length <= 64) {
        pthread_mutex_lock(&g_lock);
        g_received += (unsigned)(e->length / 4);
        if (g_head - g_tail < QUEUE) {
            memcpy(g_queue[g_head % QUEUE], e->data, e->length);
            g_queue_len[g_head % QUEUE] = e->length;
            g_head++;
        }
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
    }
}

static void* echo_thread(void* arg) {
    (void)arg;
    for (;;) {
        uint8_t data[64];
        size_t n;
        pthread_mutex_lock(&g_lock);
        while (g_head == g_tail && !g_stop)
            pthread_cond_wait(&g_cond, &g_lock);
        if (g_head == g_tail) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        n = g_queue_len[g_tail % QUEUE];
        memcpy(data, g_queue[g_tail % QUEUE], n);
        g_tail++;
        pthread_mutex_unlock(&g_lock);
        mimusb_node_endpoint_write(g_node, EP_IN, data, n, 5000);
    }
}

// The host side: reads what comes back on a thread of its own, with a long timeout (a read
// cancelled by a short one can lose packets the host controller had taken).
static libusb_device_handle* g_h;
static uint8_t g_back[EVENTS * 4];
static size_t g_got;
static int g_read_error;

static void* read_thread(void* arg) {
    (void)arg;
    while (g_got < sizeof(g_back)) {
        int n = 0;
        uint8_t chunk[64]; // one packet: a full one does not end a longer read
        int e = libusb_bulk_transfer(g_h, EP_IN, chunk, sizeof(chunk), &n, 5000);
        if (e != 0) {
            g_read_error = e;
            return NULL;
        }
        if (g_got + (size_t)n > sizeof(g_back)) {
            g_read_error = 1;
            return NULL;
        }
        memcpy(g_back + g_got, chunk, (size_t)n);
        g_got += (size_t)n;
    }
    return NULL;
}

static int has_sound_card(char* name, size_t cap) {
    FILE* f = fopen("/proc/asound/cards", "r");
    if (!f)
        return -1; // no ALSA here
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "HW MIDI")) {
            snprintf(name, cap, "%s", line);
            found = 1;
        }
    fclose(f);
    return found;
}

int main(void) {
    mimusb_device* device = hw_open_device("HW MIDI test", on_event);
    if (!device)
        return 1;
    // The descriptors of USB MIDI 1.0, B.3: jack 1 an Embedded IN (the instrument's input, fed by
    // the OUT endpoint), jack 2 an External IN, jack 3 an Embedded OUT (read through the IN
    // endpoint, fed by jack 2), jack 4 an External OUT (fed by jack 1).
    static const uint8_t ac_header[9] = {9, 0x24, 0x01, 0x00, 0x01, 9, 0, 1, 1};
    static const uint8_t ms_header[7] = {7, 0x24, 0x01, 0x00, 0x01, 65, 0};
    static const uint8_t jack1[6] = {6, 0x24, 0x02, 0x01, 1, 0};
    static const uint8_t jack2[6] = {6, 0x24, 0x02, 0x02, 2, 0};
    static const uint8_t jack3[9] = {9, 0x24, 0x03, 0x01, 3, 1, 2, 1, 0};
    static const uint8_t jack4[9] = {9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0};
    static const uint8_t cs_out[5] = {5, 0x25, 0x01, 1, 1}; // after the OUT endpoint: jack 1
    static const uint8_t cs_in[5] = {5, 0x25, 0x01, 1, 3};  // after the IN endpoint: jack 3
    mimusb_usb_endpoint in = hw_bulk_endpoint(EP_IN, 8), out = hw_bulk_endpoint(EP_OUT, 8);
    in.audio_form = out.audio_form = 1; // MIDI endpoints are audio endpoints: 9-byte descriptors
    const mimusb_usb_item ac_items[1] = {mimusb_usb_item_make_descriptor(ac_header, 9)};
    const mimusb_usb_item ms_items[10] = {
        mimusb_usb_item_make_descriptor(ms_header, 7), mimusb_usb_item_make_descriptor(jack1, 6),
        mimusb_usb_item_make_descriptor(jack2, 6),     mimusb_usb_item_make_descriptor(jack3, 9),
        mimusb_usb_item_make_descriptor(jack4, 9),     mimusb_usb_item_make_endpoint(out),
        mimusb_usb_item_make_descriptor(cs_out, 5),    mimusb_usb_item_make_endpoint(in),
        mimusb_usb_item_make_descriptor(cs_in, 5)};
    const mimusb_usb_setting ac_setting = {ac_items, 1};
    const mimusb_usb_setting ms_setting = {ms_items, 9};
    mimusb_usb_interface itf[2];
    memset(itf, 0, sizeof(itf));
    itf[0].number = 0;
    itf[0].class_code = 0x01; // Audio
    itf[0].subclass = 0x01;   // Audio Control
    itf[0].name = "HW MIDI control";
    itf[0].settings = &ac_setting;
    itf[0].setting_count = 1;
    itf[1].number = 1;
    itf[1].class_code = 0x01;
    itf[1].subclass = 0x03; // MIDIStreaming
    itf[1].name = "HW MIDI streaming";
    itf[1].settings = &ms_setting;
    itf[1].setting_count = 1;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = itf;
    nc.interface_count = 2;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    mimusb_result r = example_node_open_interfaces(device, &nc, &to, &g_node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    if (mimusb_device_wait_ready(device, 5000) != MIMUSB_OK) {
        printf("wait_ready failed\n");
        return 1;
    }
    libusb_context* ctx = NULL;
    libusb_device_handle* h = hw_open_target(&ctx);
    if (!h) {
        printf("the board's device did not appear\n");
        return 1;
    }

    printf("A: the kernel's snd-usb-audio\n");
    char card[256] = "";
    int card_found = -1;
    for (int i = 0; i < 30 && card_found != 1; i++) {
        card_found = has_sound_card(card, sizeof(card));
        if (card_found != 1)
            sleep_ms(100);
    }
    if (card_found < 0)
        printf("  no ALSA in this kernel: nothing to check\n");
    else
        printf("  sound card: %s", card_found ? card : "none\n");
    CHECK(card_found != 0, "snd-usb-audio made no sound card for the Device");

    int e = libusb_claim_interface(h, 1); // set aside from snd-usb-audio by the auto-detach
    CHECK(e == 0, "claim: %s", libusb_error_name(e));

    printf("B: %d event packets there and back with libusb\n", EVENTS);
    static uint8_t sent[EVENTS * 4];
    for (int i = 0; i < EVENTS; i++) { // cable 0, Note On (code index 9) on channel 1
        sent[i * 4 + 0] = 0x09;
        sent[i * 4 + 1] = 0x90;
        sent[i * 4 + 2] = (uint8_t)(i & 0x7F);
        sent[i * 4 + 3] = (uint8_t)((i >> 7) & 0x7F);
    }
    g_h = h;
    pthread_t reader, echo;
    pthread_create(&echo, NULL, echo_thread, NULL);
    pthread_create(&reader, NULL, read_thread, NULL);
    double t0 = now_ms();
    for (size_t put = 0; put < sizeof(sent);) {
        int n = 0;
        e = libusb_bulk_transfer(h, EP_OUT, sent + put, 64, &n, 5000); // 16 events
        if (e != 0) {
            CHECK(0, "bulk OUT at %zu: %s", put, libusb_error_name(e));
            break;
        }
        put += (size_t)n;
    }
    pthread_join(reader, NULL);
    CHECK(g_read_error == 0, "bulk IN after %zu bytes: error %d", g_got, g_read_error);
    const size_t got = g_got;
    double t1 = now_ms();
    CHECK(got == sizeof(sent) && memcmp(sent, g_back, sizeof(sent)) == 0,
          "%zu of %zu bytes came back, in order: %s", got, sizeof(sent),
          memcmp(sent, g_back, got < sizeof(sent) ? got : sizeof(sent)) == 0 ? "yes" : "no");
    printf("  %d events there and back in %.0f ms (%.1f ms per transfer of 16)\n", EVENTS, t1 - t0,
           (t1 - t0) / (EVENTS / 16.0));
    pthread_mutex_lock(&g_lock);
    CHECK(g_received == EVENTS, "the program received %u events", g_received);
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(echo, NULL);
    hw_link_check(device);
    libusb_release_interface(h, 1);
    libusb_close(h);
    libusb_exit(ctx);
    mimusb_device_close(device, 500, 1000);
    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails != 0;
}
