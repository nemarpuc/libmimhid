// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* End to end: the host library talks through a pseudo-terminal to the real firmware core,
 * which presents what a USB target would see. The scenarios are in test_e2e_*.cpp; this runs
 * them in an order that matters (the last one closes the line). */
#include "e2e_support.hpp"

#include <cstdio>

int main() {
    using namespace e2e;
    if (!open_pty()) {
        std::fprintf(stderr, "cannot create a pseudo-terminal\n");
        return 1;
    }
    if (fake_board_start(g_master) != 0)
        return 1;

#ifdef E2E_HIGH_SPEED
    test_high_speed_rules();
    fake_board_stop();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("end-to-end OK (high speed)\n");
    return g_failures == 0 ? 0 : 1;
#endif
    test_zero_identity();
    test_bad_arguments();
    test_line_rate();
    test_no_names();
    test_open_identity_and_descriptors();
    test_reports_and_neutral();
    test_queue_depth_and_loss();
    test_touch_feature();
    test_batch_and_packing();
    test_limits();
    test_events_and_raw();
    test_chip_serial_and_boot();
    test_keep_attached_and_discovery();
    test_control();
    test_filled_values();
    test_init_fills_nothing();
    test_shared_interfaces();
    test_link_loss();

    fake_board_stop();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("end-to-end OK (%d HID interfaces)\n", E2E_HID_ITFS);
    return g_failures == 0 ? 0 : 1;
}
