// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* A board made of the real firmware core (firmware/common) running on the build host: its
 * UART is a file descriptor and TinyUSB is replaced by recording stubs. The end-to-end tests
 * drive the host library against it through a pseudo-terminal. */
#ifndef MIMHID_FAKE_BOARD_H
#define MIMHID_FAKE_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t iface;
    uint16_t length;
    uint8_t data[1024];
} fake_report_t;

// Starts the board thread on a descriptor (the pty master). hid_interfaces is what the firmware
// was compiled for (MIMHID_HID_ITFS).
int fake_board_start(int fd);
void fake_board_stop(void);

// Waits for the next report the board sent to the target; returns 1, or 0 on timeout.
int fake_board_wait_report(fake_report_t* out, uint32_t timeout_ms);
void fake_board_clear_reports(void);
/* While held, the target polls no interface, so reports wait in the board's queues. */
void fake_board_hold_reports(int hold);

// What the target would see right now.
int fake_board_mounted(void);
unsigned fake_board_connects(void);
unsigned fake_board_interface_count(void);
const uint8_t* fake_board_config_descriptor(uint16_t* length);
const uint8_t* fake_board_report_descriptor(uint8_t iface, uint16_t* length);
const uint8_t* fake_board_device_descriptor(void);
size_t fake_board_string(uint8_t index, char* out, size_t capacity); // ASCII; 0 when absent
size_t fake_board_feature(uint8_t iface, uint8_t report_id, uint8_t* out, size_t capacity);

// Things the target does, run on the board thread.
void fake_board_target_set_report(uint8_t iface, uint8_t report_type, uint8_t report_id,
                                  const uint8_t* data, size_t length);
void fake_board_target_suspend(int remote_wakeup);
void fake_board_target_resume(void);
void fake_board_target_set_protocol(uint8_t iface, uint8_t protocol);
void fake_board_target_set_idle(uint8_t iface, uint8_t rate);
void fake_board_set_mount_delay_ms(unsigned ms);

// Control transfers. The target sends a request on endpoint 0; the board's wrapper of the stack's
// event handler sees it first, and what reaches the stack is played out here the way the stack
// would act: a vendor request goes to tud_vendor_control_xfer_cb, any other is left to the stack.
enum {
    FAKE_CONTROL_LEFT = 1,     // handed to the stack as it came
    FAKE_CONTROL_ANSWERED = 2, // a data stage (device to host) was sent
    FAKE_CONTROL_ACKED = 3,    // the transfer completed, with its data stage (host to device) taken
    FAKE_CONTROL_STALLED = 4
};
typedef struct {
    int outcome;
    uint8_t setup[8]; // as the stack got it (a retyped request has the vendor type)
    uint8_t data[1100];
    size_t length;    // what was sent to the target, or what the board took from it
} fake_control_result_t;

// Runs on the board thread; out_data is the data stage of a host-to-device request.
void fake_board_target_control(const uint8_t setup[8], const uint8_t* out_data, size_t out_length);
// The next outcome; returns 1, or 0 on timeout.
int fake_board_wait_control(fake_control_result_t* out, uint32_t timeout_ms);

// The line: bytes pass only while the host's port and the board run at the same rate. A port
// set above the adapter's limit sends at the limit, as adapters that round silently do (0: no
// limit). The board makes rates up to 3,000,000.
uint32_t fake_board_baud(void);
void fake_board_set_adapter_limit(uint32_t baud);

#ifdef __cplusplus
}
#endif

#endif
