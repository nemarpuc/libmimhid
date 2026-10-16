// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "pico/multicore.h"
#include "tusb.h"
#include "core.h"
#include "hal.h"

// Core 1: UART parsing and queue dispatch.
static void core1_main(void) {
    for (;;) {
        mimhid_rx_poll();
    }
}

// Core 0: TinyUSB device task and report transmit.
int main(void) {
    hal_init();
    mimhid_core_init();
    multicore_launch_core1(core1_main);

    for (;;) {
        tud_task();
        mimhid_usb_poll();
    }
}
