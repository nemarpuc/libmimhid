// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Digitizer profile: the same stylus report shape as Pen (pressure, tilt,
 * hover, and the HUT Invert selector for the opposite end) under the
 * Digitizers / Digitizer Application Collection. Barrel controls are
 * individual Digitizers-page Usage IDs because HUT does not define them as a
 * range. */
#include "profile_example.h"

static const uint16_t k_barrel_usages[] = {0x44U, 0x5AU};

static const char* const k_send_sequence[] = {
    "mimhid_digitizer_update(node, &sample)   tip, pressure, and in-range stay consistent",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)        Away state is normalized on the wire",
    "mimhid_digitizer_update(node, &sample)   in_range 0 takes the tool away; repeat the last X/Y",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)        an in-range pen/eraser switch departs "
    "first",
    NULL};

int main(void) {
    mimhid_digitizer_options options;
    const mimhid_integer_field tilt = {-90, 90, 8U, {0, 0, 0, 0, 0}};
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_digitizer_options_init(&options);
    options.enable_tilt = 1U;
    options.tilt_x = tilt;
    options.tilt_y = tilt;
    options.barrel_usages = k_barrel_usages;
    options.barrel_usage_count = sizeof k_barrel_usages / sizeof k_barrel_usages[0];
    options.enable_eraser = 1U;

    result = mimhid_spec_create_digitizer(&options, &spec);
    return mimhid_example_run("digitizer", MIMHID_PROFILE_DIGITIZER, result, spec, k_send_sequence);
}
