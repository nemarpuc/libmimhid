// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// A composite device on a real board whose target is this machine. Interface 0: a keyboard
// profile (no key is sent). Interfaces 1 and 2: one Node with an interface association (the shape
// of CDC-ACM, vendor class so no driver binds): an interrupt IN endpoint on the first, bulk IN and
// OUT on the second. Opened in one batch, so the target enumerates once.
#include <libusb.h>
#include <mimhid.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "example_values.h"
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); } else { printf("  ok: " __VA_ARGS__); } printf("\n"); } while (0)
int main(void) {
    mimhid_discovery* found = NULL;
    if (mimhid_discover(3000, &found) != MIMHID_OK || mimhid_discovery_count(found) == 0) { printf("no board\n"); return 1; }
    mimhid_device_config config; example_fill_device_config(&config, mimhid_discovery_get(found, 0)->baud_rate, "HW composite test");
    config.usb_device.device_class = 0xEF; config.usb_device.device_subclass = 2; config.usb_device.device_protocol = 1; // IAD device
    mimhid_device* device = NULL;
    mimhid_result r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK) { printf("open: %s\n", mimhid_result_name(r)); return 1; }
    mimhid_commit_timeouts to; example_fill_commit_timeouts(&to);
    CHECK(mimhid_device_batch_begin(device) == MIMHID_OK, "batch begin");

    mimhid_keyboard_options ko; example_fill_keyboard_options(&ko); mimhid_spec* spec = NULL; mimhid_spec_create_keyboard(&ko, &spec);
    mimhid_hid_node_config hc; example_fill_node_config(&hc, 0);
    mimhid_node* kbd = NULL; r = mimhid_node_open_profile(device, spec, &hc, &to, &kbd);
    CHECK(r == MIMHID_OK, "keyboard profile staged: %s", mimhid_result_name(r));

    mimhid_usb_endpoint e1[1], e2[2]; memset(e1, 0, sizeof(e1)); memset(e2, 0, sizeof(e2));
    e1[0].address = 0x82; e1[0].transfer_type = MIMHID_TRANSFER_INTERRUPT; e1[0].max_packet_size = 16; e1[0].interval = 10;
    e1[0].policy.on_full = MIMHID_FULL_DROP_OLDEST; e1[0].policy.depth = 2;
    e2[0].address = 0x83; e2[0].transfer_type = MIMHID_TRANSFER_BULK; e2[0].max_packet_size = 64; e2[0].policy.on_full = MIMHID_FULL_WAIT; e2[0].policy.depth = 4;
    e2[1].address = 0x03; e2[1].transfer_type = MIMHID_TRANSFER_BULK; e2[1].max_packet_size = 64;
    mimhid_usb_interface itf[2]; memset(itf, 0, sizeof(itf));
    itf[0].number = 1; itf[0].class_code = 0xFF; itf[0].name = "Control"; itf[0].endpoints = e1; itf[0].endpoint_count = 1;
    itf[1].number = 2; itf[1].class_code = 0xFF; itf[1].name = "Data"; itf[1].endpoints = e2; itf[1].endpoint_count = 2;
    const uint8_t iad[8] = {8, 0x0B, 1, 2, 0xFF, 0, 0, 0}; // first interface 1, two interfaces
    mimhid_node_config nc; mimhid_node_config_init(&nc); nc.interfaces = itf; nc.interface_count = 2; nc.association = iad; nc.association_length = 8;
    mimhid_node* dat = NULL; r = mimhid_node_open(device, &nc, &to, &dat);
    CHECK(r == MIMHID_OK, "two-interface Node staged: %s %s", mimhid_result_name(r), r ? mimhid_last_error()->reason : "");
    r = mimhid_device_batch_end(device, &to); CHECK(r == MIMHID_OK, "batch end (one enumeration): %s %s", mimhid_result_name(r), r ? mimhid_last_error()->reason : "");
    r = mimhid_device_wait_ready(device, 5000); CHECK(r == MIMHID_OK, "mounted: %s", mimhid_result_name(r));
    sleep_ms(300);
    libusb_context* ctx = NULL; libusb_init(&ctx);
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
    if (h) {
        struct libusb_config_descriptor* cd = NULL; libusb_get_active_config_descriptor(libusb_get_device(h), &cd);
        CHECK(cd && cd->bNumInterfaces == 3, "the target sees %d interfaces", cd ? cd->bNumInterfaces : -1);
        for (int i = 0; cd && i < cd->bNumInterfaces; i++) {
            const struct libusb_interface_descriptor* id = &cd->interface[i].altsetting[0];
            printf("    interface %d: class %02x, %d endpoint(s), driver %s\n", id->bInterfaceNumber, id->bInterfaceClass, id->bNumEndpoints, libusb_kernel_driver_active(h, i) == 1 ? "bound" : "none");
        }
        int e = libusb_claim_interface(h, 2); CHECK(e == 0, "claim the data interface: %s", libusb_error_name(e));
        e = libusb_claim_interface(h, 1); CHECK(e == 0, "claim the control interface: %s", libusb_error_name(e));
        uint8_t b[256]; int got = 0;
        const uint8_t msg[] = "bulk through the composite device";
        r = mimhid_node_endpoint_write(dat, 0x83, msg, sizeof(msg), 1000); CHECK(r == MIMHID_OK, "bulk write: %s", mimhid_result_name(r));
        e = libusb_bulk_transfer(h, 0x83, b, sizeof(b), &got, 2000); CHECK(e == 0 && got == (int)sizeof(msg) && memcmp(b, msg, got) == 0, "bulk IN arrives (%d bytes)", got);
        const uint8_t note[10] = {0xA1, 0x20, 0, 0, 1, 0, 2, 0, 3, 0};
        r = mimhid_node_endpoint_write(dat, 0x82, note, sizeof(note), 1000); CHECK(r == MIMHID_OK, "interrupt write: %s", mimhid_result_name(r));
        e = libusb_interrupt_transfer(h, 0x82, b, 16, &got, 2000); CHECK(e == 0 && got == 10 && memcmp(b, note, 10) == 0, "interrupt IN of the other interface arrives (%d bytes)", got);
        libusb_release_interface(h, 1); libusb_release_interface(h, 2); libusb_close(h);
    } else CHECK(0, "libusb cannot open the device");
    libusb_exit(ctx);
    r = mimhid_device_close(device, 500, 1000); CHECK(r == MIMHID_OK, "device_close: %s", mimhid_result_name(r));
    mimhid_spec_release(spec);
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
