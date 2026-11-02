// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Raw profile: a caller-supplied descriptor that the library validates but does
 * not generate. This one has Input items only, so requires_output and
 * requires_feature_response stay 0. */
#include "profile_example.h"

/* Generic Desktop Pointer collection, Report ID 1, one 1-bit Input field. */
static const uint8_t k_descriptor[] = {0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                       0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                       0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
static const mimhid_raw_report k_reports[] = {{{1U, 1U, {}}, 2U}};

static const char* const k_send_sequence[] = {
    "mimhid_raw_submit(node, report, sizeof report, MIMHID_SEND_TIMEOUT_MS)   caller-supplied "
    "report bytes",
    " no state is generated: the report id, length, and padding are only checked",
    " against the immutable parsed layout of the caller descriptor", NULL};

int main(void) {
    mimhid_raw_options options;
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_raw_options_init(&options);
    options.descriptor = k_descriptor;
    options.descriptor_length = sizeof k_descriptor;
    options.reports = k_reports;
    options.report_count = sizeof k_reports / sizeof k_reports[0];

    result = mimhid_spec_create_raw(&options, &spec);
    return mimhid_example_run("raw", MIMHID_PROFILE_RAW, result, spec, k_send_sequence);
}
