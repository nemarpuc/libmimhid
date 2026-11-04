# libmimhid

Physical USB HID emulation for a target device, driven from a Linux, macOS, or Windows PC. The PC sends reports over a high-speed serial link to a microcontroller board (Raspberry Pi Pico or Pico 2, with ports for other chips), which presents them to the target as a composite USB HID device. The target needs no driver, software, or special mode. You need a board, a 3.3 V
USB-to-UART bridge, and three wires ([QUICKSTART.md](docs/QUICKSTART.md)).

Sister library of [`libaoahid`](https://github.com/nemarpuc/libaoahid), which sends the same kind of input to an Android phone over USB (AOA 2.0), with no extra hardware needed. The C ABI and C++ header follow the same shape, so code moves between the two with few changes; see [Moving between libaoahid and libmimhid](#moving-between-libaoahid-and-libmimhid).

> [!NOTE]
> Version 2.1.0. Changes are listed in [CHANGELOG.md](CHANGELOG.md).

## Features

- **Profiles.** Keyboard, mouse, toggle (consumer, system, telephony, camera), gamepad, touchscreen, touchpad, pen, digitizer, and raw HID (`mimhid_node_open_raw`), with Battery Strength on the profiles that support it. Descriptors are byte-identical to `libaoahid`'s for equal options ([Profiles](#profiles)).
- **Configured from the host, without reflashing.** Every USB identity field (VID, PID, release number, language, device class, power, remote wakeup, and string descriptors) and per-interface parameter (`iface.number`, the `ep_in` / `ep_out` address, `poll_interval_ms` and `max_packet_size`, `bcd_hid`, country code, boot protocol).
- **Physical bus control.** `mimhid_device_connect()` and `mimhid_device_disconnect()` command D+=3.3V attachment or D+=0V stealth disconnection at will. `mimhid_device_commit(device, delay_ms, timeouts)` handles staged dynamic re-enumeration with a controlled disconnect window.
- **Events from the target.** Output reports (keyboard LEDs), Feature reads (answered with `mimhid_node_set_feature_response`), `SET_PROTOCOL`, `SET_IDLE`, mount, suspend, and resume reach your callback.
- **Control transfers of the target.** Every request on endpoint 0 (descriptors, `GET_REPORT`, `SET_REPORT`, vendor requests) can be reported, and chosen ones answered from the program with data, an acknowledgement or a STALL, through up to 8 rules of `mimhid_device_set_control_routing`. Off until asked; the board answers by itself as before ([API.md](docs/API.md#control-transfers-of-the-target)).
- **Low level.** Applications that need no profile helper can stay at the low level: `mimhid_discover`, `mimhid_device_open`, `mimhid_node_open_raw`, `mimhid_raw_submit` / `mimhid_report_send`, and `event_sink`. [`docs/API.md`](docs/API.md) lists the calls by group; every call is documented in [`include/mimhid.h`](include/mimhid.h).

## Design principles

- **Ultra-low latency.** Input calls do not allocate on the hot path; one report submission is a single contiguous serial write (`3.3 µs/byte` at 3,000,000 baud; the host and the board agree the fastest rate the serial adapter carries). On RP2040 and RP2350, UART DMA runs into a dedicated `.scratch_y` SRAM bank polled by Core 1, handing off reports via lock-free single-producer single-consumer (SPSC) queues to Core 0 (`TinyUSB`) for 1000 Hz (`bInterval = 1 ms`) USB Interrupt IN delivery ([LATENCY.md](docs/LATENCY.md)).
- **Thin firmware.** The host PC builds every HID report descriptor and encodes every report. The microcontroller acts strictly as a stateless-per-report I/O bridge: it knows nothing about keyboards, mice, or touchscreens, holds only the descriptors uploaded by the host, and forwards raw report payloads directly to USB endpoints. Hardware-specific code is isolated behind a small HAL (`firmware/common/hal.h`).
- **The libaoahid model.** The same objects and call shapes: a Spec (`mimhid_spec_create_*`), Nodes (`mimhid_node_open` / `mimhid_node_open_raw`, with an explicit interface number), profile calls that change state (`mimhid_keyboard_key`, `mimhid_mouse_move`, ...), and `mimhid_node_submit` that sends. `mimhid_device_batch_begin` / `mimhid_device_batch_end` combine several opens into one re-enumeration. The life cycle is in [API.md](docs/API.md#lifecycle).
- **Nothing guessed, nothing shaped.** Every call that can wait takes its timeout as an argument, and zero is refused with `MIMHID_ERR_UNSET_FIELD` ([API.md](docs/API.md#timeouts)). Each options structure has an `_init` function that fills in values that work, and a required field left zero is refused rather than filled in by the library; the vendor and product ID start as the pid.codes test pair (0x1209, 0x0001), which a device you hand out must replace with IDs of its own, and zero is a value like any other for them (USB 2.0 9.6.1). The library does not smooth, pace, or randomize input.

## Status

- Official support: Raspberry Pi Pico (RP2040) and Raspberry Pi Pico 2 (RP2350). Verified on hardware: the Pico (RP2040), against a Linux target. The Pico 2 is verified in CI and by the end-to-end tests.
- Verified on the build host and in CI: the host library builds and passes its tests on Linux, macOS, and Windows; an end-to-end test runs the host library against the real firmware core through a pseudo-terminal (Linux and macOS); the firmware core is tested against a fake HAL; the firmware builds for RP2040, RP2350, nRF52840, STM32F4, and ESP32-S3.
- See [TARGET_MATRIX.md](docs/TARGET_MATRIX.md) for platform support details. Wiring, pins, and flashing are in [QUICKSTART.md](docs/QUICKSTART.md).

## Example

With a flashed board connected (wiring and flashing are in [QUICKSTART.md](docs/QUICKSTART.md), the build in [Building](#building)), this types the letter `a` on the target:

```c
#include <mimhid.h>

mimhid_device_config config;
mimhid_device_config_init(&config);   /* working values; see docs/API.md */
/* The init above already holds the pid.codes test IDs, for testing in-house only; a device you
 * hand out needs IDs of its own. */
config.usb_device.vendor_id = 0x1209;
config.usb_device.product_id = 0x0001;
config.transport.baud_rate = 3000000; /* the fastest rate mimhid_discover() found for the port */

mimhid_device* device = NULL;
mimhid_device_open("/dev/ttyACM0", &config, &device);

mimhid_keyboard_options keyboard;
mimhid_keyboard_options_init(&keyboard);
mimhid_spec* spec = NULL;
mimhid_spec_create_keyboard(&keyboard, &spec);

mimhid_node_config node;
mimhid_node_config_init(&node, 0);    /* interface 0, IN endpoint 0x81 */
mimhid_commit_timeouts timeouts;
mimhid_commit_timeouts_init(&timeouts);
mimhid_node* keys = NULL;
mimhid_node_open(device, spec, &node, &timeouts, &keys); /* the board attaches to the target */
mimhid_spec_release(spec);                               /* the Node keeps its own reference */
mimhid_device_wait_ready(device, 5000);

mimhid_keyboard_key(keys, 0x04, 1);   /* Keyboard a */
mimhid_node_submit(keys, MIMHID_SEND_TIMEOUT_MS);
mimhid_keyboard_key(keys, 0x04, 0);
mimhid_node_submit(keys, MIMHID_SEND_TIMEOUT_MS);

mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS); /* closes keys too */
```

Every call returns a `mimhid_result`; check it, and read `mimhid_last_error()` for the details. Complete programs: [`examples/`](examples/) and [EXAMPLES.md](docs/EXAMPLES.md). For a single profile in isolation, [`examples/c/profiles/`](examples/c/profiles) has one self-contained program per HID profile kind and one for the battery report, all ten of which run without a board. [`examples/c/verify/`](examples/c/verify) holds programs built to be watched on a real target (typing, dragging, moving the mouse); [QUICKSTART.md](docs/QUICKSTART.md) walks through them.

## Building

Prebuilt libraries and firmware images are on the [Releases](https://github.com/nemarpuc/libmimhid/releases) page ([Releases](#releases) below). To build them yourself:

Requirements: CMake 3.20+, a C11/C++20 compiler (GCC, Clang, MSVC 2019+, MinGW-w64). Firmware also needs `arm-none-eabi-gcc` and the SDK of the port.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DMIMHID_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This produces `libmimhid.so`, `libmimhid.dylib`, or `mimhid.dll` with `mimhid.lib`. Options: `MIMHID_BUILD_TESTS` and `MIMHID_BUILD_EXAMPLES` (both off by default), `MIMHID_WERROR` (on by default), `MIMHID_SANITIZE` (AddressSanitizer and UBSan), `MIMHID_TSAN` (ThreadSanitizer, Linux, GCC 12+ or Clang; not together with `MIMHID_SANITIZE` or `MIMHID_BUILD_FUZZ`), `MIMHID_BUILD_FUZZ` (the libFuzzer target, Clang only; it sets its own sanitizers). The CMake presets `dev`, `release` and `tsan` set these for you. To also run the end-to-end tests, pass `-DMIMHID_TINYUSB_PATH=<TinyUSB 0.18.0 checkout>` (POSIX only).

### Firmware

| Target | Prerequisites | Command |
|---|---|---|
| RP2040 (Raspberry Pi Pico) | [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0 | `export PICO_SDK_PATH=<pico-sdk>`<br>`cmake -B build_fw -S firmware && cmake --build build_fw` |
| RP2350 (Raspberry Pi Pico 2) | Pico SDK 2.2.0 | `cmake -B build_fw -S firmware -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 && cmake --build build_fw` |
| nRF52840 | [TinyUSB](https://github.com/hathach/tinyusb) 0.18.0 and `python3 <tinyusb>/tools/get_deps.py nrf` | `cmake -B build_fw -S firmware/ports/nrf52840 -G Ninja -DTINYUSB_PATH=<tinyusb> && cmake --build build_fw` |
| STM32F4 | TinyUSB 0.18.0 and `get_deps.py stm32f4` | `cmake -B build_fw -S firmware/ports/stm32f4 -G Ninja -DTINYUSB_PATH=<tinyusb> && cmake --build build_fw` |
| ESP32-S3 | ESP-IDF 5.4 and TinyUSB 0.18.0 | `export TINYUSB_PATH=<tinyusb>`<br>`idf.py -C firmware/ports/esp32s3 set-target esp32s3 build` |

Host test of the firmware core: `cmake -B build_fwtest -S firmware/tests -DTINYUSB_PATH=<tinyusb> && cmake --build build_fwtest && ctest --test-dir build_fwtest`. Flashing is in the [Quickstart](docs/QUICKSTART.md#2-build-and-flash-the-firmware).

## Profiles

Each profile derives the report mechanics from its immutable Spec; the product and target values are what its options say (the `*_init` functions fill in one working set). The descriptors and state machines are those of `libaoahid`, byte for byte for equal options ([PROFILES.md](docs/PROFILES.md)).

| Profile | Derived by the library | You specify |
|---|---|---|
| Keyboard | Modifier routing and a full N-Key Rollover bitmap: every declared key has its own bit, so any number of simultaneous keys is reported. | Key Usage range, Report ID, layout, locale, IME behavior. |
| Mouse | Signed 64-bit pending totals for X, Y, Wheel, and AC Pan, split into in-range report fragments; a fragment is consumed only when its report has been written. | Axis ranges and widths, button count, Wheel/Pan presence, Report ID. |
| Consumer, System, Camera, Telephony | One allow-listed field per press; press/release/tap produce the `1` then `0` sequence each HID control type expects. | Each allowed Usage and its semantics, Report ID. |
| Gamepad | D-pad directions become the eight Hat values plus Null (`15`); adjacent pairs become diagonals, opposite pairs are rejected. Raw four-bit D-pad and no-D-pad modes are also available. | Axes (role, range, width, neutral), buttons, D-pad form, Report ID. |
| Touchscreen / touchpad | Contact Count, multi-report frames, Tip=0 release records with stable Contact IDs, Scan Time, packet sequencing. | Coordinate ranges, maximum contacts, contacts per report, optional fields. |
| Pen, Digitizer | Tip/pressure consistency, out-of-range normalization, automatic leave-and-re-enter when switching pen/eraser. | Coordinate and optional-field ranges, hover/eraser/barrel options. |
| Battery report | A second Input report inside any profile, sent only when the level changed and no input report is waiting. | Range, width, both Report IDs. |
| Raw Input | Only length, Report ID, and padding are checked against your descriptor. | Descriptor and report bytes. |

A state change is kept until its report is written; a conflicting change while a report is in flight returns `MIMHID_ERR_BUSY` instead of being merged ([API.md](docs/API.md#input)).

## Language bindings

The C ABI of `mimhid.h` can be called from Python, C# and Rust through declarations in [`bindings/`](bindings/):

| Language | Path | Package | Layout check |
|---|---|---|---|
| Python 3.9+ | `bindings/python` (`ctypes`) | wheel `mimhid` | `tests/test_layout.py` |
| C# (.NET 8) | `bindings/csharp/MimHid` (P/Invoke) | NuGet `MimHid` | `MimHid.Tests` |
| Rust 1.89+ | `bindings/rust/mimhid-sys` (FFI), `bindings/rust/mimhid` (owning wrappers) | crates `mimhid-sys`, `mimhid` | `tests/layout.rs` |

They hold declarations only: no native library, and no default for any field. The caller passes the path of the matching shared library (Python `mimhid.load(path)`), or makes it findable under the name `mimhid` (C#, Rust). The inline `*_init` functions of the header are C code, not part of the ABI, so a binding fills `struct_size` and every field itself. The `mimhid` crate adds typed ownership on top of the FFI declarations: a Spec that releases itself, and one node type per profile with `bool` instead of `0/1`. Each language has a short `enumerate` example (`examples/python`, `examples/csharp`, `bindings/rust/mimhid-sys/examples`); the full life cycle is shown in C only.

The declarations and the C layout oracle are generated from `include/mimhid.h` by `tools/bindings/generate.py` (needs Clang), and each layout test compares its language's sizes, offsets and constants with what the C compiler produced; see [INTERNALS.md](docs/INTERNALS.md#language-bindings). Wheel and NuGet packages are built and checked by the release workflow; they reach PyPI and NuGet only when the repository variable `MIMHID_PUBLISH_BINDINGS` is `true`.

## Moving between libaoahid and libmimhid

| | `libaoahid` | `libmimhid` |
|---|---|---|
| Header | `aoahid.h` / `aoahid.hpp` | `mimhid.h` / `mimhid.hpp` |
| Link | USB to an Android device | UART to an MCU board, which presents USB to any target |
| Open | `aoahid_context_*`, `aoahid_device_open` | `mimhid_device_open(port_path, ...)` (no context) |
| Spec, options, node calls | `aoahid_spec_create_*`, `aoahid_node_*`, `aoahid_keyboard_key`, ... | same names with the `mimhid_` prefix |
| Waits | every call that can wait takes its timeout as an argument (`control_timeout_ms`, `send_timeout_ms`, `drain_timeout_ms`; libaoahid 2.0 and later) | the same rule and names; `mimhid_commit_timeouts` adds the wait for the board's re-enumeration |
| Node id, error detail | `aoahid_node_id`; `aoahid_error_detail.node_id`, `.transport_status` | `mimhid_node_id`; `mimhid_error_detail.node_id`, `.transport_status` |
| Opening a node | registers an accessory HID device | `mimhid_node_open` / `mimhid_node_open_raw` (one re-enumeration each, or one per `mimhid_device_batch_begin` / `mimhid_device_batch_end`) |
| Interface setup | implicit / accessory defined | `mimhid_node_config`: the interface number and endpoints are explicit (`mimhid_node_config_init(&config, n)`) |
| USB identity | fixed Google accessory ID | set in `mimhid_device_config` |
| Physical bus control | handled by Android OS | `mimhid_device_connect()`, `mimhid_device_disconnect()`, `mimhid_device_commit()` |
| Output of the target | AOA has none | `event_sink` |
| Control transfers of the target | AOA has none | `mimhid_device_set_control_routing`, `mimhid_device_control_reply` |

## Releases

A pushed `v*` tag builds and checks the release, then publishes a GitHub Release. The release matrix produces:

- Linux x86_64 and AArch64 shared objects and static archives;
- Windows x86_64 and ARM64 DLLs, import libraries, and static archives;
- macOS arm64 and x86_64 dynamic libraries and static archives;
- headers, CMake and pkg-config metadata, notices, checksums, SPDX SBOMs, and the firmware images.

For version `X.Y.Z`, the GitHub Release contains exactly 41 uploaded assets, in the same layout as `libaoahid` plus the firmware:

- twelve archives: `libmimhid-X.Y.Z-linux-{x86_64,aarch64}-ubuntu22.04-{shared,static}.tar.gz`, `libmimhid-X.Y.Z-macos-{arm64,x86_64}-{shared,static}.tar.gz`, and `libmimhid-X.Y.Z-windows-{x86_64,arm64}-{shared,static}.zip`;
- twelve matching `.spdx.json` SBOM sidecars;
- six runtime bundles: the same six target names with `-runtime.tar.gz` on Linux and macOS and `-runtime.zip` on Windows;
- the tagged tree as `libmimhid-X.Y.Z-source.tar.gz`;
- `release-manifest.json` and `SHA256SUMS`;
- eight firmware files: `mimhid_firmware-rp2040-X.Y.Z.uf2`, `mimhid_firmware-rp2350-X.Y.Z.uf2`, `mimhid_firmware-nrf52840-X.Y.Z.{hex,bin}`, `mimhid_firmware-stm32f4-X.Y.Z.{hex,bin}`, `mimhid_firmware-esp32s3-X.Y.Z.bin`, and `mimhid_firmware-THIRD_PARTY_NOTICES-X.Y.Z.txt`.

The complete `*-shared` or `*-static` archive is what you build against: it carries the headers, the library, CMake package files, pkg-config files, and `examples/` with the commands to build them (`share/doc/libmimhid/examples/README.md`). The `*-runtime` bundle is for deploying next to an application that is already built: it holds only the shared library and the license and notice documents (`LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`). Consume an archive with `find_package(mimhid COMPONENTS shared)` (or `static`) or with pkg-config (`mimhid`, `mimhid-static`).

Platform baselines: Linux archives target glibc 2.35 (Ubuntu 22.04); macOS archives target macOS 11 and are signed ad hoc, not notarized; Windows archives use the dynamic MSVC runtime. The AArch64 Linux and ARM64 Windows libraries are cross-built and are not run in CI.

The bindings are in the source archive (`bindings/`); the binary archives carry no binding. The Python wheel and the NuGet package are built and checked by the release workflow and kept as workflow artifacts, not as release assets; they go to PyPI and NuGet only when the repository variable `MIMHID_PUBLISH_BINDINGS` is `true`.

## Documentation

| Document | Contents |
|---|---|
| [QUICKSTART.md](docs/QUICKSTART.md) | Wiring, flashing, serial-port access, first run, programs to watch, troubleshooting |
| [API.md](docs/API.md) | The model, structures and initial values, life cycle, timeouts, events, control transfers, result codes |
| [EXAMPLES.md](docs/EXAMPLES.md) | Walkthrough of the example programs and usage patterns |
| [PROFILES.md](docs/PROFILES.md) | The profiles and what they assume of the target |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Host, interface configuration, firmware, re-enumeration, tests |
| [PROTOCOL.md](docs/PROTOCOL.md) | The serial wire protocol: frames, commands, line rate, events, control transfers |
| [LIMITS.md](docs/LIMITS.md) | Protocol, board, host, and Linux HID parser limits |
| [LATENCY.md](docs/LATENCY.md) | Where the time goes, what is arithmetic and what is measured |
| [PORTING.md](docs/PORTING.md) | Firmware layout, the HAL, endpoint counts, other MCUs, the host serial backend |
| [TARGET_MATRIX.md](docs/TARGET_MATRIX.md) | What has been verified, on which board, host, and target |
| [INTERNALS.md](docs/INTERNALS.md) | Ownership, locks, the report and commit paths, and how the bindings are generated, for people changing the library |

## License

`libmimhid` itself is **MIT** licensed. The full text is in [LICENSE](LICENSE). The host library is built from first-party source only and has no third-party dependency (on Linux the C++ runtime is linked statically under the GCC Runtime Library Exception 3.1). The prebuilt firmware images are different: they link third-party SDK and USB stack code under permissive licenses, and their notices are reproduced in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and in the `mimhid_firmware-THIRD_PARTY_NOTICES-X.Y.Z.txt` release asset.

| What you receive | License | Where the text is |
|---|---|---|
| Host library, headers, bindings, examples, firmware source | MIT | `LICENSE` |
| Every firmware image | TinyUSB 0.18.0 (MIT), Newlib C runtime | `THIRD_PARTY_NOTICES.md` section 3 |
| `rp2040`, `rp2350` images | Raspberry Pi Pico SDK 2.2.0 (BSD-3-Clause), Arm CMSIS Core (Apache-2.0) | sections 3.2, 3.6 |
| `nrf52840` images | Nordic `nrfx` (BSD-3-Clause), Arm CMSIS 5 (Apache-2.0) | sections 3.3, 3.6 |
| `stm32f4` images | ST `stm32f4xx_hal_driver` (BSD-3-Clause), `cmsis_device_f4` (Apache-2.0 / BSD-3-Clause), Arm CMSIS 5 (Apache-2.0) | sections 3.4, 3.6 |
| `esp32s3` image | Espressif ESP-IDF 5.4.2 (Apache-2.0), Amazon FreeRTOS Kernel (MIT) | section 3.5 |

[NOTICE](NOTICE) summarizes the same; `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md` and `CHANGELOG.md` are in every host archive under `share/doc/libmimhid/`.

## AI disclosure

Most of the code, tests, and documentation in this repository were written by
an AI coding assistant. Architecture and design decisions, hardware
verification steps, debugging, and review of the AI's output were done by a
human. Hardware results are listed in
[TARGET_MATRIX.md](docs/TARGET_MATRIX.md); review the code before relying on
it for anything security- or safety-relevant.
