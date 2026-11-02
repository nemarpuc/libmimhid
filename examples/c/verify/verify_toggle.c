// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Sends a short, human-observable sequence of Consumer Control taps so a
 * person watching the target (and, ideally, an input viewer on it such as
 * `evtest` on Linux) can confirm whether each Usage actually reaches the
 * target at all, and if so what it does there.
 *
 * This intentionally tests only Consumer Control Usages (media mute,
 * play/pause, volume, and the "AC New" selector) because every one of them is
 * safe to send unattended: worst case is a volume change or a paused video,
 * both trivially reversible. It deliberately does NOT send System Control
 * Power Down/Sleep/Wake Usages, because those can suspend, lock, or power off
 * the target mid-test with no way for this program to detect or undo it. If
 * you need to verify a System Control Usage, send it one at a time by hand
 * with the target unlocked, watching an input viewer the whole time.
 *
 * Before running: unlock the target and leave the screen on, ideally with a
 * media app (e.g. a paused video/music track) in the foreground so Play/Pause
 * and Mute have something observable to act on. See docs/QUICKSTART.md for
 * cable/udev setup. Ctrl+C to stop early. */
#include "verify_common.h"

typedef struct {
    uint16_t usage;
    mimhid_usage_semantic semantic;
    const char* label;
    /* The Linux event this Usage is expected to produce; mimhid_spec_create_toggle
     * requires this (and its EV_* type) for every allow-listed Usage -- see
     * examples/c/profiles/toggle.c. A best-guess default mapping, meant to be
     * confirmed or corrected against this run's actual input viewer output. */
    const char* expected_type;
    const char* expected_code;
} toggle_case;

/* Same allow-list and semantics as examples/c/profiles/toggle.c. HUT 1.7
 * section 3.4 gives each control type (OSC, RTC, OOC, Sel) a different
 * press/release encoding, so each Usage needs its own explicit semantic
 * instead of an inferred one. */
static const toggle_case k_cases[] = {
    {0x00CDU, MIMHID_USAGE_ONE_SHOT, "Play/Pause (Consumer 0x0C/0x00CD, OSC)", "EV_KEY",
     "KEY_PLAYPAUSE"},
    {0x00E9U, MIMHID_USAGE_RETRIGGER, "Volume Increment (Consumer 0x0C/0x00E9, RTC)", "EV_KEY",
     "KEY_VOLUMEUP"},
    {0x00E2U, MIMHID_USAGE_ON_OFF_MAINTAINED, "Mute (Consumer 0x0C/0x00E2, OOC)", "EV_KEY",
     "KEY_MUTE"},
    {0x0201U, MIMHID_USAGE_SELECTOR_BITMAP, "AC New (Consumer 0x0C/0x0201, Sel)", "EV_KEY",
     "KEY_NEW"},
};
/* A macro, not a `static const size_t`: MSVC's C front end does not treat a
 * named const object as an integer constant expression, so using one as an
 * array bound below would fail to compile there (unlike GCC/Clang, which
 * accept it). A macro expands to the literal sizeof-division at each use, so
 * every compiler sees the same ordinary constant expression. */
#define TOGGLE_CASE_COUNT (sizeof k_cases / sizeof k_cases[0])

static int run_case(mimhid_node* node, const toggle_case* c) {
    printf("\n-- sending: %s --\n", c->label);
    printf("   watch the target now (and your input viewer, if you have it running)\n");
    mimhid_result result = mimhid_toggle_usage(node, c->usage, 1U);
    if (result != MIMHID_OK)
        return verify_fail("toggle down", result);
    result = verify_submit(node);
    if (result != MIMHID_OK)
        return verify_fail("submit toggle down", result);
    verify_sleep_ms(600);
    result = mimhid_toggle_usage(node, c->usage, 0U);
    if (result != MIMHID_OK)
        return verify_fail("toggle up", result);
    result = verify_submit(node);
    if (result != MIMHID_OK)
        return verify_fail("submit toggle up", result);
    verify_sleep_ms(1200);
    return 0;
}

int main(void) {
    mimhid_device* device = NULL;
    if (verify_open_first_board(&device) != 0)
        return 1;

    mimhid_toggle_options options;
    mimhid_toggle_options_init(&options);

    uint16_t usages[TOGGLE_CASE_COUNT];
    mimhid_usage_semantic semantics[TOGGLE_CASE_COUNT];
    const char* expected_types[TOGGLE_CASE_COUNT];
    const char* expected_codes[TOGGLE_CASE_COUNT];
    for (size_t i = 0; i < TOGGLE_CASE_COUNT; ++i) {
        usages[i] = k_cases[i].usage;
        semantics[i] = k_cases[i].semantic;
        expected_types[i] = k_cases[i].expected_type;
        expected_codes[i] = k_cases[i].expected_code;
    }
    options.allowed_usages = usages;
    options.allowed_usage_count = TOGGLE_CASE_COUNT;
    options.usage_semantics = semantics;
    options.expected_linux_event_types = expected_types;
    options.expected_linux_codes = expected_codes;

    mimhid_spec* spec = NULL;
    mimhid_result result = mimhid_spec_create_toggle(&options, &spec);
    if (result != MIMHID_OK) {
        mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
        return verify_fail("toggle spec create", result);
    }

    mimhid_node* node = NULL;
    result = verify_open_node(device, spec, 0U, &node);
    mimhid_spec_release(spec);
    if (result != MIMHID_OK) {
        mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
        return verify_fail("toggle node open", result);
    }

    printf("Unlock the target and put a media app in the foreground now.\n");
    printf("If available, run an input viewer on the target (for example `evtest` on Linux).\n");
    printf("Starting in 3 seconds...\n");
    verify_sleep_ms(3000);

    int exit_code = 0;
    for (size_t i = 0; i < TOGGLE_CASE_COUNT && exit_code == 0; ++i)
        exit_code = run_case(node, &k_cases[i]);

    if (exit_code == 0) {
        printf("\ndone. For each Usage above, record in TARGET_MATRIX.md whether the target (or\n"
               "the input viewer) reacted at all:\n"
               "  - reacted as expected            -> evidence toward Hardware-verified\n"
               "  - the viewer showed the edge but the target did nothing -> the input layer "
               "accepted it,\n"
               "    the desktop or app did not react (record as Conditional)\n"
               "  - nothing at all, not even in the viewer -> record as Failed for this exact\n"
               "    target/build, with the kernel log and viewer output attached\n");
    }
    mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
    return exit_code;
}
