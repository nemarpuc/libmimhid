// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A composite device on a real board whose target is this machine. Interface 0: a keyboard
// profile (no key is sent). Interfaces 1 and 2: one Node with an interface association (the shape
// of CDC-ACM, vendor class so no driver binds): an interrupt IN endpoint on the first, bulk IN and
// OUT on the second. Opened in one batch, so the target enumerates once.
#include "example_values.h"
#include <libusb.h>
#include <mimusb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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
        } else {                                                                                   \
            printf("  ok: " __VA_ARGS__);                                                          \
        }                                                                                          \
        printf("\n");                                                                              \
    } while (0)
int main(void) {
    mimusb_discovery* found = NULL;
    if (mimusb_discover(3000, &found) != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate,
                               "HW composite test");
    config.usb_device.device_class = 0xEF;
    config.usb_device.device_subclass = 2;
    config.usb_device.device_protocol = 1; // IAD device
    mimusb_device* device = NULL;
    mimusb_result r =
        mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s\n", mimusb_result_name(r));
        return 1;
    }
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    CHECK(mimusb_device_batch_begin(device) == MIMUSB_OK, "batch begin");

    mimusb_keyboard_options ko;
    example_fill_keyboard_options(&ko);
    mimusb_spec* spec = NULL;
    mimusb_spec_create_keyboard(&ko, &spec);
    example_placement hc;
    example_fill_placement(&hc, 0);
    mimusb_node* kbd = NULL;
    r = mimusb_node_open(device, spec, &hc.placement, &to, &kbd);
    CHECK(r == MIMUSB_OK, "keyboard profile staged: %s", mimusb_result_name(r));

    mimusb_usb_endpoint e1[1], e2[2];
    memset(e1, 0, sizeof(e1));
    memset(e2, 0, sizeof(e2));
    e1[0].address = 0x82;
    e1[0].transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    e1[0].max_packet_size = 16;
    e1[0].interval = 10;
    e1[0].policy.on_full = MIMUSB_FULL_DROP_OLDEST;
    e1[0].policy.depth = 2;
    e2[0].address = 0x83;
    e2[0].transfer_type = MIMUSB_TRANSFER_BULK;
    e2[0].max_packet_size = 64;
    e2[0].policy.on_full = MIMUSB_FULL_WAIT;
    e2[0].policy.depth = 4;
    e2[1].address = 0x03;
    e2[1].transfer_type = MIMUSB_TRANSFER_BULK;
    e2[1].max_packet_size = 64;
    mimusb_usb_interface itf[2];
    memset(itf, 0, sizeof(itf));
    itf[0].number = 1;
    itf[0].class_code = 0xFF;
    itf[0].name = "Control";
    const mimusb_usb_item it1[1] = {mimusb_usb_item_make_endpoint(e1[0])};
    const mimusb_usb_item it2[2] = {mimusb_usb_item_make_endpoint(e2[0]),
                                    mimusb_usb_item_make_endpoint(e2[1])};
    const mimusb_usb_setting st1 = {it1, 1};
    const mimusb_usb_setting st2 = {it2, 2};
    itf[0].settings = &st1;
    itf[0].setting_count = 1;
    itf[1].number = 2;
    itf[1].class_code = 0xFF;
    itf[1].name = "Data";
    itf[1].settings = &st2;
    itf[1].setting_count = 1;
    const uint8_t iad[8] = {8, 0x0B, 1, 2, 0xFF, 0, 0, 0}; // first interface 1, two interfaces
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = itf;
    nc.interface_count = 2;
    nc.association = iad;
    nc.association_length = 8;
    mimusb_node* dat = NULL;
    r = example_node_open_interfaces(device, &nc, &to, &dat);
    CHECK(r == MIMUSB_OK, "two-interface Node staged: %s %s", mimusb_result_name(r),
          r ? mimusb_last_error()->reason : "");
    r = mimusb_device_batch_end(device, &to);
    CHECK(r == MIMUSB_OK, "batch end (one enumeration): %s %s", mimusb_result_name(r),
          r ? mimusb_last_error()->reason : "");
    r = mimusb_device_wait_ready(device, 5000);
    CHECK(r == MIMUSB_OK, "mounted: %s", mimusb_result_name(r));
    sleep_ms(300);
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
    if (h) {
        struct libusb_config_descriptor* cd = NULL;
        libusb_get_active_config_descriptor(libusb_get_device(h), &cd);
        CHECK(cd && cd->bNumInterfaces == 3, "the target sees %d interfaces",
              cd ? cd->bNumInterfaces : -1);
        for (int i = 0; cd && i < cd->bNumInterfaces; i++) {
            const struct libusb_interface_descriptor* id = &cd->interface[i].altsetting[0];
            printf("    interface %d: class %02x, %d endpoint(s), driver %s\n",
                   id->bInterfaceNumber, id->bInterfaceClass, id->bNumEndpoints,
                   libusb_kernel_driver_active(h, i) == 1 ? "bound" : "none");
        }
        int e = libusb_claim_interface(h, 2);
        CHECK(e == 0, "claim the data interface: %s", libusb_error_name(e));
        e = libusb_claim_interface(h, 1);
        CHECK(e == 0, "claim the control interface: %s", libusb_error_name(e));
        uint8_t b[256];
        int got = 0;
        const uint8_t msg[] = "bulk through the composite device";
        r = mimusb_node_endpoint_write(dat, 0x83, msg, sizeof(msg), 1000);
        CHECK(r == MIMUSB_OK, "bulk write: %s", mimusb_result_name(r));
        e = libusb_bulk_transfer(h, 0x83, b, sizeof(b), &got, 2000);
        CHECK(e == 0 && got == (int)sizeof(msg) && memcmp(b, msg, got) == 0,
              "bulk IN arrives (%d bytes)", got);
        const uint8_t note[10] = {0xA1, 0x20, 0, 0, 1, 0, 2, 0, 3, 0};
        r = mimusb_node_endpoint_write(dat, 0x82, note, sizeof(note), 1000);
        CHECK(r == MIMUSB_OK, "interrupt write: %s", mimusb_result_name(r));
        e = libusb_interrupt_transfer(h, 0x82, b, 16, &got, 2000);
        CHECK(e == 0 && got == 10 && memcmp(b, note, 10) == 0,
              "interrupt IN of the other interface arrives (%d bytes)", got);
        libusb_release_interface(h, 1);
        libusb_release_interface(h, 2);
        libusb_close(h);
    } else
        CHECK(0, "libusb cannot open the device");
    libusb_exit(ctx);
    r = mimusb_device_close(device, 500, 1000);
    CHECK(r == MIMUSB_OK, "device_close: %s", mimusb_result_name(r));
    mimusb_spec_release(spec);
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
