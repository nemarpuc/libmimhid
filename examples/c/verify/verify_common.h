// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Shared board-connection helper for examples/c/verify/. Each program here is
 * meant to be run with a real board and a real target machine and watched, not
 * just compiled: it prints what to do, waits, then does one clearly described
 * thing. See docs/QUICKSTART.md for the setup this assumes.
 */
#ifndef MIMHID_VERIFY_COMMON_H
#define MIMHID_VERIFY_COMMON_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
/* Must be defined before any system header is included in this translation
 * unit, or glibc's feature-test-macro selection will already be locked in
 * and nanosleep() below will not be declared. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "../example_values.h"
#include "mimhid.h"

#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* A helper that only some of the programs use must not warn in the others. */
#if defined(__GNUC__)
#define VERIFY_MAYBE_UNUSED __attribute__((unused))
#else
#define VERIFY_MAYBE_UNUSED
#endif

/* nanosleep is POSIX-only; Windows has no equivalent libc symbol, so this
 * dispatches to Sleep() there instead. */
static void verify_sleep_ms(int milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Prints the field/reason from the calling thread's last error and returns 1,
 * so every failure path in a verify program can just `return verify_fail(...)`. */
static int verify_fail(const char* what, mimhid_result result) {
    /* Read before mimhid_result_name(), a public call that replaces the record;
     * argument evaluation order would otherwise decide which one is printed. */
    const mimhid_error_detail* detail = mimhid_last_error();
    const char* field = detail != NULL && detail->field != NULL ? detail->field : "no field";
    const char* reason = detail != NULL && detail->reason != NULL ? detail->reason : "no reason";
    fprintf(stderr, "%s failed: %s (%s: %s)\n", what, mimhid_result_name(result), field, reason);
    return 1;
}

/* Sends the pending edge. The library never retries: a timeout may have been
 * delivered, so it is reported rather than resent. */
static mimhid_result verify_submit(mimhid_node* node) {
    return mimhid_node_submit_blocking(node, EXAMPLE_SEND_TIMEOUT_MS);
}

/* Finds the first board this host can see and opens it. On success the caller
 * owns *out_device and must close it; on failure *out_device is null and a
 * message has already been printed. The USB identity is the pid.codes test ID
 * pair (example_values.h), for testing in-house only: a device you hand out needs IDs of its own.
 */
static int verify_open_first_board(mimhid_device** out_device) {
    *out_device = NULL;

    mimhid_discovery* discovery = NULL;
    mimhid_result result = mimhid_discover(50, &discovery);
    if (result != MIMHID_OK)
        return verify_fail("discover", result);
    if (mimhid_discovery_count(discovery) == 0U) {
        fprintf(stderr, "no mimhid board found. Is it plugged in, and may you open its serial "
#ifdef _WIN32
                        "port? "
#else
                        "port (the udev rule, the dialout group)? "
#endif
                        "See docs/QUICKSTART.md.\n");
        mimhid_discovery_destroy(discovery);
        return 1;
    }
    const mimhid_device_info* info = mimhid_discovery_get(discovery, 0U);
    printf("using board: %s\n", info->port_name);

    mimhid_device_config config;
    example_fill_device_config(&config, info->baud_rate, "libmimhid verify");

    result = mimhid_device_open(info->port_name, &config, out_device);
    mimhid_discovery_destroy(discovery);
    if (result != MIMHID_OK)
        return verify_fail("device open", result);
    return 0;
}

/* Opens one Node for `spec` on USB interface `interface_number` and waits until
 * the target has enumerated the board. */
VERIFY_MAYBE_UNUSED static mimhid_result verify_open_node(mimhid_device* device, mimhid_spec* spec,
                                                          uint8_t interface_number,
                                                          mimhid_node** out_node) {
    mimhid_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimhid_hid_node_config node_config;
    example_fill_node_config(&node_config, interface_number);
    mimhid_result result =
        mimhid_node_open_profile(device, spec, &node_config, &timeouts, out_node);
    if (result == MIMHID_OK)
        result = mimhid_device_wait_ready(device, EXAMPLE_RECONNECT_TIMEOUT_MS);
    return result;
}

#endif
