// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "bsp/board_api.h"
#include "core.h"
#include "hal.h"

int main(void) {
    hal_init();
    mimhid_core_init();
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }
    mimhid_run();
    return 0;
}
