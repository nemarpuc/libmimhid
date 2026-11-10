# Contributing

Use the [issue tracker](https://github.com/nemarpuc/libmimhid/issues) for
proposed changes and submit patches through
[pull requests](https://github.com/nemarpuc/libmimhid/pulls).

## Technical Standards

Every protocol, descriptor, or platform statement must cite primary
technical specifications: USB HID 1.11, HID Usage Tables 1.7 (the version `include/mimhid.h` cites),
Raspberry Pi RP2040 / RP2350 Datasheets, and TinyUSB documentation. Do not guess Usage IDs
or serial wire formats.

Code must keep transport settings, HID report profiles, and descriptor parameters
caller-controlled. Preserve deterministic latency and minimal resource usage:
- Hot paths (report serialization, queue push/pop, UART send/receive) must remain
  free of heap allocations and avoidable copies, and take no lock beyond the one
  they take now: on the host a report is written under `Link::tx_mutex_` (one
  frame on the wire at a time); the board's report queues take no lock.
- Host C library ABI and firmware must adhere to clean C11.
- C++ headers and tests must adhere to modern C++20.
- All code must conform to `.clang-format` and `.clang-tidy` (CI uses clang-format 14.0.6 and clang-tidy 14).

## Bindings

`bindings/*/` declarations and `tests/abi/layout_oracle.c` are generated. After changing `include/mimhid.h`, run `python3 tools/bindings/generate.py`, and commit the result; CI runs it with `--check`.

## Releasing

`python3 tools/release/sync_version.py X.Y.Z` writes the version into every file that carries it (the list is `tools/release/version_files.py`; `validate_release.py` reads the same list). Then run `python3 tools/bindings/generate.py` for the constants in the generated bindings, add the dated `CHANGELOG.md` section, and tag.

## Development Workflow

1. Configure with standard CMake presets:
   ```bash
   cmake --preset dev
   cmake --build --preset dev
   ```
2. Run the test suite:
   ```bash
   ctest --preset dev
   ```
3. Format code:
   ```bash
   clang-format -i include/*.h include/*.hpp src/**/*.cpp src/**/*.hpp tests/*.cpp examples/**/*.c examples/**/*.cpp
   ```
4. Verify firmware compilation. The Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) images need the ARM GCC toolchain and the Pico SDK; the other ports are listed in [docs/BUILDING.md](docs/BUILDING.md#building-the-firmware). The pinned SDK and TinyUSB versions are in `.github/scripts/`.
   ```bash
   cmake -B build_firmware -S firmware -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico -DMIMHID_UART_ID=uart0 -DMIMHID_UART_TX_PIN=0 -DMIMHID_UART_RX_PIN=1
   cmake --build build_firmware --parallel
   ```
5. Run the firmware core tests (they need only a TinyUSB checkout):
   ```bash
   cmake -B build_fwtest -S firmware/tests -DTINYUSB_PATH=<tinyusb>
   cmake --build build_fwtest && ctest --test-dir build_fwtest
   ```
6. Run the simulated-board tests (POSIX; they need a TinyUSB checkout):
   ```bash
   cmake -B build_sim -S . -DMIMHID_BUILD_TESTS=ON -DMIMHID_TINYUSB_PATH=<tinyusb>
   cmake --build build_sim && ctest --test-dir build_sim -R sim
   ```
7. Submit pull requests with concise descriptions and test verification evidence.
