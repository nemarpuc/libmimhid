// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Toggle profile: a sparse allow-list of one-bit selector controls on any HUT
 * page. application_page/application_usage pick the Application Collection
 * (here HUT 0x0C/0x01 Consumer Control); field_page picks the Usage Page each
 * allowed_usages entry is read from. The same three fields let one factory
 * reach System Control (0x01/0x80), Camera keys (0x90 nested under Consumer),
 * or Telephony keys (0x0B/0x01) instead -- see the commented alternatives
 * below. Every Usage still needs an explicit HUT semantic and the target
 * event the caller expects it to produce, because the library refuses to
 * guess either one. */
#include "profile_example.h"

static const uint16_t k_usages[] = {0x00CDU, 0x00E9U, 0x00E2U, 0x0201U};
static const mimusb_usage_semantic k_semantics[] = {MIMUSB_USAGE_ONE_SHOT, MIMUSB_USAGE_RETRIGGER,
                                                    MIMUSB_USAGE_ON_OFF_MAINTAINED,
                                                    MIMUSB_USAGE_SELECTOR_BITMAP};
/* Placeholders. A product must name the events it verified on its own target. */
static const char* const k_expected[] = {"TEST_EVENT_CODE", "TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                         "TEST_EVENT_CODE"};

static const char* const k_send_sequence[] = {
    "mimusb_toggle_usage(node, 0x00CD, 1)   assert one allow-listed Usage",
    "mimusb_node_submit(node, send_timeout_ms)  sends the 1 edge",
    "mimusb_toggle_usage(node, 0x00CD, 0)   release it",
    "mimusb_node_submit(node, send_timeout_ms)  sends the 0 edge that completes the tap", NULL};

int main(void) {
    mimusb_toggle_options options;
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_toggle_options_init(&options);
    options.function.bcd_hid = 0x0111; /* HID 1.11 */
    /* Consumer Control. For System Control set the three pages to 0x01/0x80/0x01; for Camera keys
     * keep 0x0C/0x01 and set field_page to 0x90 (Auto-focus 0x20 and Shutter 0x21 only); for
     * Telephony keys 0x0B/0x01/0x0B. */
    options.application_page = 0x0CU;
    options.application_usage = 0x01U;
    options.field_page = 0x0CU;
    options.allowed_usages = k_usages;
    options.allowed_usage_count = sizeof k_usages / sizeof k_usages[0];
    options.usage_semantics = k_semantics;
    options.expected_linux_event_types = k_expected;
    options.expected_linux_codes = k_expected;

    result = mimusb_spec_create_toggle(&options, &spec);
    return mimusb_example_run("toggle", MIMUSB_PROFILE_TOGGLE, result, spec, k_send_sequence);
}
