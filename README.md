# libmimhid

[Canonical repository](https://github.com/nemarpuc/libmimhid)

Send keyboard, mouse, gamepad, touchscreen, touchpad, pen, and media-key input
from a PC to any USB host through a microcontroller board (Raspberry Pi Pico
RP2040 / RP2350, Nordic nRF52840, STM32F4, or ESP32-S3) connected over a high-speed
UART link. The target machine sees a standard USB composite HID device and needs
no driver, no app, and no agent. Linux, Windows, and macOS hosts; usable from C,
C++, Python, C#, and Rust. MIT licensed.

> Version 0.2.0. Changes are listed in [CHANGELOG.md](CHANGELOG.md).

`libmimhid` is a C++20 library with a stable C ABI. You describe the USB device
identity and each HID interface (ranges, bit widths, Report IDs, Usages,
endpoints); the library validates the descriptor against the USB HID 1.11, HUT
1.7, and target Linux parser rules, streams the configuration over UART to the
board, and keeps the report state machines (multi-touch contacts, key rollover,
pointer deltas, pen tool transitions) bit-exact. The board holds its USB data
lines at 0 V until your first configuration is committed, so no generic vendor
or bootloader device ever appears on the target bus.

Sister library of [`libaoahid`](https://github.com/nemarpuc/libaoahid), which
sends the same kind of input to an Android device over a plain USB cable using
Android Open Accessory 2.0, with no microcontroller board needed. Both libraries
share the same `Spec` / `Node` model and profile state machines.

## What it is not

- **Not a source of defaults.** Every `*_init` function zeroes its structure and
  sets `struct_size` only. Any required USB, transport, endpoint, or profile
  field left zero is rejected with `MIMHID_ERR_UNSET_FIELD` (`mimhid_last_error`
  names the exact field).
- **Not a scheduler.** The library generates no key repeat, debounce, or timed
  motion, and stores no timeout on a handle: every wait is passed to the call
  that waits.
- **Not reflashed per device.** One firmware image per MCU serves any USB
  identity, interface layout, and report descriptor sent at runtime.
- **Not verified on every board.** Only the RP2040 has been tested on physical
  hardware; see [Verification status](#verification-status).

## Install

Requirements: a C11/C++20 compiler and CMake 3.20 or later; no external host
libraries are needed. To talk to a real board you also need one supported MCU
flashed with `mimhid_firmware` and a 3.3 V USB-UART adapter
([QUICKSTART.md](docs/QUICKSTART.md)).

- **Release archive.** The [Releases](https://github.com/nemarpuc/libmimhid/releases) page publishes **exactly 18 uploaded assets** per release: one `libmimhid-X.Y.Z-<platform>-<arch>[-ubuntu22.04]` archive per platform (Linux, Windows, macOS) carrying both the shared and static libraries, the headers, and the CMake and `pkg-config` files, alongside the prebuilt firmware images, source archive, SPDX 2.3 SBOM, and SHA-256 checksums ([RELEASES.md](docs/RELEASES.md)).
- **CMake.** `find_package(mimhid 0.2 CONFIG REQUIRED COMPONENTS shared)` or `COMPONENTS static` (or both), then link `mimhid::mimhid` or `mimhid::mimhid_static` (or `mimhid::cpp` / `mimhid::cpp_static` for the C++20 header). A `0.2` request is satisfied by a `0.2.x` install only.
- **pkg-config.** `pkg-config --cflags --libs mimhid` for the shared library, or `pkg-config --static --cflags --libs mimhid-static` for the static archive.
- **vcpkg.** Not needed: the host library has zero external dependencies and uses operating-system serial APIs (`termios` on Linux and macOS, Win32 on Windows) directly.
- **From source.** `cmake -S . -B build -DMIMHID_BUILD_SHARED=ON -DMIMHID_BUILD_STATIC=ON && cmake --build build --config Release && cmake --install build --prefix staging --config Release`. Every CMake option, the presets, the firmware builds, and the sanitizer and coverage commands are in [BUILDING.md](docs/BUILDING.md).

## First program

Pressing and releasing the `a` key on a board on `/dev/ttyUSB0` (see
[examples/c/basic_keyboard_mouse.c](examples/c/basic_keyboard_mouse.c) for port
discovery and full error reporting):

```c
#include "mimhid.h"

int main(void) {
    mimhid_device_config dev_cfg;
    mimhid_node_config node_cfg;
    mimhid_commit_timeouts waits;
    mimhid_keyboard_options kb_opts;
    mimhid_device_config_init(&dev_cfg);
    mimhid_node_config_init(&node_cfg);
    mimhid_commit_timeouts_init(&waits);
    mimhid_keyboard_options_init(&kb_opts);

    dev_cfg.transport.baud_rate = MIMHID_BAUD_FASTEST;
    dev_cfg.transport.handshake_timeout_ms = 200;
    dev_cfg.transport.maximum_report_bytes = 64;
    dev_cfg.transport.linux_hid = (mimhid_linux_hid_policy){4096, 256, 4, 256, 12288, 65528};
    dev_cfg.usb_device.bcd_usb = 0x0200;
    dev_cfg.usb_device.ep0_max_packet_size = 64;
    dev_cfg.usb_device.vendor_id = 0x1209;
    dev_cfg.usb_device.product_id = 0x0001;
    dev_cfg.usb_device.language_id = 0x0409;
    dev_cfg.usb_config.configuration_value = 1;
    dev_cfg.usb_config.bm_attributes = 0x80;
    dev_cfg.usb_config.bmax_power = 50;
    node_cfg.iface.class_code = 0x03;
    node_cfg.iface.bcd_hid = 0x0111;
    node_cfg.ep_in = (mimhid_usb_endpoint){0x81, 1, 64};
    waits.control_timeout_ms = 500;
    waits.reconnect_timeout_ms = 3000;
    kb_opts.usage_minimum = 0x04;
    kb_opts.usage_maximum = 0xDD;

    mimhid_device *dev = NULL;
    mimhid_spec *spec = NULL;
    mimhid_node *kb = NULL;
    if (mimhid_device_open("/dev/ttyUSB0", &dev_cfg, &dev) == MIMHID_OK &&
        mimhid_spec_create_keyboard(&kb_opts, &spec) == MIMHID_OK &&
        mimhid_node_open(dev, spec, &node_cfg, &waits, &kb) == MIMHID_OK &&
        mimhid_device_wait_ready(dev, 5000) == MIMHID_OK) {
        mimhid_keyboard_key(kb, 0x04, 1); /* Keyboard a */
        mimhid_node_submit(kb, 500);
        mimhid_keyboard_key(kb, 0x04, 0);
        mimhid_node_submit(kb, 500);
    }
    if (kb != NULL) mimhid_node_close(kb, &waits, 1000);
    if (spec != NULL) mimhid_spec_release(spec);
    if (dev != NULL) mimhid_device_close(dev, 500, 1000);
    return 0;
}
```

[QUICKSTART.md](docs/QUICKSTART.md) walks through wiring a board, setting serial
permissions, and running the watchable demos under
[examples/c/verify/](examples/c/verify).

## Profiles

| Profile | Factory | Input | Output / Feature |
|---|---|---|---|
| Keyboard (NKRO bitmap) | `mimhid_spec_create_keyboard` | 8 modifiers + `usage_minimum..usage_maximum` bitmap | Optional 5-LED Output report |
| Mouse (relative pointer) | `mimhid_spec_create_mouse` | `1..16` buttons, `X`/`Y`, optional Wheel and AC Pan | — |
| Toggle controls | `mimhid_spec_create_toggle` | Allow-list on Consumer (`0x0C`), Generic Desktop (`0x01`), Telephony (`0x0B`), or Camera (`0x90`) | — |
| Gamepad | `mimhid_spec_create_gamepad` | `1..32` buttons, caller-declared axes, optional hat or 4-button D-pad | — |
| Touchscreen (multi-touch) | `mimhid_spec_create_touchscreen` | `1..32` contact slots (`X`, `Y`, optional width/height/pressure) | `Contact Count Maximum` Feature report |
| Touchpad | `mimhid_spec_create_touchpad` | `1..32` contact slots + `1..3` buttons | `Contact Count Maximum` Feature report |
| Pen (direct screen) | `mimhid_spec_create_pen` | In-Range, Tip, Invert, Eraser, barrel switches, `X`/`Y`, optional pressure/tilt | — |
| Digitizer | `mimhid_spec_create_digitizer` | Pen state machine under `Digitizer` (`0x0D/0x01`) | — |
| Raw descriptor | `mimhid_spec_create_raw` / `mimhid_node_open_raw` | Any validated Input report (`mimhid_raw_submit` / `mimhid_report_send`) | Output and Feature events via `transport.event_sink` |

Any generated profile can also include a battery-strength Input report. Full
descriptor layouts and state-machine rules are in [PROFILES.md](docs/PROFILES.md).

## Moving between libaoahid and libmimhid

Both libraries share the same profile `Spec` factories, report state machines,
and error model (`s/aoahid/mimhid/` for profile and mutation calls). They differ
where the hardware differs: `libmimhid` has no `Context` handle, takes a serial
port name and full USB/endpoint descriptors (`mimhid_device_config`,
`mimhid_node_config`), uses `mimhid_commit_timeouts` on calls that re-enumerate
the board, and supports two-way HID traffic (Output reports, Feature responses,
bus connect/disconnect, and endpoint 0 control transfer routing). The full
comparison is in [API.md](docs/API.md#differences-from-libaoahid).

## Verification status

- **Hardware (RP2040):** verified on 2026-11-07 on a Raspberry Pi Pico with a Prolific PL2303 USB-UART adapter against a Linux 7.2 target (15 mouse interfaces at 7,812,500 baud with 0 link errors, 500-keystroke end-to-end latency run, endpoint 0 control routing, and the 7-interface all-profile verification run).
- **Release 0.2.0 itself is not hardware-verified:** protocol 0.2.0 and the initial-value rules changed after the 2026-11-07 hardware run ([CHANGELOG.md](CHANGELOG.md)).
- **Not tested on hardware:** RP2350, nRF52840, STM32F4, and ESP32-S3 firmware ports, raw descriptors, System Control / Camera / Telephony toggles, battery reports, and Windows or macOS hosts and targets ([TARGET_MATRIX.md](docs/TARGET_MATRIX.md)).
- **Automated CI:** unit, scripted-board (`tests/scripted`), and simulated-board (`tests/sim`, running the real C firmware core over a pseudo-terminal) suites run with ASan/UBSan, ThreadSanitizer, and an 86% line / 73% branch coverage gate on `src/`.
- **Details and commands:** [VERIFICATION.md](docs/VERIFICATION.md) has the feature-by-layer matrix and exact reproduction steps.

## Documentation

| Document | Contents |
|---|---|
| [QUICKSTART.md](docs/QUICKSTART.md) | Wiring a board, flashing firmware, permissions, and running the verification demos |
| [BUILDING.md](docs/BUILDING.md) | Host requirements, CMake options and presets, firmware builds, package consumption, bindings |
| [API.md](docs/API.md) | Complete C and C++ API reference: handles, structs, rules, every public function, result codes |
| [PROFILES.md](docs/PROFILES.md) | Descriptor and state-machine contract of all nine profiles and the battery report |
| [EXAMPLES.md](docs/EXAMPLES.md) | Walkthrough of the C, C++, Python, C#, and Rust example programs |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | System structure, threading, data paths, and design decisions |
| [INTERNALS.md](docs/INTERNALS.md) | Contributor guide: source map, handle internals, locking, teardown, design decisions |
| [PROTOCOL.md](docs/PROTOCOL.md) | Binary UART wire protocol (frames, CRC-16, commands, events, line-rate handshake) |
| [LIMITS.md](docs/LIMITS.md) | Every numeric limit with its USB, HID, chip, or firmware source |
| [LATENCY.md](docs/LATENCY.md) | Where time goes, measured RP2040 end-to-end latency, and how to measure |
| [PORTING.md](docs/PORTING.md) | Adding a new MCU port (the 12 HAL functions) or a new host OS serial backend |
| [TARGET_MATRIX.md](docs/TARGET_MATRIX.md) | Hardware verification status per MCU board, host OS, and HID profile |
| [VERIFICATION.md](docs/VERIFICATION.md) | Feature × test-layer verification matrix, coverage numbers, and reproduction commands |
| [RELEASES.md](docs/RELEASES.md) | The 18 release assets, archive layout, platform baselines, and component licenses |
| [CHANGELOG.md](CHANGELOG.md) | Release history and hardware verification notes |

## License

`libmimhid`, its headers, examples, language bindings, and portable firmware
core are MIT licensed ([LICENSE](LICENSE)). Prebuilt firmware images link
TinyUSB (MIT) and vendor HALs under BSD-3-Clause and Apache-2.0; see
[RELEASES.md](docs/RELEASES.md#licenses) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## AI disclosure

Most of the code, tests, and documentation in this repository were written by
an AI coding assistant. Architecture and design decisions, hardware
verification steps, debugging, and review of the AI's output were done by a
human. Hardware results are listed in
[TARGET_MATRIX.md](docs/TARGET_MATRIX.md); review the code before relying on
it for anything security- or safety-relevant.
