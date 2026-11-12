// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "bsp/board_api.h"
#include "core.h"
#include "hal.h"

int main(void) {
    hal_init();
    mimusb_core_init();
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }
    mimusb_run();
    return 0;
}
