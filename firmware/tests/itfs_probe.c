// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Prints what the port's build states (MIMHID_HID_ITFS, MIMHID_MAX_ENDPOINT_NUMBER,
// MIMHID_MAX_ENDPOINT_BYTES) as seen after the headers are read in the order the build picks
// (ITFS_TUSB_FIRST or not). Every source of the firmware must see the same numbers, whatever it
// includes first. Built without one of them, it must not compile (core.h says so).
#ifdef ITFS_TUSB_FIRST
#include "tusb.h"
#include "core.h"
#else
#include "core.h"
#include "tusb.h"
#endif
#include <stdio.h>

int main(void) {
    printf("%d %d %d\n", MIMHID_HID_ITFS, MIMHID_MAX_ENDPOINT_NUMBER, MIMHID_MAX_ENDPOINT_BYTES);
    return 0;
}
