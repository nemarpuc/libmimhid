// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core.h"
#include "hal.h"

// The loop never blocks, so it runs on core 1 in its own task. tusb_init() is called there so
// the USB interrupt is allocated on the same core.
static void mimusb_task(void* arg) {
    (void)arg;
    hal_init();
    mimusb_core_init();
    mimusb_run();
}

void app_main(void) {
    xTaskCreatePinnedToCore(mimusb_task, "mimusb", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}
