// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// MCU and OS configuration
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the build system
#endif

// The build of the port states the OS, the root hub port mode and the device speed; none has a
// default here (a missing one would be a full-speed, bare-metal guess).
#ifndef CFG_TUSB_OS
#error CFG_TUSB_OS must be defined by the build system
#endif
#ifndef CFG_TUSB_RHPORT0_MODE
#error CFG_TUSB_RHPORT0_MODE must be defined by the build system
#endif
#ifndef CFG_TUD_MAX_SPEED
#error CFG_TUD_MAX_SPEED must be defined by the build system
#endif
// tusb_init() with no argument runs the stack on root hub port 0, and so does this firmware.
#if defined(BOARD_TUD_RHPORT) && (BOARD_TUD_RHPORT != 0)
#error the firmware runs the stack on root hub port 0 only
#endif

// Device Mode enabled
#define CFG_TUD_ENABLED             1

// Endpoint 0 size
#define CFG_TUD_ENDPOINT0_SIZE      64

#include "core.h"
#include "config.h"

// One HID class instance per USB HID interface the MCU's endpoints allow
#define CFG_TUD_HID                 MIMHID_HID_ITFS

// HID buffer per interface: a report, and the data stage of a Feature report
#define CFG_TUD_HID_EP_BUFSIZE      MIMHID_HID_BUFSIZE

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
