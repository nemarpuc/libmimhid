# Verification

What is tested in each verification layer, what has and has not been verified on
real hardware, the coverage floor, and the exact commands to reproduce every
check.

Detailed test organization lives in [tests/README.md](../tests/README.md); the
step-by-step physical board checklist lives in
[tests/hw/README.md](../tests/hw/README.md).

## Four verification layers

1. **`unit` ([tests/unit/](../tests/unit), [tests/abi/](../tests/abi), [tests/hidtools/](../tests/hidtools), [tests/cmake/](../tests/cmake), [firmware/tests/](../firmware/tests)).**
   Pure in-process tests of the HID item writer, descriptor builder, raw
   descriptor validator, frame codec, profile state machines, argument and
   option rejection paths, golden descriptors, C ABI layout oracle, CMake
   package contracts, and the portable firmware core (`test_core_{15,7,4,3,hs}`).
   Needs no serial port and no hardware.
2. **`scripted` ([tests/scripted/](../tests/scripted)).**
   Runs the host library (`SerialPort`, `Link`, `Handshaker`, Linux port
   discovery against a synthetic sysfs/`/dev` tree, and the C API) over a POSIX
   pseudo-terminal against a test-controlled state machine
   ([tests/scripted/scripted_board.cpp](../tests/scripted/scripted_board.cpp)).
   Exercises failure modes a healthy firmware core never volunteers: CRC corruption,
   truncated frames, silence/timeouts, `NAK` codes on every command, rejected
   baud rates, write backpressure, and peer hang-ups.
3. **`sim` ([tests/sim/](../tests/sim)).**
   Runs the host library over a POSIX pseudo-terminal against the real C
   firmware core (`core.c`, `control.c`, `ep0.c`, `usb_descriptors.c`) and a
   TinyUSB device-controller stub ([tests/sim/fake_board.c](../tests/sim/fake_board.c))
   in a background thread, compiled four ways: `15`, `7`, and `3` Full-Speed
   interfaces (`mimhid.sim-15`, `mimhid.sim-7`, `mimhid.sim-3`) and `7`
   High-Speed interfaces (`mimhid.sim-hs`).
4. **`hw` ([tests/hw/README.md](../tests/hw/README.md), [examples/c/verify/](../examples/c/verify)).**
   Manual runs with a physical microcontroller board wired to a USB-UART adapter
   and plugged into a real target host. Only this layer exercises the real silicon
   USB controller, crystal/PLL baud dividers, DMA ring wrap-around, physical D+
   pull-up transitions, and the target operating system's USB and HID stacks.

## Feature verification matrix

| Feature / area | `unit` | `scripted` | `sim` (pty + firmware core) | `hw` (real board) |
|---|---|---|---|---|
| HID item writer & descriptor builder (`src/hid/`) | `item_writer`, `descriptor_builder`, `mimhid.golden-descriptors` | — | Used by every profile open | RP2040 -> Linux 7.2 (`2026-11-07`) |
| Raw descriptor validator (`src/hid/validator.cpp`) | `validator_parser_state`, `validator_arithmetic`, `validator_encoding`, `spec_rejections`, `mimhid_fuzz_descriptor` | `api_raw_node` | `test_sim_reports` (raw Input, Output, Feature) | Not hardware-verified |
| Profile specs & option validation (`src/profiles/spec.cpp`) | `profiles_*`, `spec_rejections`, `mimhid.golden-descriptors`, `hid-tools` (`hid_decode.py`) | `api_open` | `test_sim_open`, `test_sim_reports` | RP2040 -> Linux 7.2 (7 profiles in `verify_all`) |
| Profile state machines & edge guards (`src/profiles/state.cpp`) | `profiles_common`, `profiles_keyboard`, `profiles_mouse`, `profiles_usage_controls`, `profiles_gamepad`, `profiles_touch`, `profiles_pen`, `profiles_raw`, `profiles_battery`, `input_rejections` | `api_reports` | `test_sim_reports` | RP2040 -> Linux 7.2 (keyboard, mouse, touchscreen, touchpad, pen, gamepad, Consumer toggle) |
| Null, unset, and out-of-range API arguments (`src/api/`) | `api_arguments`, `error_detail`, `spec_rejections`, `input_rejections` | `api_open`, `api_commands` | `test_sim_arguments` | — |
| C ABI struct layout, exports & C++20 wrapper (`include/`) | `mimhid.abi-layout-oracle`, `mimhid.c-abi`, `mimhid.cpp-wrapper`, `symbol_lint.py` | — | — | — |
| Frame codec & CRC-16/CCITT-FALSE (`src/transport/frame.cpp`) | `frame`, `firmware_core_*` | `link`, `handshaker` | Every `mimhid.sim-*` run | RP2040 + PL2303 at 7,812,500 baud (`2026-11-07`) |
| Baud handshake, probe burst & UART BREAK recovery (`src/transport/handshaker.cpp`) | `firmware_core_*` | `handshaker`, `api_open` | `test_sim_line_rate` (rate cap, noise injection, BREAK recovery) | RP2040 + PL2303 up to 7,812,500 baud (`2026-11-07`) |
| Serial port I/O & candidate port discovery (`src/transport/`) | — | `serial_port`, `ports_linux` (synthetic sysfs/`/dev`) | Pseudo-terminal I/O in `mimhid.sim-*` | Linux host (`ttyUSB0` PL2303, `2026-11-07`) |
| Command ACK/NAK, retries & sequence deduplication (`src/transport/link.cpp`, `core.c`) | `firmware_core_*` | `link`, `api_commands` | `test_sim_open`, `test_sim_reports` | RP2040 -> Linux 7.2 (`2026-11-07`) |
| Dynamic USB descriptors, node packing & `COMMIT` batching | `firmware_core_{15,7,4,3,hs}`, `itfs_*` | `api_open`, `api_commands` | `test_sim_open` (`15`, `7`, `3` interfaces), `test_sim_high_speed` | RP2040 at 15 mouse interfaces and 7 mixed interfaces (`2026-11-07`) |
| Physical bus control (`disconnect`, `connect`, 0V wait before first commit) | `firmware_core_*` | `api_commands` | `test_sim_open`, `test_sim_events` | RP2040 -> Linux 7.2 (`2026-11-07`) |
| Target events (`MOUNTED`, `UNMOUNTED`, `SUSPEND`, `RESUME`, `OUTPUT_REPORT`, `FEATURE_REPORT`, `SET_PROTOCOL`, `SET_IDLE`, `REPORT_LOST`) | `firmware_core_*` | `api_events`, `api_raw_node` | `test_sim_events`, `test_sim_reports` | `MOUNTED`, keyboard LED `OUTPUT_REPORT`, `GET_REPORT(Feature)` on RP2040 -> Linux 7.2 (`2026-11-07`) |
| Endpoint 0 control transfer routing & replies (`control.c`, `ep0.c`) | `firmware_core_*` | `api_commands`, `api_events` | `test_sim_control` (hold, answer, stall, drop, timeout, fixed requests, 8/16/32-byte EP0 packet splitting) | RP2040 -> Linux 7.2 (`examples/c/control_transfers.c`, `2026-11-07`) |
| Language bindings layout (Python, C#, Rust) | `generate.py --check`, `test_layout.py`, `MimHid.Tests`, `cargo test` | — | — | — |
| CMake package & release archives | `mimhid.package-multiarch-layout`, `mimhid.package-component-contract`, `tests/release/test_release_tools.py` | — | — | — |

## Hardware verification status

- **RP2040 verified on 2026-11-07.** A Raspberry Pi Pico (`RP2040`) wired to a
  Prolific PL2303 USB-UART adapter (`GP0` TX, `GP1` RX, `GND`) was tested from
  an Arch Linux (`6.18.9-arch1-2`) host against a Linux `7.2` target:
  - Release `0.1.0` firmware was verified with 15 simultaneous mouse interfaces
    at `7,812,500` baud with `0` framing, overrun, ring, CRC, or timeout errors
    ([TARGET_MATRIX.md](TARGET_MATRIX.md)), a 500-keystroke end-to-end `evdev`
    latency run ([LATENCY.md](LATENCY.md#measured-once)), and endpoint 0 vendor
    control transfer interception ([examples/c/control_transfers.c](../examples/c/control_transfers.c)).
  - An earlier firmware revision on the same RP2040 + PL2303 setup verified the
    7-interface `verify_all` demo (keyboard with CAPS LOCK LED echo, mouse,
    10-finger touchscreen with `Contact Count Maximum` Feature read, touchpad,
    pen with hover/pressure/tilt/barrel/eraser, gamepad, and Consumer Control
    media keys).
- **Everything else is not hardware-verified.** Release `0.2.0` itself, the
  **RP2350**, **nRF52840**, **STM32F4**, and **ESP32-S3** firmware ports, raw
  descriptors, System Control / Camera / Telephony toggles, battery reports,
  other USB-UART adapters (FT232H, CP2102N, CH343), Windows and macOS hosts, and
  Windows, macOS, Android, iOS, and console targets have **not** been tested on
  physical hardware ([CHANGELOG.md](../CHANGELOG.md),
  [TARGET_MATRIX.md](TARGET_MATRIX.md)).

## Coverage numbers

When `MIMHID_COVERAGE=ON` is enabled, every target is compiled with
`--coverage -O0 -fprofile-update=atomic` and measured over `src/` by `gcovr`
(with `--exclude-throw-branches --exclude-unreachable-branches` and the C ABI
exception-containment macros excluded via
`--exclude-lines-by-pattern '.*MIMHID_C_(RESULT|VALUE|VOID)_CATCH.*'`; see
[tests/README.md](../tests/README.md#coverage)):

- **Current coverage on `src/`:** **88.3% line coverage** and **75.1% branch
  coverage** across `mimhid_tests`, `mimhid_scripted_tests`,
  `mimhid_export_descriptors`, `mimhid_c_abi_test`, `mimhid_cpp_wrapper_test`,
  and `mimhid_sim_{15,7,3,hs}`.
- **Enforced CI gate:** `gcovr` fails the `Coverage` CI job below **86% line
  coverage** (`--fail-under-line 86`) or **73% branch coverage**
  (`--fail-under-branch 73`).

## Reproducing the checks

### Unit, scripted, and simulated-board tests

```sh
cmake --preset dev -DMIMHID_TINYUSB_PATH=/path/to/tinyusb
cmake --build --preset dev
ctest --preset dev
```

On Windows or without a TinyUSB checkout, omit `-DMIMHID_TINYUSB_PATH=...` to
run the unit, ABI, golden-descriptor, and CMake package tests (plus
`mimhid.scripted` on Linux and macOS).

### AddressSanitizer and UndefinedBehaviorSanitizer

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMIMHID_BUILD_TESTS=ON \
  -DMIMHID_BUILD_EXAMPLES=ON \
  -DMIMHID_SANITIZE=ON \
  -DMIMHID_TINYUSB_PATH=/path/to/tinyusb
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

### ThreadSanitizer (Linux, Clang or GCC 12+)

```sh
cmake --preset tsan -DMIMHID_TINYUSB_PATH=/path/to/tinyusb
cmake --build --preset tsan
ctest --preset tsan
```

### Line and branch coverage (`gcovr`)

```sh
cmake --preset coverage -DMIMHID_TINYUSB_PATH=/path/to/tinyusb
cmake --build --preset coverage
ctest --preset coverage
gcovr --root . --object-directory build/coverage --filter 'src/' \
  --exclude-throw-branches --exclude-unreachable-branches \
  --exclude-lines-by-pattern '.*MIMHID_C_(RESULT|VALUE|VOID)_CATCH.*' \
  --fail-under-line 86 --fail-under-branch 73 \
  --txt-summary
```

### Release, provenance, contract, and symbol checks

```sh
python3 -m unittest discover -s tests/release -p "test_*.py"
python3 tools/check_input_provenance.py
python3 tools/check_public_contracts.py
python3 tools/check_license_headers.py
python3 tools/symbol-lint/symbol_lint.py --header include/mimhid.h
python3 tools/bindings/generate.py --check
```
