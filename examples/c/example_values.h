// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The values the board-connected examples (basic_keyboard_mouse, control_transfers, the C++
 * example and everything in verify/) choose. The library fills in nothing: its *_init functions
 * only zero a structure and set struct_size, so every value below is written out here, once, for
 * the examples to share. They are examples of what a program decides; none is a default of the
 * library, and a product picks its own (the limits they sit within are in docs/LIMITS.md). */
#ifndef MIMUSB_EXAMPLE_VALUES_H
#define MIMUSB_EXAMPLE_VALUES_H

#include "mimusb.h"

#include <string.h>

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
static inline void example_fill_device_config(mimusb_device_config* config, uint32_t baud_rate,
                                              const char* product) {
    mimusb_device_config_init(config);
    config->transport.baud_rate = baud_rate;
    config->transport.handshake_timeout_ms = 50U;
    config->transport.maximum_report_bytes = 64U;
    config->transport.linux_hid.descriptor_bytes = 4096U;
    config->transport.linux_hid.fields_per_report = 256U;
    config->transport.linux_hid.global_stack_depth = 4U;
    config->transport.linux_hid.report_size_bits = 256U;
    config->transport.linux_hid.usages = 12288U;
    config->transport.linux_hid.report_data_bits = 65528U;
    config->transport.log_level = MIMUSB_LOG_DISABLED;
    config->usb_device.bcd_usb = 0x0200U;
    config->usb_device.vendor_id = 0x1209U;
    config->usb_device.product_id = 0x0001U;
    config->usb_device.ep0_max_packet_size = 64U;
    config->usb_device.bcd_device = 0x0100U;
    config->usb_device.language_id = 0x0409U;
    config->usb_device.manufacturer = "libmimusb";
    config->usb_device.product = product;
    config->usb_device.serial_from_chip_id = 1U;
    config->usb_config.configuration_value = 1U;
    config->usb_config.bm_attributes = 0xA0U;
    config->usb_config.bmax_power = 50U;
}

static inline void example_fill_commit_timeouts(mimusb_commit_timeouts* timeouts) {
    mimusb_commit_timeouts_init(timeouts);
    timeouts->control_timeout_ms = EXAMPLE_CONTROL_TIMEOUT_MS;
    timeouts->reconnect_timeout_ms = EXAMPLE_RECONNECT_TIMEOUT_MS;
}

/* The placement of a HID Spec: bInterfaceNumber interface_number, polled every 1 ms through IN
 * endpoint 0x81 + interface_number with 64-byte packets, and no OUT endpoint. The numbers in use
 * must be 0, 1, 2, ... without a gap (open them in that order, or in one batch). Two Nodes with the
 * same number share an interface: give each Node its own unless you mean that. The placement
 * points at the endpoint inside the same structure: use it where it is filled, do not copy it. */
typedef struct example_placement {
    mimusb_node_placement placement;
    mimusb_placed_endpoint in_endpoint;
} example_placement;

static inline void example_fill_placement(example_placement* p, uint8_t interface_number) {
    mimusb_node_placement_init(&p->placement);
    memset(&p->in_endpoint, 0, sizeof(p->in_endpoint));
    p->in_endpoint.role = MIMUSB_HID_ROLE_IN;
    p->in_endpoint.endpoint.address = (uint8_t)(0x81U + interface_number);
    p->in_endpoint.endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    p->in_endpoint.endpoint.interval = 1U;
    p->in_endpoint.endpoint.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    p->in_endpoint.endpoint.policy.depth = 7U;
    p->in_endpoint.endpoint.max_packet_size = 64U;
    p->placement.first_interface = interface_number;
    p->placement.endpoints = &p->in_endpoint;
    p->placement.endpoint_count = 1U;
}

/* Opens the interfaces of a mimusb_node_config as one Node: a Spec of the kind
 * MIMUSB_PROFILE_INTERFACES with an empty placement (the interfaces carry their own numbers and
 * endpoints). The Node keeps its own reference to the Spec. */
static inline mimusb_result example_node_open_interfaces(mimusb_device* device,
                                                         const mimusb_node_config* config,
                                                         const mimusb_commit_timeouts* timeouts,
                                                         mimusb_node** out_node) {
    mimusb_spec* spec = NULL;
    mimusb_node_placement placement;
    mimusb_result result = mimusb_spec_create_interfaces(config, &spec);
    if (result != MIMUSB_OK)
        return result;
    mimusb_node_placement_init(&placement);
    result = mimusb_node_open(device, spec, &placement, timeouts, out_node);
    mimusb_spec_release(spec);
    return result;
}

/* Every key from Keyboard a (0x04) to Keypad Hexadecimal (0xDD). */
static inline void example_fill_keyboard_options(mimusb_keyboard_options* options) {
    mimusb_keyboard_options_init(options);
    options->function.bcd_hid = 0x0111U; /* HID 1.11 */
    options->usage_minimum = 0x04U;
    options->usage_maximum = 0xDDU;
}

/* Five buttons, 16-bit X and Y, and an 8-bit wheel and pan. */
static inline void example_fill_mouse_options(mimusb_mouse_options* options) {
    mimusb_mouse_options_init(options);
    options->function.bcd_hid = 0x0111U; /* HID 1.11 */
    options->button_count = 5U;
    options->x = mimusb_integer_field_make(-32767, 32767, 16U);
    options->y = options->x;
    options->enable_wheel = 1U;
    options->wheel = mimusb_integer_field_make(-127, 127, 8U);
    options->enable_pan = 1U;
    options->pan = options->wheel;
}

/* Ten contacts in one report, X and Y from 0 to 32767, and no optional field. */
static inline void example_fill_touchscreen_options(mimusb_touchscreen_options* options) {
    mimusb_touchscreen_options_init(options);
    options->function.bcd_hid = 0x0111U; /* HID 1.11 */
    options->maximum_contacts = 10U;
    options->contacts_per_report = 10U;
    options->contact_id = mimusb_integer_field_make(0, 15, 4U);
    options->x = mimusb_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->contact_count = mimusb_integer_field_make(0, 10, 4U);
}

#endif
