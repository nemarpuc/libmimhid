// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Prints MIMHID_HID_ITFS as seen after the headers are read in the order the build picks
// (ITFS_TUSB_FIRST or not). Every source of the firmware must see the same number, whatever it
// includes first.
#ifdef ITFS_TUSB_FIRST
#include "tusb.h"
#include "core.h"
#else
#include "core.h"
#include "tusb.h"
#endif
#include <stdio.h>

int main(void) {
    printf("%d\n", MIMHID_HID_ITFS);
    return 0;
}
