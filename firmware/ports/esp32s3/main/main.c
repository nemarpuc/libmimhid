// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core.h"
#include "hal.h"

// The loop never blocks, so it runs on core 1 in its own task. tusb_init() is called there so
// the USB interrupt is allocated on the same core.
static void mimhid_task(void* arg) {
    (void)arg;
    hal_init();
    mimhid_core_init();
    mimhid_run();
}

void app_main(void) {
    xTaskCreatePinnedToCore(mimhid_task, "mimhid", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}
