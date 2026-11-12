// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Shared board-connection helper for examples/c/verify/. Each program here is
 * meant to be run with a real board and a real target machine and watched, not
 * just compiled: it prints what to do, waits, then does one clearly described
 * thing. See docs/QUICKSTART.md for the setup this assumes.
 */
#ifndef MIMUSB_VERIFY_COMMON_H
#define MIMUSB_VERIFY_COMMON_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
/* Must be defined before any system header is included in this translation
 * unit, or glibc's feature-test-macro selection will already be locked in
 * and nanosleep() below will not be declared. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "../example_values.h"
#include "mimusb.h"

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
static int verify_fail(const char* what, mimusb_result result) {
    /* Read before mimusb_result_name(), a public call that replaces the record;
     * argument evaluation order would otherwise decide which one is printed. */
    const mimusb_error_detail* detail = mimusb_last_error();
    const char* field = detail != NULL && detail->field != NULL ? detail->field : "no field";
    const char* reason = detail != NULL && detail->reason != NULL ? detail->reason : "no reason";
    fprintf(stderr, "%s failed: %s (%s: %s)\n", what, mimusb_result_name(result), field, reason);
    return 1;
}

/* Sends the pending edge. The library never retries: a timeout may have been
 * delivered, so it is reported rather than resent. */
static mimusb_result verify_submit(mimusb_node* node) {
    return mimusb_node_submit_blocking(node, EXAMPLE_SEND_TIMEOUT_MS);
}

/* Finds the first board this host can see and opens it. On success the caller
 * owns *out_device and must close it; on failure *out_device is null and a
 * message has already been printed. The USB identity is the pid.codes test ID
 * pair (example_values.h), for testing in-house only: a device you hand out needs IDs of its own.
 */
static int verify_open_first_board(mimusb_device** out_device) {
    *out_device = NULL;

    mimusb_discovery* discovery = NULL;
    mimusb_result result = mimusb_discover(50, &discovery);
    if (result != MIMUSB_OK)
        return verify_fail("discover", result);
    if (mimusb_discovery_count(discovery) == 0U) {
        fprintf(stderr, "no mimusb board found. Is it plugged in, and may you open its serial "
#ifdef _WIN32
                        "port? "
#else
                        "port (the udev rule, the dialout group)? "
#endif
                        "See docs/QUICKSTART.md.\n");
        mimusb_discovery_destroy(discovery);
        return 1;
    }
    const mimusb_device_info* info = mimusb_discovery_get(discovery, 0U);
    printf("using board: %s\n", info->port_name);

    mimusb_device_config config;
    example_fill_device_config(&config, info->baud_rate, "libmimusb verify");

    result = mimusb_device_open(info->port_name, &config, out_device);
    mimusb_discovery_destroy(discovery);
    if (result != MIMUSB_OK)
        return verify_fail("device open", result);
    return 0;
}

/* Opens one Node for `spec` on USB interface `interface_number` and waits until
 * the target has enumerated the board. */
VERIFY_MAYBE_UNUSED static mimusb_result verify_open_node(mimusb_device* device, mimusb_spec* spec,
                                                          uint8_t interface_number,
                                                          mimusb_node** out_node) {
    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimusb_hid_node_config node_config;
    example_fill_node_config(&node_config, interface_number);
    mimusb_result result =
        mimusb_node_open_profile(device, spec, &node_config, &timeouts, out_node);
    if (result == MIMUSB_OK)
        result = mimusb_device_wait_ready(device, EXAMPLE_RECONNECT_TIMEOUT_MS);
    return result;
}

#endif
