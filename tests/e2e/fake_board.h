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
    uint8_t length;
    uint8_t data[64];
} fake_report_t;

// Starts the board thread on a descriptor (the pty master). hid_interfaces is what the firmware
// was compiled for (MIMHID_HID_ITFS).
int fake_board_start(int fd);
void fake_board_stop(void);

// Waits for the next report the board sent to the target; returns 1, or 0 on timeout.
int fake_board_wait_report(fake_report_t* out, uint32_t timeout_ms);
void fake_board_clear_reports(void);

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

#ifdef __cplusplus
}
#endif

#endif
