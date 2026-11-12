// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A USB mass storage function (class 0x08, subclass 0x06 SCSI, protocol 0x50 Bulk-Only; USB Mass
// Storage Class Bulk-Only Transport 1.0) on a real board whose target is this machine. The program
// is the disk: it answers the SCSI commands of a RAM disk of 2 MiB, which the board knows nothing
// about; it only places the interface and moves the bulk packets.
//
//   hw_msc
//
// A: the kernel's usb-storage binds, probes the disk with its own SCSI commands, and /sys/block
//    shows a disk of the size the program reported, with the vendor and product it gave.
// B: with the kernel's driver set aside, this program is the initiator with libusb: INQUIRY, READ
//    CAPACITY, a WRITE(10) and READ(10) of 64 blocks that must give the bytes back, an unsupported
//    command (a stalled endpoint, a failed CSW, REQUEST SENSE with the reason), and the
//    Bulk-Only Mass Storage Reset.
// The data of /dev/sdX cannot be read without the disk group; B is what proves the data path.
#include "example_values.h"
#include <dirent.h>
#include <libusb.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BLOCK = 512, BLOCKS = 4096, EP_IN = 0x81, EP_OUT = 0x01 };

static uint8_t g_disk[BLOCK * BLOCKS];

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static uint8_t g_rx[1 << 17];
static size_t g_rx_len;
static int g_halt_cleared, g_resets, g_stop;
static unsigned g_commands, g_failed_commands;

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA && e->endpoint_address == EP_OUT) {
        if (g_rx_len + e->length <= sizeof(g_rx)) {
            memcpy(g_rx + g_rx_len, e->data, e->length);
            g_rx_len += e->length;
        }
    } else if (e->kind == MIMUSB_EVENT_ENDPOINT_HALT && e->endpoint_address == EP_IN &&
               e->value == 0) {
        g_halt_cleared++;
    } else if (e->kind == MIMUSB_EVENT_CONTROL_SETUP && (e->control_flags & MIMUSB_CONTROL_FLAG_ACCEPTED)) {
        // The only ACCEPT rule is the Bulk-Only Mass Storage Reset.
        g_resets++;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("  FAIL: " __VA_ARGS__);                                                        \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---- the disk: the responder thread ---------------------------------------------------------

static mimusb_node* g_node;
static uint8_t g_sense_key, g_asc, g_ascq;

// Waits until n bytes are in the receive buffer and moves them out; 0 when stopped or too late.
static int take(uint8_t* out, size_t n, int timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_lock);
    while (g_rx_len < n && !g_stop)
        if (pthread_cond_timedwait(&g_cond, &g_lock, &until) != 0)
            break;
    int ok = g_rx_len >= n && !g_stop;
    if (ok) {
        memcpy(out, g_rx, n);
        memmove(g_rx, g_rx + n, g_rx_len - n);
        g_rx_len -= n;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static void send_in(const uint8_t* data, size_t n) {
    mimusb_result r = mimusb_node_endpoint_write(g_node, EP_IN, data, n, 2000);
    if (r != MIMUSB_OK)
        printf("  responder: endpoint_write: %s\n", mimusb_result_name(r));
}

static void send_csw(uint32_t tag, uint32_t residue, uint8_t status) {
    uint8_t csw[13] = {'U', 'S', 'B', 'S'};
    put_le32(csw + 4, tag);
    put_le32(csw + 8, residue);
    csw[12] = status;
    send_in(csw, sizeof(csw));
}

static void fail_sense(uint8_t key, uint8_t asc, uint8_t ascq) {
    g_sense_key = key;
    g_asc = asc;
    g_ascq = ascq;
}

static void* responder(void* arg) {
    (void)arg;
    static uint8_t buf[BLOCK * 128];
    for (;;) {
        uint8_t cbw[31];
        if (!take(cbw, sizeof(cbw), 500)) {
            pthread_mutex_lock(&g_lock);
            int stop = g_stop;
            pthread_mutex_unlock(&g_lock);
            if (stop)
                return NULL;
            continue;
        }
        if (le32(cbw) != 0x43425355U) { // 'USBC': not a CBW; BOT 6.6.1 says stall and wait for reset
            printf("  responder: a bad CBW signature\n");
            continue;
        }
        const uint32_t tag = le32(cbw + 4), dlen = le32(cbw + 8);
        const int to_host = (cbw[12] & 0x80) != 0;
        const uint8_t* cb = cbw + 15;
        uint32_t sent = 0;
        uint8_t status = 0;
        g_commands++;
        switch (cb[0]) {
        case 0x00: // TEST UNIT READY
        case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
            break;
        case 0x03: { // REQUEST SENSE
            uint8_t s[18] = {0x70, 0, g_sense_key, 0, 0, 0, 0, 10};
            s[12] = g_asc;
            s[13] = g_ascq;
            sent = dlen < 18 ? dlen : 18;
            send_in(s, sent);
            fail_sense(0, 0, 0);
            break;
        }
        case 0x12: { // INQUIRY
            uint8_t q[36] = {0x00, 0x80, 0x04, 0x02, 31};
            memcpy(q + 8, "MIMUSB  ", 8);
            memcpy(q + 16, "RAM DISK        ", 16);
            memcpy(q + 32, "0.6 ", 4);
            if (cb[1] & 1) { // an EVPD page: not supported
                status = 1;
                fail_sense(5, 0x24, 0);
                break;
            }
            sent = dlen < 36 ? dlen : 36;
            send_in(q, sent);
            break;
        }
        case 0x1A: { // MODE SENSE(6): no pages, not write protected
            const uint8_t m[4] = {3, 0, 0, 0};
            sent = dlen < 4 ? dlen : 4;
            send_in(m, sent);
            break;
        }
        case 0x25: { // READ CAPACITY(10)
            uint8_t c[8];
            put_be32(c, BLOCKS - 1);
            put_be32(c + 4, BLOCK);
            sent = dlen < 8 ? dlen : 8;
            send_in(c, sent);
            break;
        }
        case 0x28:
        case 0x2A: { // READ(10), WRITE(10)
            const uint32_t lba = be32(cb + 2), count = (uint32_t)(cb[7] << 8 | cb[8]);
            if ((uint64_t)lba + count > BLOCKS || count * BLOCK > sizeof(buf)) {
                status = 1;
                fail_sense(5, 0x21, 0); // LOGICAL BLOCK ADDRESS OUT OF RANGE
                if (cb[0] == 0x2A) {
                    uint32_t left = dlen;
                    while (left) {
                        uint32_t n = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
                        if (!take(buf, n, 2000))
                            break;
                        left -= n;
                    }
                }
                break;
            }
            if (cb[0] == 0x28) {
                send_in(g_disk + (size_t)lba * BLOCK, (size_t)count * BLOCK);
                sent = count * BLOCK;
            } else {
                if (!take(buf, (size_t)count * BLOCK, 3000)) {
                    status = 1;
                    fail_sense(4, 0x08, 0);
                    break;
                }
                memcpy(g_disk + (size_t)lba * BLOCK, buf, (size_t)count * BLOCK);
                sent = count * BLOCK;
            }
            break;
        }
        default: // INVALID COMMAND OPERATION CODE
            status = 1;
            fail_sense(5, 0x20, 0);
            if (dlen && to_host) {
                // BOT 6.7.2: the device stalls Bulk-In, and sends the CSW once the host cleared it.
                pthread_mutex_lock(&g_lock);
                int before = g_halt_cleared;
                pthread_mutex_unlock(&g_lock);
                mimusb_node_endpoint_stall(g_node, EP_IN, 500);
                for (int i = 0; i < 60; i++) {
                    pthread_mutex_lock(&g_lock);
                    int now = g_halt_cleared;
                    pthread_mutex_unlock(&g_lock);
                    if (now != before)
                        break;
                    sleep_ms(50);
                }
            } else if (dlen) { // data the host sends anyway: take it
                uint32_t left = dlen;
                while (left) {
                    uint32_t n = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
                    if (!take(buf, n, 2000))
                        break;
                    left -= n;
                }
            }
            break;
        }
        if (status)
            g_failed_commands++;
        send_csw(tag, dlen > sent ? dlen - sent : 0, status);
    }
}

// ---- the initiator on the target side ---------------------------------------------------------

static libusb_device_handle* g_h;

// One Bulk-Only command: CBW, the data stage, CSW. Returns the CSW status, or -1 on a transport
// error; *data_got is the length of the data stage.
static int bot(const uint8_t* cb, uint8_t cb_len, int to_host, uint8_t* data, uint32_t dlen,
               uint32_t* data_got, uint32_t* residue, int* stalled) {
    static uint32_t tag = 0x1000;
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    put_le32(cbw + 4, ++tag);
    put_le32(cbw + 8, dlen);
    cbw[12] = to_host ? 0x80 : 0x00;
    cbw[14] = cb_len;
    memcpy(cbw + 15, cb, cb_len);
    int n = 0;
    *data_got = 0;
    if (stalled)
        *stalled = 0;
    if (libusb_bulk_transfer(g_h, EP_OUT, cbw, sizeof(cbw), &n, 2000) != 0 || n != 31)
        return -1;
    if (dlen) {
        int e = libusb_bulk_transfer(g_h, to_host ? EP_IN : EP_OUT, data, (int)dlen, &n, 5000);
        if (e == LIBUSB_ERROR_PIPE) {
            if (stalled)
                *stalled = 1;
            libusb_clear_halt(g_h, to_host ? EP_IN : EP_OUT);
        } else if (e != 0) {
            return -1;
        }
        *data_got = (uint32_t)n;
    }
    uint8_t csw[13];
    if (libusb_bulk_transfer(g_h, EP_IN, csw, sizeof(csw), &n, 3000) != 0 || n != 13 ||
        le32(csw) != 0x53425355U || le32(csw + 4) != tag)
        return -1;
    if (residue)
        *residue = le32(csw + 8);
    return csw[12];
}

static int find_sysfs_disk(char* out, size_t cap, uint64_t* sectors) {
    DIR* d = opendir("/sys/block");
    struct dirent* ent;
    int found = 0;
    while (d && (ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "sd", 2) != 0)
            continue;
        char path[300], vendor[16] = "";
        snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(vendor, sizeof(vendor), f) && strncmp(vendor, "MIMUSB", 6) == 0) {
            snprintf(out, cap, "%s", ent->d_name);
            snprintf(path, sizeof(path), "/sys/block/%s/size", ent->d_name);
            FILE* s = fopen(path, "r");
            if (s) {
                unsigned long long v = 0;
                if (fscanf(s, "%llu", &v) == 1)
                    *sectors = v;
                fclose(s);
            }
            found = 1;
        }
        fclose(f);
        if (found)
            break;
    }
    if (d)
        closedir(d);
    return found;
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(g_disk); i += 4)
        put_le32(g_disk + i, (uint32_t)(i * 2654435761u)); // a recognisable, non-zero disk
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "HW MSC test");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }

    mimusb_usb_endpoint in, out;
    memset(&in, 0, sizeof(in));
    in.address = EP_IN;
    in.transfer_type = MIMUSB_TRANSFER_BULK;
    in.max_packet_size = 64;
    in.policy.on_full = MIMUSB_FULL_WAIT;
    in.policy.depth = 4;
    memset(&out, 0, sizeof(out));
    out.address = EP_OUT;
    out.transfer_type = MIMUSB_TRANSFER_BULK;
    out.max_packet_size = 64;
    const mimusb_usb_item items[2] = {mimusb_usb_item_make_endpoint(in),
                                      mimusb_usb_item_make_endpoint(out)};
    const mimusb_usb_setting setting = {items, 2};
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0x08;
    itf.subclass = 0x06;
    itf.protocol = 0x50;
    itf.name = "HW MSC disk";
    itf.settings = &setting;
    itf.setting_count = 1;
    // BOT 3.1 and 3.2: Bulk-Only Mass Storage Reset (class, interface, 0xFF) is accepted; Get Max
    // LUN (class, interface, 0xFE) is answered with one byte, the highest LUN: 0.
    const uint8_t max_lun = 0;
    mimusb_control_rule rules[2];
    memset(rules, 0, sizeof(rules));
    rules[0].request_type_mask = 0xFF;
    rules[0].request_type_value = 0x21;
    rules[0].request_mask = 0xFF;
    rules[0].request_value = 0xFF;
    rules[0].index_mask = 0xFFFF;
    rules[0].index_value = 0;
    rules[0].action = MIMUSB_CONTROL_ACCEPT;
    rules[1].request_type_mask = 0xFF;
    rules[1].request_type_value = 0xA1;
    rules[1].request_mask = 0xFF;
    rules[1].request_value = 0xFE;
    rules[1].index_mask = 0xFFFF;
    rules[1].index_value = 0;
    rules[1].action = MIMUSB_CONTROL_ANSWER;
    rules[1].data = &max_lun;
    rules[1].length = 1;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    nc.rules = rules;
    nc.rule_count = 2;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    r = example_node_open_interfaces(device, &nc, &to, &g_node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, responder, NULL);
    if (mimusb_device_wait_ready(device, 5000) != MIMUSB_OK) {
        printf("wait_ready failed\n");
        return 1;
    }

    printf("A: the kernel's usb-storage and the disk it found\n");
    char name[64] = "";
    uint64_t sectors = 0;
    for (int i = 0; i < 150 && !name[0]; i++) {
        if (!find_sysfs_disk(name, sizeof(name), &sectors))
            sleep_ms(100);
    }
    CHECK(name[0], "no /sys/block/sd* with the vendor MIMUSB appeared");
    if (name[0]) {
        printf("  /sys/block/%s: %llu sectors (%llu bytes); the program answered %u commands\n",
               name, (unsigned long long)sectors, (unsigned long long)sectors * 512, g_commands);
        CHECK(sectors == BLOCKS, "the kernel sees %llu sectors, the program reported %u",
              (unsigned long long)sectors, BLOCKS);
    }
    sleep_ms(1000);

    printf("B: Bulk-Only Transport with libusb\n");
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    for (int i = 0; i < 50 && !g_h; i++) {
        g_h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
        if (!g_h)
            sleep_ms(100);
    }
    if (!g_h) {
        printf("libusb: cannot open 1209:0001 (permissions?)\n");
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(g_h, 1);
    int e = libusb_claim_interface(g_h, 0);
    CHECK(e == 0, "claim: %s", libusb_error_name(e));
    if (e != 0)
        return 1;

    uint8_t lun = 0xEE;
    e = libusb_control_transfer(g_h, 0xA1, 0xFE, 0, 0, &lun, 1, 1000);
    CHECK(e == 1 && lun == 0, "GET_MAX_LUN: %d, %u", e, lun);

    static uint8_t data[BLOCK * 64], back[BLOCK * 64];
    uint32_t got = 0, residue = 0;
    int stalled = 0;
    const uint8_t tur[6] = {0x00};
    CHECK(bot(tur, 6, 0, NULL, 0, &got, &residue, NULL) == 0, "TEST UNIT READY");

    const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    int st = bot(inquiry, 6, 1, data, 36, &got, &residue, NULL);
    CHECK(st == 0 && got == 36 && memcmp(data + 8, "MIMUSB  ", 8) == 0 &&
              memcmp(data + 16, "RAM DISK", 8) == 0,
          "INQUIRY: status %d, %u bytes", st, got);

    const uint8_t capacity[10] = {0x25};
    st = bot(capacity, 10, 1, data, 8, &got, &residue, NULL);
    CHECK(st == 0 && got == 8 && be32(data) == BLOCKS - 1 && be32(data + 4) == BLOCK,
          "READ CAPACITY: status %d, last LBA %u, block %u", st, be32(data), be32(data + 4));

    // A: 64 blocks (32 KiB) through 64-byte packets, both ways, compared byte for byte.
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + (i >> 8));
    uint8_t wr[10] = {0x2A, 0, 0, 0, 0, 100, 0, 0, 64, 0};
    double t0 = now_ms();
    st = bot(wr, 10, 0, data, sizeof(data), &got, &residue, NULL);
    double t1 = now_ms();
    CHECK(st == 0 && got == sizeof(data) && residue == 0, "WRITE(10) 64 blocks: status %d, %u sent",
          st, got);
    uint8_t rd[10] = {0x28, 0, 0, 0, 0, 100, 0, 0, 64, 0};
    double t2 = now_ms();
    st = bot(rd, 10, 1, back, sizeof(back), &got, &residue, NULL);
    double t3 = now_ms();
    CHECK(st == 0 && got == sizeof(back) && residue == 0, "READ(10) 64 blocks: status %d, %u read",
          st, got);
    CHECK(memcmp(data, back, sizeof(data)) == 0, "the 32 KiB read back differ from those written");
    printf("  32 KiB written in %.0f ms (%.0f kB/s), read in %.0f ms (%.0f kB/s)\n", t1 - t0,
           sizeof(data) / (t1 - t0), t3 - t2, sizeof(back) / (t3 - t2));

    // The last block of the disk, and one past it.
    uint8_t last[10] = {0x28, 0, 0, 0, 0x0F, 0xFF, 0, 0, 1, 0};
    st = bot(last, 10, 1, back, BLOCK, &got, &residue, NULL);
    CHECK(st == 0 && got == BLOCK && memcmp(back, g_disk + (BLOCKS - 1) * BLOCK, BLOCK) == 0,
          "READ of the last block: status %d, %u", st, got);
    uint8_t past[10] = {0x28, 0, 0, 0, 0x10, 0x00, 0, 0, 1, 0};
    st = bot(past, 10, 1, back, BLOCK, &got, &residue, &stalled);
    CHECK(st == 1 && stalled, "READ past the end: status %d, stalled %d", st, stalled);
    uint8_t sense_cb[6] = {0x03, 0, 0, 0, 18, 0};
    st = bot(sense_cb, 6, 1, back, 18, &got, &residue, NULL);
    CHECK(st == 0 && got == 18 && back[2] == 5 && back[12] == 0x21,
          "REQUEST SENSE after the read: key %u, ASC %02X", back[2], back[12]);

    // An operation code the disk does not have: Bulk-In is stalled, the host clears it, the CSW
    // says "failed", and the sense says why.
    uint8_t bogus[6] = {0xC7};
    st = bot(bogus, 6, 1, back, 64, &got, &residue, &stalled);
    CHECK(st == 1 && stalled && residue == 64, "unsupported command: status %d, stalled %d, "
          "residue %u", st, stalled, residue);
    st = bot(sense_cb, 6, 1, back, 18, &got, &residue, NULL);
    CHECK(st == 0 && back[2] == 5 && back[12] == 0x20, "REQUEST SENSE: key %u, ASC %02X", back[2],
          back[12]);

    // Bulk-Only Mass Storage Reset (BOT 3.1), then the disk goes on.
    e = libusb_control_transfer(g_h, 0x21, 0xFF, 0, 0, NULL, 0, 1000);
    CHECK(e == 0, "Bulk-Only Mass Storage Reset: %s", libusb_error_name(e));
    libusb_clear_halt(g_h, EP_IN);
    libusb_clear_halt(g_h, EP_OUT);
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    int resets = g_resets;
    pthread_mutex_unlock(&g_lock);
    CHECK(resets == 1, "the program heard %d resets", resets);
    CHECK(bot(tur, 6, 0, NULL, 0, &got, &residue, NULL) == 0, "TEST UNIT READY after the reset");

    // A longer run: 128 transfers of 4 KiB alternating, the disk is the reference.
    int bad = 0;
    t0 = now_ms();
    for (int i = 0; i < 128; i++) {
        uint32_t lba = (uint32_t)(i * 8);
        uint8_t w[10] = {0x2A, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                         (uint8_t)lba, 0, 0, 8, 0};
        uint8_t rr[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                          (uint8_t)lba, 0, 0, 8, 0};
        for (int k = 0; k < 4096; k++)
            data[k] = (uint8_t)(k + i);
        if (bot(w, 10, 0, data, 4096, &got, &residue, NULL) != 0 || got != 4096)
            bad++;
        else if (bot(rr, 10, 1, back, 4096, &got, &residue, NULL) != 0 || got != 4096 ||
                 memcmp(data, back, 4096) != 0)
            bad++;
    }
    t1 = now_ms();
    CHECK(bad == 0, "%d of 128 write+read pairs of 4 KiB went wrong", bad);
    printf("  128 write+read pairs of 4 KiB in %.0f ms (%.0f kB/s each way)\n", t1 - t0,
           128 * 4.0 / ((t1 - t0) / 1000));

    mimusb_link_counters lc;
    memset(&lc, 0, sizeof(lc));
    lc.struct_size = sizeof(lc);
    r = mimusb_device_link_counters(device, 0, 500, &lc);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n",
           mimusb_result_name(r), lc.bad_crc, lc.framing, lc.uart_overrun, lc.ring_overrun);
    CHECK(r == MIMUSB_OK && lc.bad_crc == 0 && lc.framing == 0 && lc.uart_overrun == 0 &&
              lc.ring_overrun == 0,
          "link errors");

    libusb_release_interface(g_h, 0);
    libusb_close(g_h);
    libusb_exit(ctx);
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(thread, NULL);
    mimusb_device_close(device, 500, 1000);
    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails != 0;
}
