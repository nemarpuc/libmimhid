// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The values the board-connected examples (basic_keyboard_mouse, control_transfers, the C++
 * example and everything in verify/) choose. The library fills in nothing: its *_init functions
 * only zero a structure and set struct_size, so every value below is written out here, once, for
 * the examples to share. They are examples of what a program decides; none is a default of the
 * library, and a product picks its own (the limits they sit within are in docs/LIMITS.md). */
#ifndef MIMHID_EXAMPLE_VALUES_H
#define MIMHID_EXAMPLE_VALUES_H

#include "mimhid.h"

#include <stdint.h>

/* Waits that suit a board on a USB serial adapter, in milliseconds. */
#define EXAMPLE_CONTROL_TIMEOUT_MS 500U    /* each configuration frame */
#define EXAMPLE_SEND_TIMEOUT_MS 500U       /* a serial write that makes no progress */
#define EXAMPLE_DRAIN_TIMEOUT_MS 1000U     /* a close: the neutral reports */
#define EXAMPLE_RECONNECT_TIMEOUT_MS 3000U /* the board's USB device enumerated again */

/* A Device for a bus-powered USB 2.0 device with remote wakeup, English strings, and the chip's
 * unique id as the serial number. The vendor and product ID are the pair pid.codes sets aside for
 * testing in-house only: a device you hand out needs IDs of its own. The Linux HID limits are
 * those of the kernel revision in docs/LIMITS.md. */
static inline void example_fill_device_config(mimhid_device_config* config, uint32_t baud_rate,
                                              const char* product) {
    mimhid_device_config_init(config);
    config->transport.baud_rate = baud_rate;
    config->transport.handshake_timeout_ms = 50U;
    config->transport.maximum_report_bytes = 64U;
    config->transport.linux_hid.descriptor_bytes = 4096U;
    config->transport.linux_hid.fields_per_report = 256U;
    config->transport.linux_hid.global_stack_depth = 4U;
    config->transport.linux_hid.report_size_bits = 256U;
    config->transport.linux_hid.usages = 12288U;
    config->transport.linux_hid.report_data_bits = 65528U;
    config->transport.log_level = MIMHID_LOG_DISABLED;
    config->usb_device.bcd_usb = 0x0200U;
    config->usb_device.vendor_id = 0x1209U;
    config->usb_device.product_id = 0x0001U;
    config->usb_device.ep0_max_packet_size = 64U;
    config->usb_device.bcd_device = 0x0100U;
    config->usb_device.language_id = 0x0409U;
    config->usb_device.manufacturer = "libmimhid";
    config->usb_device.product = product;
    config->usb_device.serial_from_chip_id = 1U;
    config->usb_config.configuration_value = 1U;
    config->usb_config.bm_attributes = 0xA0U;
    config->usb_config.bmax_power = 50U;
}

static inline void example_fill_commit_timeouts(mimhid_commit_timeouts* timeouts) {
    mimhid_commit_timeouts_init(timeouts);
    timeouts->control_timeout_ms = EXAMPLE_CONTROL_TIMEOUT_MS;
    timeouts->reconnect_timeout_ms = EXAMPLE_RECONNECT_TIMEOUT_MS;
}

/* A HID interface (HID 1.11, no boot protocol) labelled interface_number, polled every 1 ms
 * through IN endpoint 0x81 + interface_number with 64-byte packets, and no OUT endpoint. Two Nodes
 * with the same number share an interface: give each Node its own unless you mean that. */
static inline void example_fill_node_config(mimhid_node_config* config, uint8_t interface_number) {
    mimhid_node_config_init(config);
    config->iface.number = interface_number;
    config->iface.class_code = 0x03U;
    config->iface.bcd_hid = 0x0111U;
    config->ep_in.address = (uint8_t)(0x81U + interface_number);
    config->ep_in.poll_interval_ms = 1U;
    config->ep_in.max_packet_size = 64U;
}

/* Every key from Keyboard a (0x04) to Keypad Hexadecimal (0xDD). */
static inline void example_fill_keyboard_options(mimhid_keyboard_options* options) {
    mimhid_keyboard_options_init(options);
    options->usage_minimum = 0x04U;
    options->usage_maximum = 0xDDU;
}

/* Five buttons, 16-bit X and Y, and an 8-bit wheel and pan. */
static inline void example_fill_mouse_options(mimhid_mouse_options* options) {
    mimhid_mouse_options_init(options);
    options->button_count = 5U;
    options->x = mimhid_integer_field_make(-32767, 32767, 16U);
    options->y = options->x;
    options->enable_wheel = 1U;
    options->wheel = mimhid_integer_field_make(-127, 127, 8U);
    options->enable_pan = 1U;
    options->pan = options->wheel;
}

/* Ten contacts in one report, X and Y from 0 to 32767, and no optional field. */
static inline void example_fill_touchscreen_options(mimhid_touchscreen_options* options) {
    mimhid_touchscreen_options_init(options);
    options->maximum_contacts = 10U;
    options->contacts_per_report = 10U;
    options->contact_id = mimhid_integer_field_make(0, 15, 4U);
    options->x = mimhid_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->contact_count = mimhid_integer_field_make(0, 10, 4U);
}

#endif
