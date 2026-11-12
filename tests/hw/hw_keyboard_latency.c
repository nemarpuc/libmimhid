// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Latency of a keyboard profile Node on a real board whose target is this machine: the time from
// just before mimhid_node_submit to the kernel's evdev timestamp (docs/LATENCY.md, method b). The
// event device is grabbed before any key is sent, so nothing is typed into the session.
//
//   hw_keyboard_latency [presses]     (500 when not given)
//
// One run's median moves by some 0.3 ms from one enumeration to the next: compare versions over
// several runs each, never one against one.
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mimhid.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "example_values.h"

static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static void sleep_us(long us) { struct timespec t = {us / 1000000, (us % 1000000) * 1000L}; nanosleep(&t, NULL); }

// The event device of the keyboard with this vendor and product, or -1.
static int open_event_device(void) {
    DIR* d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent* e; int fd = -1;
    while ((e = readdir(d)) != NULL && fd < 0) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char p[512], v[16] = "", pr[16] = "";
        snprintf(p, sizeof(p), "/sys/class/input/%s/device/id/vendor", e->d_name);
        FILE* f = fopen(p, "r"); if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
        snprintf(p, sizeof(p), "/sys/class/input/%s/device/id/product", e->d_name);
        f = fopen(p, "r"); if (f) { if (!fgets(pr, sizeof(pr), f)) pr[0] = 0; fclose(f); }
        if (strncmp(v, "1209", 4) != 0 || strncmp(pr, "0001", 4) != 0) continue;
        snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
            int has_a = (keys[KEY_A / (8 * sizeof(long))] >> (KEY_A % (8 * sizeof(long)))) & 1;
            if (!has_a) { close(fd); fd = -1; } else printf("event device %s\n", p);
        }
    }
    closedir(d);
    return fd;
}

int main(int argc, char** argv) {
    const int presses = argc > 1 ? atoi(argv[1]) : 500;
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(3000, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) { printf("no board\n"); return 1; }
    mimhid_device_config config;
    example_fill_device_config(&config, mimhid_discovery_get(found, 0)->baud_rate, "HW keyboard test");
    printf("board on %s at %u baud\n", mimhid_discovery_get(found, 0)->port_name, mimhid_discovery_get(found, 0)->baud_rate);
    mimhid_device* device = NULL;
    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK) { printf("open: %s\n", mimhid_result_name(r)); return 1; }
    mimhid_keyboard_options ko; example_fill_keyboard_options(&ko);
    mimhid_spec* spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&ko, &spec)) != MIMHID_OK) { printf("spec: %s\n", mimhid_result_name(r)); return 1; }
    mimhid_hid_node_config nc; example_fill_node_config(&nc, 0);
    mimhid_commit_timeouts to; example_fill_commit_timeouts(&to);
    mimhid_node* node = NULL;
    if ((r = mimhid_node_open_profile(device, spec, &nc, &to, &node)) != MIMHID_OK) { printf("node_open: %s %s\n", mimhid_result_name(r), mimhid_last_error()->reason); return 1; }
    if ((r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK) { printf("wait_ready: %s\n", mimhid_result_name(r)); return 1; }

    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) { fd = open_event_device(); if (fd < 0) sleep_us(50000); }
    if (fd < 0) { printf("no event device for 1209:0001 (permissions?)\n"); return 1; }
    if (ioctl(fd, EVIOCGRAB, 1) != 0) { perror("EVIOCGRAB"); return 1; } // nothing reaches the session
    int clk = CLOCK_MONOTONIC; ioctl(fd, EVIOCSCLOCKID, &clk);
    printf("grabbed; sending %d presses\n", presses);

    double* down = malloc(sizeof(double) * presses); double* up = malloc(sizeof(double) * presses);
    int nd = 0, nu = 0, missed = 0;
    srand(1);
    for (int i = 0; i < presses; i++) {
        for (int phase = 1; phase >= 0; phase--) {
            sleep_us(8100 + rand() % 1200); // not locked to the 1 ms frame
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
            mimhid_keyboard_key(node, 0x04, (uint32_t)phase);
            r = mimhid_node_submit(node, 100);
            if (r != MIMHID_OK) { printf("submit: %s\n", mimhid_result_name(r)); return 1; }
            struct pollfd pf = {fd, POLLIN, 0}; int got = 0;
            double end_ms = 200; struct timespec ts;
            while (!got) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                double el = (ts.tv_sec - t0.tv_sec) * 1e3 + (ts.tv_nsec - t0.tv_nsec) / 1e6;
                if (el > end_ms) break;
                if (poll(&pf, 1, 20) <= 0) continue;
                struct input_event ev[16]; ssize_t n = read(fd, ev, sizeof(ev));
                for (int k = 0; k < (int)(n / (ssize_t)sizeof(ev[0])); k++) {
                    if (ev[k].type == EV_KEY && ev[k].code == KEY_A && ev[k].value == phase) {
                        double us = (ev[k].time.tv_sec - t0.tv_sec) * 1e6 + ev[k].time.tv_usec - t0.tv_nsec / 1e3;
                        if (phase) down[nd++] = us; else up[nu++] = us;
                        got = 1;
                    }
                }
            }
            if (!got) missed++;
        }
    }
    ioctl(fd, EVIOCGRAB, 0); close(fd);
    qsort(down, nd, sizeof(double), cmp); qsort(up, nu, sizeof(double), cmp);
    printf("presses %d, releases %d, missed %d\n", nd, nu, missed);
    if (nd > 10) printf("submit -> key-down (us): min %.0f median %.0f p90 %.0f p99 %.0f max %.0f\n", down[0], down[nd / 2], down[nd * 9 / 10], down[nd * 99 / 100], down[nd - 1]);
    if (nu > 10) printf("submit -> key-up   (us): min %.0f median %.0f p90 %.0f p99 %.0f max %.0f\n", up[0], up[nu / 2], up[nu * 9 / 10], up[nu * 99 / 100], up[nu - 1]);
    mimhid_link_counters lc; memset(&lc, 0, sizeof(lc)); lc.struct_size = sizeof(lc);
    r = mimhid_device_link_counters(device, 0, 500, &lc);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n", mimhid_result_name(r), lc.bad_crc, lc.framing, lc.uart_overrun, lc.ring_overrun);
    r = mimhid_node_close(node, &to, 1000); printf("node_close: %s\n", mimhid_result_name(r));
    mimhid_spec_release(spec);
    r = mimhid_device_close(device, 500, 1000); printf("device_close: %s\n", mimhid_result_name(r));
    return missed != 0;
}
