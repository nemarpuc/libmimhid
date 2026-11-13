// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A USB printer (class 0x07, subclass 0x01, protocol 0x02 bidirectional; USB Device Class
// Definition for Printing Devices 1.1) on a real board whose target is this machine. The program is
// the printer: it takes a print job on the bulk OUT endpoint and says how much it took on the bulk
// IN endpoint; the three class requests (GET_DEVICE_ID, GET_PORT_STATUS, SOFT_RESET) are rules.
//
//   hw_printer
//
// A: whether the kernel's usblp driver bound the interface (a /dev/usb/lp node), when it is loaded.
// B: with libusb: GET_DEVICE_ID (an ANSWER rule: a 2-byte big-endian length, then the IEEE 1284
//    string), GET_PORT_STATUS (an ANSWER rule), SOFT_RESET (an ACCEPT rule, told to the program), a
//    print job of 200,000 bytes whose end the program recognises, and its answer on the IN
//    endpoint.
#include "hw_util.h"
#include <dirent.h>

enum { EP_IN = 0x81, EP_OUT = 0x01, JOB = 200000 };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static mimusb_node* g_node;
static size_t g_job_len;
static uint32_t g_job_sum;
static unsigned g_resets, g_jobs;
static uint8_t g_tail[16];
static size_t g_tail_len;
static const char kEnd[] = "\x1b%-12345X@PJL EOJ\n";

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA && e->endpoint_address == EP_OUT) {
        pthread_mutex_lock(&g_lock);
        g_job_len += e->length;
        for (size_t i = 0; i < e->length; i++)
            g_job_sum = g_job_sum * 31u + e->data[i];
        // The end marker may be cut between packets: keep the last bytes seen.
        uint8_t joined[sizeof(g_tail) + 64];
        size_t jn = 0;
        memcpy(joined, g_tail, g_tail_len);
        jn = g_tail_len;
        const size_t take = e->length < 64 ? e->length : 64;
        memcpy(joined + jn, e->data + (e->length - take), take);
        jn += take;
        const size_t marker = sizeof(kEnd) - 1;
        int ended = jn >= marker && memcmp(joined + jn - marker, kEnd, marker) == 0;
        g_tail_len = jn < sizeof(g_tail) ? jn : sizeof(g_tail);
        memcpy(g_tail, joined + jn - g_tail_len, g_tail_len);
        const size_t len = g_job_len;
        const uint32_t sum = g_job_sum;
        if (ended) {
            g_jobs++;
            g_job_len = 0;
            g_job_sum = 0;
            g_tail_len = 0;
        }
        pthread_mutex_unlock(&g_lock);
        if (ended) {
            char text[64];
            int n = snprintf(text, sizeof(text), "DONE %zu %08x\n", len, sum);
            mimusb_node_endpoint_write(g_node, EP_IN, (const uint8_t*)text, (size_t)n, 2000);
        }
    } else if (e->kind == MIMUSB_EVENT_CONTROL_SETUP &&
               (e->control_flags & MIMUSB_CONTROL_FLAG_ACCEPTED)) {
        pthread_mutex_lock(&g_lock);
        g_resets++; // the only ACCEPT rule is SOFT_RESET
        g_job_len = 0;
        g_job_sum = 0;
        g_tail_len = 0;
        pthread_mutex_unlock(&g_lock);
    }
}

static int usblp_bound(void) {
    DIR* d = opendir("/sys/bus/usb/drivers/usblp");
    if (!d)
        return -1; // not loaded
    struct dirent* ent;
    int bound = 0;
    while ((ent = readdir(d)) != NULL)
        if (strchr(ent->d_name, ':') != NULL)
            bound = 1;
    closedir(d);
    return bound;
}

int main(void) {
    mimusb_device* device = hw_open_device("HW printer test", on_event);
    if (!device)
        return 1;
    mimusb_usb_endpoint in = hw_bulk_endpoint(EP_IN, 4), out = hw_bulk_endpoint(EP_OUT, 4);
    const mimusb_usb_item items[2] = {mimusb_usb_item_make_endpoint(in),
                                      mimusb_usb_item_make_endpoint(out)};
    const mimusb_usb_setting setting = {items, 2};
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0x07;
    itf.subclass = 0x01;
    itf.protocol = 0x02;
    itf.name = "HW printer";
    itf.settings = &setting;
    itf.setting_count = 1;
    // Printing Devices 1.1 4.2.1: GET_DEVICE_ID returns a 2-byte big-endian length (that of the
    // whole answer, these two bytes included) and the IEEE 1284 device ID string.
    static const char id_string[] = "MFG:MIMUSB;CMD:PJL,PCL;MDL:HW Printer;CLS:PRINTER;";
    uint8_t id[2 + sizeof(id_string) - 1];
    id[0] = (uint8_t)(sizeof(id) >> 8);
    id[1] = (uint8_t)sizeof(id);
    memcpy(id + 2, id_string, sizeof(id_string) - 1);
    // 4.2.2: GET_PORT_STATUS: bit 5 paper empty (0), bit 4 selected (1), bit 3 not error (1).
    static const uint8_t port_status = 0x18;
    mimusb_control_rule rules[3];
    memset(rules, 0, sizeof(rules));
    for (int i = 0; i < 3; i++) {
        rules[i].request_type_mask = 0xFF;
        rules[i].request_mask = 0xFF;
    }
    rules[0].request_type_value = 0xA1;
    rules[0].request_value = 0; // GET_DEVICE_ID: wIndex = interface << 8 | alternate setting
    rules[0].index_mask = 0xFFFF;
    rules[0].index_value = 0;
    rules[0].action = MIMUSB_CONTROL_ANSWER;
    rules[0].data = id;
    rules[0].length = sizeof(id);
    rules[1].request_type_value = 0xA1;
    rules[1].request_value = 1; // GET_PORT_STATUS
    rules[1].action = MIMUSB_CONTROL_ANSWER;
    rules[1].data = &port_status;
    rules[1].length = 1;
    rules[2].request_type_value = 0x21;
    rules[2].request_value = 2; // SOFT_RESET
    rules[2].action = MIMUSB_CONTROL_ACCEPT;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    nc.rules = rules;
    nc.rule_count = 3;
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

    printf("A: the kernel's usblp\n");
    int bound = -1;
    for (int i = 0; i < 30 && bound != 1; i++) {
        bound = usblp_bound();
        if (bound != 1)
            sleep_ms(100);
    }
    if (bound < 0)
        printf("  usblp is not loaded here: nothing to check\n");
    else
        printf("  usblp %s the interface\n", bound ? "bound" : "did not bind");
    // Not a failure when the driver is absent; when it is loaded it must bind a printer-class
    // interface.
    CHECK(bound != 0, "usblp is loaded and did not bind the printer interface");

    int e = libusb_claim_interface(h, 0); // the auto-detach sets usblp aside
    CHECK(e == 0, "claim: %s", libusb_error_name(e));

    printf("B: requests and a print job with libusb\n");
    uint8_t got[256];
    int n = libusb_control_transfer(h, 0xA1, 0, 0, 0, got, sizeof(got), 1000);
    CHECK(n == (int)sizeof(id) && memcmp(got, id, sizeof(id)) == 0, "GET_DEVICE_ID: %d bytes", n);
    if (n > 2)
        printf("  device id: %.*s\n", n - 2, (char*)got + 2);
    uint8_t status = 0;
    n = libusb_control_transfer(h, 0xA1, 1, 0, 0, &status, 1, 1000);
    CHECK(n == 1 && status == 0x18, "GET_PORT_STATUS: %d, 0x%02x", n, status);
    n = libusb_control_transfer(h, 0x21, 2, 0, 0, NULL, 0, 1000);
    CHECK(n == 0, "SOFT_RESET: %d", n);
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_resets == 1, "SOFT_RESET told to the program %u times", g_resets);
    pthread_mutex_unlock(&g_lock);

    static uint8_t job[JOB];
    uint32_t sum = 0;
    for (size_t i = 0; i < JOB - (sizeof(kEnd) - 1); i++) {
        job[i] = (uint8_t)(i * 2654435761u >> 24);
    }
    memcpy(job + JOB - (sizeof(kEnd) - 1), kEnd, sizeof(kEnd) - 1);
    for (size_t i = 0; i < JOB; i++)
        sum = sum * 31u + job[i];
    double t0 = now_ms();
    size_t sent = 0;
    while (sent < JOB) {
        int chunk = 0;
        size_t want = JOB - sent < 16384 ? JOB - sent : 16384;
        e = libusb_bulk_transfer(h, EP_OUT, job + sent, (int)want, &chunk, 5000);
        if (e != 0) {
            CHECK(0, "bulk OUT at %zu: %s", sent, libusb_error_name(e));
            break;
        }
        sent += (size_t)chunk;
    }
    double t1 = now_ms();
    char reply[64] = "";
    int rn = 0;
    e = libusb_bulk_transfer(h, EP_IN, (uint8_t*)reply, sizeof(reply) - 1, &rn, 5000);
    reply[rn > 0 ? rn : 0] = 0;
    char want_reply[64];
    snprintf(want_reply, sizeof(want_reply), "DONE %d %08x\n", JOB, sum);
    CHECK(e == 0 && strcmp(reply, want_reply) == 0, "the answer to the job: '%s' (want '%s')",
          reply, want_reply);
    printf("  a job of %d bytes in %.0f ms (%.0f kB/s); the printer answered '%.*s'\n", JOB,
           t1 - t0, JOB / (t1 - t0), rn > 0 ? rn - 1 : 0, reply);
    CHECK(g_jobs == 1, "jobs seen by the program: %u", g_jobs);

    hw_link_check(device);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    mimusb_device_close(device, 500, 1000);
    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails != 0;
}
