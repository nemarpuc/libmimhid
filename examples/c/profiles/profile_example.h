// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Shared reporting helper for the per-profile examples. It holds no product
 * values: each example supplies its own complete option set. */
#ifndef MIMHID_PROFILE_EXAMPLE_H
#define MIMHID_PROFILE_EXAMPLE_H

#include "mimhid.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Prints the descriptor and report layout the library derived from the caller's
 * options, then the exact call sequence that would send this profile through a
 * real board, then releases the Spec. Returns a process exit code.
 *
 * `send_sequence` is the per-profile part of that walkthrough: the mutation
 * calls that sit between opening a Node and submitting a report. It is printed
 * rather than executed because none of these examples opens a board. */
static int mimhid_example_run(const char* name, mimhid_profile_kind expected_kind,
                              mimhid_result creation_result, mimhid_spec* spec,
                              const char* const* send_sequence) {
    mimhid_capability_manifest manifest;
    const uint8_t* descriptor = NULL;
    size_t descriptor_length = 0U;
    mimhid_result result = creation_result;
    size_t index;
    size_t step;

    mimhid_capability_manifest_init(&manifest);
    if (result == MIMHID_OK && spec != NULL)
        result = mimhid_spec_manifest(spec, &manifest);
    if (result == MIMHID_OK)
        result = mimhid_spec_descriptor(spec, &descriptor, &descriptor_length);
    if (result != MIMHID_OK || spec == NULL) {
        const mimhid_error_detail* detail = mimhid_last_error();
        const char* field = detail == NULL || detail->field == NULL ? "no field" : detail->field;
        const char* reason =
            detail == NULL || detail->reason == NULL ? "no detail" : detail->reason;
        fprintf(stderr, "%s: %s (%s: %s)\n", name, mimhid_result_name(result), field, reason);
        mimhid_spec_release(spec);
        return 1;
    }
    if (manifest.profile_kind != expected_kind || descriptor_length != manifest.descriptor_length) {
        fprintf(stderr, "%s: manifest disagrees with the requested profile\n", name);
        mimhid_spec_release(spec);
        return 1;
    }

    printf("%s: descriptor %zu bytes, %zu report(s), input %s\n", name, descriptor_length,
           manifest.report_count, manifest.input_supported != 0U ? "yes" : "no");
    for (index = 0U; index < manifest.report_count; ++index) {
        const mimhid_report_capability report = manifest.reports[index];
        if (report.report_id.enabled != 0U)
            printf("  report id %u: %u wire bytes\n", (unsigned)report.report_id.value,
                   (unsigned)report.wire_length);
        else
            printf("  no report id: %u wire bytes\n", (unsigned)report.wire_length);
    }

    /* The Spec above is complete and needs no board. Everything below is the
     * remaining work to make a target react to it. The steps are identical for
     * every profile except the mutation calls, which each example supplies. */
    printf("\nTo send this profile through a real board:\n");
    printf("  1. mimhid_discover()           then mimhid_discovery_get() to pick a board\n");
    printf("  2. mimhid_device_open()        USB identity and serial link, both given\n");
    printf("  3. mimhid_node_open()          adds this Spec as a Node on an interface\n");
    printf("     (every call that waits takes its timeout as an argument; none has a default)\n");
    printf("  4. mimhid_device_wait_ready()  the target has enumerated the board\n");
    step = 5U;
    for (index = 0U; send_sequence != NULL && send_sequence[index] != NULL; ++index) {
        /* A leading space marks a continuation of the previous step rather
         * than a new call, so it must not consume a step number. */
        if (send_sequence[index][0] == ' ')
            printf("    %s\n", send_sequence[index] + 1);
        else
            printf("  %zu. %s\n", step++, send_sequence[index]);
    }
    printf("  %zu. mimhid_device_close()     closes its Nodes too\n", step);
    printf("\nA complete runnable session is examples/c/basic_keyboard_mouse.c.\n");
    printf("A watchable real-board demo is examples/c/verify/ (docs/QUICKSTART.md).\n");

    mimhid_spec_release(spec);
    return 0;
}

#endif
