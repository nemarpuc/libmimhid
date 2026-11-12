// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "pico/multicore.h"
#include "tusb.h"
#include "core.h"
#include "hal.h"

// Core 1: UART parsing and queue dispatch.
static void core1_main(void) {
    for (;;) {
        mimusb_rx_poll();
    }
}

// Core 0: TinyUSB device task and report transmit.
int main(void) {
    hal_init();
    mimusb_core_init();
    multicore_launch_core1(core1_main);

    for (;;) {
        tud_task();
        mimusb_usb_poll();
    }
}
