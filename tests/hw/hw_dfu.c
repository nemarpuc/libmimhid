// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A USB DFU 1.1 device in DFU mode (class 0xFE, subclass 0x01, protocol 0x02; USB Device Class
// Specification for Device Firmware Upgrade 1.1) on a real board whose target is this machine. The
// interface has no endpoint: every DFU request is a class request of endpoint 0, so this is the
// test of control rules with data stages. The program is the device and its state machine; the
// board knows nothing of DFU.
//
//   hw_dfu
//
// DFU_DNLOAD and DFU_CLRSTATUS / DFU_ABORT are ACCEPT rules (the data stage is taken by the board
// and told to the program); DFU_GETSTATUS, DFU_GETSTATE and DFU_UPLOAD are ASK rules (the program
// answers each from its state). A request the table does not name is stalled by the board.
// With libusb this program is the host: GETSTATUS, a download of 16 blocks of 64 bytes with a
// GETSTATUS after each, the zero-length block that starts the manifestation, the states that
// follow, and an upload that must give the image back.
#include "hw_util.h"

enum { BLOCK = 64, BLOCKS = 16 };
enum {
    DFU_IDLE = 2,
    DFU_DNLOAD_SYNC = 3,
    DFU_DNLOAD_IDLE = 5,
    DFU_MANIFEST_SYNC = 6,
    DFU_MANIFEST = 7,
    DFU_UPLOAD_IDLE = 9
};
enum { DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, GETSTATE = 5, ABORT = 6 };

static mimusb_device* g_device;
static uint8_t g_image[BLOCK * BLOCKS];
static size_t g_image_len;
static uint8_t g_state = DFU_IDLE;
static uint8_t g_setup[8];
static unsigned g_asked, g_accepted, g_data_events;

// All of it runs on the reader thread, in order: the state needs no lock.
static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    if (e->kind == MIMUSB_EVENT_CONTROL_SETUP) {
        memcpy(g_setup, e->data, 8);
        const uint8_t request = g_setup[1];
        const uint16_t value = (uint16_t)(g_setup[2] | g_setup[3] << 8);
        const uint16_t length = (uint16_t)(g_setup[6] | g_setup[7] << 8);
        if (e->control_flags & MIMUSB_CONTROL_FLAG_ACCEPTED) {
            g_accepted++;
            if (request == DNLOAD && length == 0) {
                g_state = DFU_MANIFEST_SYNC; // the end of the download: manifestation
            } else if (request == CLRSTATUS || request == ABORT) {
                g_state = DFU_IDLE;
            }
        }
        if (e->control_flags & MIMUSB_CONTROL_FLAG_HELD) {
            g_asked++;
            uint8_t out[BLOCK] = {0};
            size_t n = 0;
            if (request == GETSTATUS) {
                // bStatus, bwPollTimeout (3 bytes), bState, iString. The block is written at once.
                if (g_state == DFU_DNLOAD_SYNC)
                    g_state = DFU_DNLOAD_IDLE;
                else if (g_state == DFU_MANIFEST_SYNC)
                    g_state = DFU_MANIFEST; // manifestation tolerant: back to idle at the next poll
                else if (g_state == DFU_MANIFEST)
                    g_state = DFU_IDLE;
                const uint8_t st[6] = {0, 0, 0, 0, g_state, 0};
                memcpy(out, st, 6);
                n = 6;
            } else if (request == GETSTATE) {
                out[0] = g_state;
                n = 1;
            } else if (request == UPLOAD) {
                const size_t at = (size_t)value * BLOCK;
                if (at < g_image_len) {
                    n = g_image_len - at < BLOCK ? g_image_len - at : BLOCK;
                    memcpy(out, g_image + at, n);
                    g_state = DFU_UPLOAD_IDLE;
                } else {
                    g_state = DFU_IDLE; // a short (here empty) block ends the upload
                }
                if (n > length)
                    n = length;
            }
            mimusb_device_control_reply(g_device, e->control_token, MIMUSB_CONTROL_ACTION_ANSWER,
                                        out, n, 200);
        }
    } else if (e->kind == MIMUSB_EVENT_CONTROL_DATA) {
        g_data_events++;
        const uint16_t value = (uint16_t)(g_setup[2] | g_setup[3] << 8);
        const size_t at = (size_t)value * BLOCK;
        if (g_setup[1] == DNLOAD && at + e->length <= sizeof(g_image)) {
            memcpy(g_image + at, e->data, e->length);
            g_image_len = at + e->length;
            g_state = DFU_DNLOAD_SYNC;
        }
    }
}

static int status(libusb_device_handle* h, uint8_t out[6]) {
    return libusb_control_transfer(h, 0xA1, GETSTATUS, 0, 0, out, 6, 1000);
}

int main(void) {
    g_device = hw_open_device("HW DFU test", on_event);
    if (!g_device)
        return 1;
    // DFU 1.1 4.1.3: the functional descriptor: canDnload, canUpload, manifestationTolerant;
    // wDetachTimeOut 255 ms; wTransferSize 64; bcdDFUVersion 1.10.
    static const uint8_t functional[9] = {9, 0x21, 0x07, 0xFF, 0x00, BLOCK, 0x00, 0x10, 0x01};
    const mimusb_usb_item items[1] = {
        mimusb_usb_item_make_descriptor(functional, sizeof(functional))};
    const mimusb_usb_setting setting = {items, 1};
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0xFE;
    itf.subclass = 0x01;
    itf.protocol = 0x02;
    itf.name = "HW DFU";
    itf.settings = &setting;
    itf.setting_count = 1;
    mimusb_control_rule rules[6];
    memset(rules, 0, sizeof(rules));
    for (int i = 0; i < 6; i++) {
        rules[i].request_type_mask = 0xFF;
        rules[i].request_mask = 0xFF;
        rules[i].index_mask = 0xFFFF; // the interface number: 0
        rules[i].index_value = 0;
    }
    const struct {
        uint8_t type, request;
        mimusb_control_rule_action action;
    } table[6] = {{0x21, DNLOAD, MIMUSB_CONTROL_ACCEPT}, {0x21, CLRSTATUS, MIMUSB_CONTROL_ACCEPT},
                  {0x21, ABORT, MIMUSB_CONTROL_ACCEPT},  {0xA1, GETSTATUS, MIMUSB_CONTROL_ASK},
                  {0xA1, GETSTATE, MIMUSB_CONTROL_ASK},  {0xA1, UPLOAD, MIMUSB_CONTROL_ASK}};
    for (int i = 0; i < 6; i++) {
        rules[i].request_type_value = table[i].type;
        rules[i].request_value = table[i].request;
        rules[i].action = table[i].action;
        if (table[i].action == MIMUSB_CONTROL_ASK) {
            rules[i].timeout_ms = 200;
            rules[i].when_late = MIMUSB_CONTROL_ACTION_STALL;
        }
    }
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    nc.rules = rules;
    nc.rule_count = 6;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    mimusb_node* node = NULL;
    mimusb_result r = example_node_open_interfaces(g_device, &nc, &to, &node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    if (mimusb_device_wait_ready(g_device, 5000) != MIMUSB_OK) {
        printf("wait_ready failed\n");
        return 1;
    }
    libusb_context* ctx = NULL;
    libusb_device_handle* h = hw_open_target(&ctx);
    if (!h) {
        printf("the board's device did not appear\n");
        return 1;
    }
    int e = libusb_claim_interface(h, 0);
    CHECK(e == 0, "claim: %s", libusb_error_name(e));

    printf("A: the descriptors\n");
    uint8_t cfg[256];
    int n = libusb_control_transfer(h, 0x80, 6, 0x0200, 0, cfg, sizeof(cfg), 1000);
    int found_functional = 0;
    for (int i = 0; n > 0 && i + 9 <= n; i += cfg[i] ? cfg[i] : 1)
        if (cfg[i] == 9 && cfg[i + 1] == 0x21 && memcmp(cfg + i, functional, 9) == 0)
            found_functional = 1;
    CHECK(found_functional, "the DFU functional descriptor is not in the configuration");

    printf("B: DFU requests through the rules\n");
    uint8_t st[6];
    double t0 = now_ms();
    n = status(h, st);
    double t1 = now_ms();
    CHECK(n == 6 && st[0] == 0 && st[4] == DFU_IDLE, "GETSTATUS at the start: %d, state %d", n,
          n == 6 ? st[4] : -1);
    printf("  GETSTATUS (an ASK answered by the program): %.1f ms\n", t1 - t0);

    uint8_t block[BLOCK];
    int bad = 0;
    t0 = now_ms();
    for (int b = 0; b < BLOCKS; b++) {
        for (int i = 0; i < BLOCK; i++)
            block[i] = (uint8_t)(b * 31 + i * 7);
        n = libusb_control_transfer(h, 0x21, DNLOAD, (uint16_t)b, 0, block, BLOCK, 1000);
        int m = status(h, st);
        if (n != BLOCK || m != 6 || st[0] != 0 || st[4] != DFU_DNLOAD_IDLE)
            bad++;
    }
    t1 = now_ms();
    CHECK(bad == 0, "%d of %d download blocks went wrong", bad, BLOCKS);
    printf("  %d blocks of %d bytes, each followed by GETSTATUS: %.0f ms\n", BLOCKS, BLOCK,
           t1 - t0);

    n = libusb_control_transfer(h, 0x21, DNLOAD, BLOCKS, 0, NULL, 0, 1000);
    CHECK(n == 0, "the zero-length DNLOAD: %d", n);
    int m = status(h, st);
    CHECK(m == 6 && st[4] == DFU_MANIFEST, "first GETSTATUS after it: state %d (want %d)",
          m == 6 ? st[4] : -1, DFU_MANIFEST);
    m = status(h, st);
    CHECK(m == 6 && st[4] == DFU_IDLE, "second GETSTATUS: state %d (want %d)", m == 6 ? st[4] : -1,
          DFU_IDLE);
    uint8_t state = 0;
    n = libusb_control_transfer(h, 0xA1, GETSTATE, 0, 0, &state, 1, 1000);
    CHECK(n == 1 && state == DFU_IDLE, "GETSTATE: %d, %d", n, state);

    bad = 0;
    for (int b = 0; b <= BLOCKS; b++) {
        uint8_t back[BLOCK];
        n = libusb_control_transfer(h, 0xA1, UPLOAD, (uint16_t)b, 0, back, BLOCK, 1000);
        if (b < BLOCKS) {
            if (n != BLOCK || memcmp(back, g_image + (size_t)b * BLOCK, BLOCK) != 0)
                bad++;
        } else if (n != 0) {
            bad++;
        }
    }
    CHECK(bad == 0, "%d upload blocks differ from what was downloaded", bad);

    n = libusb_control_transfer(h, 0x21, ABORT, 0, 0, NULL, 0, 1000);
    CHECK(n == 0, "ABORT: %d", n);
    n = libusb_control_transfer(h, 0x21, 7, 0, 0, NULL, 0, 1000); // no such DFU request
    CHECK(n == LIBUSB_ERROR_PIPE, "a request the table does not name: %d (want a stall)", n);
    printf("  the program was asked %u times, accepted %u requests, saw %u data stages\n", g_asked,
           g_accepted, g_data_events);
    CHECK(g_data_events == BLOCKS, "data stages: %u", g_data_events);

    hw_link_check(g_device);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    mimusb_device_close(g_device, 500, 1000);
    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails != 0;
}
