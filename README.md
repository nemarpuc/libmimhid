# libmimusb

[Canonical repository](https://github.com/nemarpuc/libmimusb)

Make a microcontroller board (Raspberry Pi Pico RP2040 / RP2350, Nordic nRF52840, STM32F4,
or ESP32-S3) show any USB device you describe to another computer, controlled from this PC
over a high-speed UART link. The target sees a standard USB device and needs no driver, no app,
and no agent: ready-made HID profiles (keyboard, mouse, gamepad, touchscreen, touchpad, pen,
media keys), interfaces of any other class from their descriptors, or a whole device given as
bytes. Linux, Windows, and macOS hosts; usable from C, C++, Python, C#, and Rust. MIT licensed.

> Version 0.6.0. Changes are listed in [CHANGELOG.md](CHANGELOG.md).

`libmimusb` is a C++20 library with a stable C ABI. For the HID profiles you describe the USB device
identity and each HID interface (ranges, bit widths, Report IDs, Usages,
endpoints); the library validates the descriptor against the USB HID 1.11, HUT
1.7, and target Linux parser rules, streams the configuration over UART to the
board, and keeps the report state machines (multi-touch contacts, key rollover,
pointer deltas, pen tool transitions) bit-exact. The board holds its USB data
lines at 0 V until your first configuration is committed, so no generic vendor
or bootloader device ever appears on the target bus. The device does not have to
be HID: `mimusb_node_open` presents interfaces of any class (bulk and interrupt
endpoints, your own control rules), and `mimusb_device_set_descriptors` presents
a whole device given as the bytes the target reads, a BOS descriptor included.

Sister library of [`libaoahid`](https://github.com/nemarpuc/libaoahid), which
sends the same kind of input to an Android device over a plain USB cable using
Android Open Accessory 2.0, with no microcontroller board needed. Both libraries
share the same `Spec` / `Node` model and profile state machines.

## What it is not

- **Not a source of defaults.** Every `*_init` function zeroes its structure and
  sets `struct_size` only. Any required USB, transport, endpoint, or profile
  field left zero is rejected with `MIMUSB_ERR_UNSET_FIELD` (`mimusb_last_error`
  names the exact field).
- **Not a scheduler.** The library generates no key repeat, debounce, or timed
  motion, and stores no timeout on a handle: every wait is passed to the call
  that waits.
- **Not reflashed per device.** One firmware image per MCU serves any USB
  identity, interface layout, and report descriptor sent at runtime.
- **Not hardware-verified.** No release has been verified on a board; see
  [Verification status](#verification-status).

## Install

Requirements: a C11/C++20 compiler and CMake 3.20 or later; no external host
libraries are needed. To talk to a real board you also need one supported MCU
flashed with `mimusb_firmware` and a 3.3 V USB-UART adapter
([QUICKSTART.md](docs/QUICKSTART.md)).

- **Release archive.** The [Releases](https://github.com/nemarpuc/libmimusb/releases) page publishes **exactly 18 uploaded assets** per release: one `libmimusb-X.Y.Z-<platform>-<arch>[-ubuntu22.04]` archive per platform (Linux, Windows, macOS) carrying both the shared and static libraries, the headers, and the CMake and `pkg-config` files, alongside the prebuilt firmware images, source archive, SPDX 2.3 SBOM, and SHA-256 checksums ([RELEASES.md](docs/RELEASES.md)).
- **CMake.** `find_package(mimusb 0.3 CONFIG REQUIRED COMPONENTS shared)` or `COMPONENTS static` (or both), then link `mimusb::mimusb` or `mimusb::mimusb_static` (or `mimusb::cpp` / `mimusb::cpp_static` for the C++20 header). A `0.3` request is satisfied by a `0.3.x` install only.
- **pkg-config.** `pkg-config --cflags --libs mimusb` for the shared library, or `pkg-config --static --cflags --libs mimusb-static` for the static archive.
- **vcpkg.** Not needed: the host library has zero external dependencies and uses operating-system serial APIs (`termios` on Linux and macOS, Win32 on Windows) directly.
- **From source.** `cmake -S . -B build -DMIMUSB_BUILD_SHARED=ON -DMIMUSB_BUILD_STATIC=ON && cmake --build build --config Release && cmake --install build --prefix staging --config Release`. Every CMake option, the presets, the firmware builds, and the sanitizer and coverage commands are in [BUILDING.md](docs/BUILDING.md).

## First program

Pressing and releasing the `a` key on a board on `/dev/ttyUSB0`. Every value the
library needs is set, including `transport.log_level`: a zero there is refused
like any other unset field. See
[examples/c/basic_keyboard_mouse.c](examples/c/basic_keyboard_mouse.c) for port
discovery.

```c
#define _POSIX_C_SOURCE 200809L /* nanosleep */
#include "mimusb.h"

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void pause_ms(unsigned ms) {
    const struct timespec t = {0, (long)ms * 1000000L};
    nanosleep(&t, NULL);
}
#endif

static int failed(const char* call, mimusb_result result) {
    /* Read the record first: mimusb_result_name is a call too and replaces it. */
    const mimusb_error_detail* detail = mimusb_last_error();
    const char* field = detail->field != NULL ? detail->field : "-";
    const char* reason = detail->reason;
    fprintf(stderr, "%s: %s (%s: %s)\n", call, mimusb_result_name(result), field, reason);
    return 1;
}

int main(void) {
    mimusb_device_config dev_cfg;
    mimusb_node_placement place;
    mimusb_placed_endpoint in_ep;
    mimusb_commit_timeouts waits;
    mimusb_keyboard_options kb_opts;
    mimusb_device_config_init(&dev_cfg);
    mimusb_node_placement_init(&place);
    mimusb_commit_timeouts_init(&waits);
    mimusb_keyboard_options_init(&kb_opts);

    dev_cfg.transport.baud_rate = MIMUSB_BAUD_FASTEST;
    dev_cfg.transport.handshake_timeout_ms = 200;
    dev_cfg.transport.maximum_report_bytes = 64;
    dev_cfg.transport.linux_hid = (mimusb_linux_hid_policy){4096, 256, 4, 256, 12288, 65528};
    dev_cfg.transport.log_level = MIMUSB_LOG_DISABLED; /* zero is refused as unset */
    dev_cfg.usb_device.bcd_usb = 0x0200;
    dev_cfg.usb_device.ep0_max_packet_size = 64;
    dev_cfg.usb_device.vendor_id = 0x1209; /* pid.codes test IDs: in-house testing only */
    dev_cfg.usb_device.product_id = 0x0001;
    dev_cfg.usb_device.language_id = 0x0409;
    dev_cfg.usb_config.configuration_value = 1;
    dev_cfg.usb_config.bm_attributes = 0x80;
    dev_cfg.usb_config.bmax_power = 50;
    in_ep = (mimusb_placed_endpoint){MIMUSB_HID_ROLE_IN, 0,
                                     {0x81, MIMUSB_TRANSFER_INTERRUPT, 1, 0, 64, 0,
                                      {MIMUSB_FULL_DROP_NEWEST, 7}}};
    place.endpoints = &in_ep; /* the Spec's roles: mimusb_spec_roles */
    place.endpoint_count = 1;
    waits.control_timeout_ms = 500;
    waits.reconnect_timeout_ms = 3000;
    kb_opts.function.bcd_hid = 0x0111; /* HID 1.11: the Spec says what the function is */
    kb_opts.usage_minimum = 0x04;
    kb_opts.usage_maximum = 0xDD;

    mimusb_device* dev = NULL;
    mimusb_spec* spec = NULL;
    mimusb_node* kb = NULL;
    mimusb_result r = mimusb_device_open("/dev/ttyUSB0", &dev_cfg, &dev);
    if (r != MIMUSB_OK)
        return failed("mimusb_device_open", r);
    int status = 1;
    if ((r = mimusb_spec_create_keyboard(&kb_opts, &spec)) != MIMUSB_OK) {
        failed("mimusb_spec_create_keyboard", r);
    } else if ((r = mimusb_node_open(dev, spec, &place, &waits, &kb)) != MIMUSB_OK) {
        failed("mimusb_node_open", r);
    } else if ((r = mimusb_device_wait_ready(dev, 5000)) != MIMUSB_OK) {
        failed("mimusb_device_wait_ready", r);
    } else if ((r = mimusb_keyboard_key(kb, 0x04, 1)) != MIMUSB_OK || /* Keyboard a */
               (r = mimusb_node_submit(kb, 500)) != MIMUSB_OK ||
               (r = mimusb_keyboard_key(kb, 0x04, 0)) != MIMUSB_OK ||
               (r = mimusb_node_submit(kb, 500)) != MIMUSB_OK) {
        failed("typing a", r);
    } else {
        status = 0;
    }
    /* Taking the device off the target drops reports still queued on the board, so give the
     * target, which polls every bInterval (1 ms here), time to read them first. */
    pause_ms(50);
    if (kb != NULL && (r = mimusb_node_close(kb, &waits, 1000)) != MIMUSB_OK)
        status = failed("mimusb_node_close", r);
    if (spec != NULL)
        mimusb_spec_release(spec);
    if ((r = mimusb_device_close(dev, 500, 1000)) != MIMUSB_OK)
        status = failed("mimusb_device_close", r);
    return status;
}
```

On Linux, from the directory of a release archive that holds `include/` and
`lib/` (the static library needs the C++ runtime; [examples/README.md](examples/README.md)
has the other link lines):

```sh
cc -std=c11 -Iinclude first.c lib/libmimusb_static.a -lstdc++ -lpthread -o first
```

[QUICKSTART.md](docs/QUICKSTART.md) walks through wiring a board, setting serial
permissions, and running the watchable demos under
[examples/c/verify/](examples/c/verify).

## Profiles

| Profile | Factory | Input | Output / Feature |
|---|---|---|---|
| Keyboard (NKRO bitmap) | `mimusb_spec_create_keyboard` | 8 modifiers + one bit per Usage from `usage_minimum` (at least `0x04`) to `usage_maximum` | — |
| Mouse (relative pointer) | `mimusb_spec_create_mouse` | `1..65535` buttons, `X`/`Y`, optional Wheel and AC Pan | — |
| Toggle controls | `mimusb_spec_create_toggle` | `1..65535` one-bit controls from an allow-list on a page of the caller's choice (Consumer `0x0C`, Generic Desktop `0x01`, Telephony `0x0B`, Camera Control `0x90`, ...) | — |
| Gamepad | `mimusb_spec_create_gamepad` | `1..65535` buttons, at least two caller-declared axes (`X` and `Y` among them), optional hat or 4-button D-pad | — |
| Touchscreen (multi-touch) | `mimusb_spec_create_touchscreen` | `1..16` contact slots (`X`, `Y`, optional pressure, width, height, azimuth), optional Scan Time | Optional `Contact Count Maximum` Feature report |
| Touchpad | `mimusb_spec_create_touchpad` | `1..16` contact slots + `0..65535` buttons | Optional `Contact Count Maximum` Feature report |
| Pen (direct screen) | `mimusb_spec_create_pen` | In Range, Tip, optional Invert (eraser), up to two barrel switches, `X`/`Y`, optional pressure, tilt, twist | — |
| Digitizer | `mimusb_spec_create_digitizer` | The Pen report under `Digitizers / Digitizer` (`0x0D/0x01`) | — |
| Raw descriptor | `mimusb_spec_create_raw` | Any Input report (`mimusb_raw_submit` checks it against the Spec) | Output reports and Feature writes arrive through `transport.event_sink`; Feature reads are answered with `mimusb_node_set_feature_response` |

A HID interface that is not a profile (a descriptor copied from a real device) is a
`mimusb_node_open` Node of class 0x03 with its own control rules and
`mimusb_node_endpoint_write` ([examples/c/hid_interface.c](examples/c/hid_interface.c)).
The board has no HID driver: every interface runs on one generic driver, and
what HID asks of endpoint 0 is a table of control rules that the library writes
for a profile or you write yourself.

Any generated profile can also include a battery-strength Input report. Full
descriptor layouts and state-machine rules are in [PROFILES.md](docs/PROFILES.md).

## Moving between libaoahid and libmimusb

Both libraries share the profile `Spec` factories, the report state machines,
and the input calls (`aoahid_keyboard_key` and `mimusb_keyboard_key`, and so
on). They differ where the hardware differs: `libmimusb` has no `Context`
handle, takes a serial port name and the full USB and endpoint descriptors
(`mimusb_device_config`, `mimusb_node_config`, `mimusb_node_placement`), uses `mimusb_commit_timeouts` on
calls that re-enumerate the board, and carries two-way HID traffic (Output
reports, Feature answers, bus connect and disconnect, endpoint 0 control
transfer routing). Three differences change code that is moved across:

- **Initializers.** `libaoahid`'s `*_init` functions fill in values that work
  with a current Android phone, and its header has the timeout constants
  `AOAHID_CONTROL_TIMEOUT_MS`, `AOAHID_SEND_TIMEOUT_MS` and
  `AOAHID_DRAIN_TIMEOUT_MS`. `libmimusb`'s `*_init` functions only zero the
  structure and set `struct_size`, there are no timeout constants, and a
  required field or timeout left at zero is refused with
  `MIMUSB_ERR_UNSET_FIELD`.
- **Closing.** `libaoahid` has `AOAHID_CLOSE_PENDING` (15): a transfer of the
  object is still in flight and the caller calls again later. In `libmimusb`
  writes to the serial port finish within the call, so a close never leaves
  anything pending and code 15 is not used.
- **Result code 4.** It is `AOAHID_ERR_NOT_AOA` in `libaoahid` and
  `MIMUSB_ERR_PROTOCOL` (firmware of another major or minor version) here.

The full comparison is in [API.md](docs/API.md#differences-from-libaoahid).

## Verification status

- **Hardware:** no release is hardware-verified. [CHANGELOG.md](CHANGELOG.md)
  records for 0.1.0 that an earlier firmware ran on a Raspberry Pi Pico and
  that the 0.1.0 firmware has not, and for 0.2.0 that none of it has run on a
  board yet.
- **One latency measurement** is recorded in
  [LATENCY.md](docs/LATENCY.md#measured-once), as it was written.
- **Automated CI:** unit, scripted-board (`tests/scripted`), and simulated-board
  (`tests/sim`, the real C firmware core over a pseudo-terminal) suites, also
  under ASan/UBSan and ThreadSanitizer, and an 86% line / 73% branch coverage
  floor on `src/` (the `Coverage` job).
- **Details and commands:** [VERIFICATION.md](docs/VERIFICATION.md) has the
  feature-by-layer matrix and the commands to reproduce each check.

## Documentation

| Document | Contents |
|---|---|
| [QUICKSTART.md](docs/QUICKSTART.md) | Wiring a board, flashing firmware, permissions, and running the verification demos |
| [BUILDING.md](docs/BUILDING.md) | Host requirements, CMake options and presets, firmware builds, package consumption, bindings |
| [API.md](docs/API.md) | C and C++ API reference: handles, structs, rules, every public function with its results, result codes |
| [PROFILES.md](docs/PROFILES.md) | Descriptor and state-machine contract of all nine profiles and the battery report |
| [EXAMPLES.md](docs/EXAMPLES.md) | Walkthrough of the C, C++, Python, C#, and Rust example programs |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | System structure, threading, data paths, and design decisions |
| [INTERNALS.md](docs/INTERNALS.md) | Contributor guide: source map, handle internals, locking, teardown, design decisions |
| [PROTOCOL.md](docs/PROTOCOL.md) | Binary UART wire protocol (frames, CRC-16, commands, events, line-rate handshake) |
| [LIMITS.md](docs/LIMITS.md) | Every numeric limit with its USB, HID, chip, or firmware source |
| [LATENCY.md](docs/LATENCY.md) | Where time goes, the one recorded latency measurement, and how to measure |
| [PORTING.md](docs/PORTING.md) | Adding a new MCU port (the 12 HAL functions) or a new host OS serial backend |
| [TARGET_MATRIX.md](docs/TARGET_MATRIX.md) | Build and verification status per MCU board, host OS, and target |
| [VERIFICATION.md](docs/VERIFICATION.md) | Feature × test-layer verification matrix, coverage numbers, and reproduction commands |
| [RELEASES.md](docs/RELEASES.md) | The 18 release assets, archive layout, platform baselines, and component licenses |
| [CHANGELOG.md](CHANGELOG.md) | Release history and hardware verification notes |

## License

`libmimusb`, its headers, examples, language bindings, and portable firmware
core are MIT licensed ([LICENSE](LICENSE)). Prebuilt firmware images link
TinyUSB (MIT) and vendor HALs under BSD-3-Clause and Apache-2.0; see
[RELEASES.md](docs/RELEASES.md#licenses) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## AI disclosure

Most of the code, tests, and documentation in this repository were written by
an AI coding assistant. Architecture and design decisions, hardware
verification steps, debugging, and review of the AI's output were done by a
human. The hardware status is in
[TARGET_MATRIX.md](docs/TARGET_MATRIX.md); review the code before relying on
it for anything security- or safety-relevant.
