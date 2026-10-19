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

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

// Root Hub Port 0 configuration: Device Mode + Full Speed
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

// Device Mode enabled
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

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
