# Verification

What is tested in each verification layer, what has and has not run on real
hardware, the coverage floor, and the commands to reproduce every check.

Detailed test organization lives in [tests/README.md](../tests/README.md#tests); the
step-by-step physical board checklist lives in
[tests/hw/README.md](../tests/hw/README.md#hardware-tests).

## Terms

The two sister libraries use some of the same words for different things:

| Term | In `libmimhid` | In `libaoahid` ([its tests/README.md](https://github.com/nemarpuc/libaoahid/blob/main/tests/README.md)) |
|---|---|---|
| `unit` | In-process tests with no serial port and no board (`mimhid.unit` and the other suites of layer 1 below) | "nothing but memory": no USB transport at all |
| `scripted` | The host library over a pseudo-terminal against a peer the test drives frame by frame (`tests/scripted/scripted_board.cpp`) | — (no such layer) |
| `sim` | The host library over a pseudo-terminal against the real firmware core (`firmware/common`) built for the build host (`tests/sim/fake_board.c`) | The library against "the fake libusb in `fake_libusb/`" |
| `hw` | A physical board, a USB-UART adapter, and a target computer ([tests/hw/README.md](../tests/hw/README.md#hardware-tests)) | "a real Android phone" |
| hardware-verified | A release whose library and firmware ran on a physical board as `tests/hw/README.md` describes | A release run against a real Android phone |

## Four verification layers

1. **`unit` ([tests/unit/](../tests/unit), [tests/abi/](../tests/abi), [tests/hidtools/](../tests/hidtools), [tests/cmake/](../tests/cmake), [firmware/tests/](../firmware/tests)).**
   Pure in-process tests of the HID item writer, descriptor builder, raw
   descriptor validator, frame codec, profile state machines, argument and
   option rejection paths, golden descriptors, C ABI layout oracle, CMake
   package contracts, and the portable firmware core (`firmware_core_15`, `firmware_core_7`, `firmware_core_4`, `firmware_core_3`, `firmware_core_hs`).
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
   in a background thread, compiled four ways: with 15, 7, and 3 full-speed
   endpoint numbers (`mimhid.sim-15`, `mimhid.sim-7`, `mimhid.sim-3`) and `7`
   high-speed ones (`mimhid.sim-hs`). The fake target reads descriptors,
   reports and Feature answers with control transfers, since the board has no
   HID driver.
4. **`hw` ([tests/hw/README.md](../tests/hw/README.md#hardware-tests), [examples/c/verify/](../examples/c/verify)).**
   Manual runs with a physical microcontroller board wired to a USB-UART adapter
   and plugged into a real target host. Only this layer exercises the real silicon
   USB controller, crystal/PLL baud dividers, DMA ring wrap-around, physical D+
   pull-up transitions, and the target operating system's USB and HID stacks.

## Feature verification matrix

| Feature / area | `unit` | `scripted` | `sim` (pty + firmware core) | `hw` (real board) |
|---|---|---|---|---|
| HID item writer & descriptor builder (`src/hid/`) | `item_writer`, `descriptor_builder`, `mimhid.golden-descriptors` | — | Used by every profile open | — |
| Raw descriptor validator (`src/hid/validator.cpp`) | `validator_parser_state`, `validator_arithmetic`, `validator_encoding`, `spec_rejections`, `mimhid_fuzz_descriptor` | `api_raw_node` | `test_sim_reports` (raw Input, Output, Feature) | — |
| Profile specs & option validation (`src/profiles/spec.cpp`) | `profiles_*`, `spec_rejections`, `mimhid.golden-descriptors`, the `hid-tools` CI job (the official hid-tools parser) | `api_open` | `test_sim_open`, `test_sim_reports` | — |
| Profile state machines & edge guards (`src/profiles/state.cpp`) | `profiles_common`, `profiles_keyboard`, `profiles_mouse`, `profiles_usage_controls`, `profiles_gamepad`, `profiles_touch`, `profiles_pen`, `profiles_raw`, `profiles_battery`, `input_rejections` | `api_reports` | `test_sim_reports` | — |
| Null, unset, and out-of-range API arguments (`src/api/`) | `api_arguments`, `error_detail`, `spec_rejections`, `input_rejections` | `api_open`, `api_commands` | `test_sim_arguments` | — |
| C ABI struct layout, exports & C++20 wrapper (`include/`) | `mimhid.abi-layout-oracle`, `mimhid.c-abi`, `mimhid.cpp-wrapper`, `symbol_lint.py` | — | — | — |
| Frame codec & CRC-16/CCITT-FALSE (`src/transport/frame.cpp`) | `frame`, `firmware_core_*` | `link`, `handshaker` | Every `mimhid.sim-*` run | — |
| Baud handshake, probe burst & UART BREAK recovery (`src/transport/handshaker.cpp`) | `firmware_core_*` | `handshaker`, `api_open` | `test_sim_line_rate` (rate cap, noise injection, BREAK recovery) | — |
| Serial port I/O & candidate port discovery (`src/transport/`) | — | `serial_port`, `ports_linux` (synthetic sysfs/`/dev`) | Pseudo-terminal I/O in `mimhid.sim-*` | — |
| Command ACK/NAK, retries & sequence deduplication (`src/transport/link.cpp`, `core.c`) | `firmware_core_*` | `link`, `api_commands` | `test_sim_open`, `test_sim_reports` | — |
| USB descriptors composed by the host (`src/usb/descriptor_composer.cpp`, byte for byte against the descriptors the firmware used to build), the firmware's `COMMIT` walk, interface numbers without a gap & `COMMIT` batching | `descriptor_composer`, `firmware_core_{15,7,6,3,hs}`, `probe_*` | `api_open`, `api_commands`, `api_raw_node` | `test_sim_open` (`15`, `7`, `3` endpoint numbers), `test_sim_reports` (interface numbers), `test_sim_high_speed` | — |
| Physical bus control (`disconnect`, `connect`, 0V wait before first commit) | `firmware_core_*` | `api_commands` | `test_sim_open`, `test_sim_events` | — |
| Target events (`MOUNTED`, `UNMOUNTED`, `SUSPEND`, `RESUME`, `TRANSFER_LOST`, and the events the library makes from accepted control requests and OUT endpoint frames: `OUTPUT_REPORT`, `FEATURE_REPORT`, `SET_PROTOCOL`, `SET_IDLE`) | `firmware_core_*` | `api_events`, `api_raw_node` | `test_sim_events`, `test_sim_reports` | — |
| Endpoint 0 control rules & replies (`control.c`, `ep0.c`: `ANSWER`, `ANSWER_LAST_SENT`, `ACCEPT`, `ASK`, `STALL`, `RULES_APPLY`, `RULE_DATA`, HID behaviour of profile Nodes from `compose_hid_rules`) | `firmware_core_*`, `descriptor_composer` | `api_commands`, `api_events`, `api_raw_node` | `test_sim_control` (hold, answer, last sent, accept, stall, drop, timeout, rule order, fixed requests, 8/16/32-byte EP0 packet splitting) | — |
| Language bindings layout (Python, C#, Rust) | `generate.py --check`, `test_layout.py`, `MimHid.Tests`, `cargo test` | — | — | — |
| CMake package & release archives | `mimhid.package-multiarch-layout`, `mimhid.package-component-contract`, `tests/release/test_release_tools.py` | — | — | — |

The `hw` column is empty: no release is hardware-verified (below).

## Hardware verification status

[CHANGELOG.md](../CHANGELOG.md) records, as of this release:

- **0.1.0:** "Not hardware-verified: the host library, the protocol and the
  firmware core are tested with a simulated board over a pseudo-terminal and
  with the firmware core's own tests; an earlier firmware ran on a Raspberry Pi
  Pico, this one has not yet."
- **0.2.0:** "Not hardware-verified: the changes are tested with the firmware
  core's own tests, with the simulated board over a pseudo-terminal, and by
  building every port (RP2040, RP2350, nRF52840, STM32F4; the ESP32-S3 build is
  left to CI); none of it has run on a board yet."

[LATENCY.md](LATENCY.md#measured-once) holds one end-to-end measurement as it
was recorded. A hardware run is recorded as [tests/hw/README.md](../tests/hw/README.md#what-to-record-and-where)
describes.

## Coverage

When `MIMHID_COVERAGE=ON` is enabled, every target is compiled with
`--coverage -O0 -fprofile-update=atomic` and `gcovr` measures `src/` (with
`--exclude-throw-branches --exclude-unreachable-branches`, and the lines of the
C ABI exception macros excluded with
`--exclude-lines-by-pattern '.*MIMHID_C_(RESULT|VALUE|VOID)_CATCH.*'`; see
[tests/README.md](../tests/README.md#coverage)).

- **Current figures:** the `Coverage` job of the CI workflow writes them to its
  run summary and keeps the full report as the `coverage` artifact, for the
  commit it ran on. This page does not copy them, since they change with every
  commit.
- **Floor:** `gcovr` fails the `Coverage` job below **86% line coverage**
  (`--fail-under-line 86`) or **73% branch coverage** (`--fail-under-branch 73`).

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
  --print-summary
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
