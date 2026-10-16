# Contributing

Use the [issue tracker](https://github.com/nemarpuc/libmimhid/issues) for
proposed changes and submit patches through
[pull requests](https://github.com/nemarpuc/libmimhid/pulls).

## Technical Standards

Every protocol, descriptor, or platform statement must cite primary
technical specifications: USB HID 1.11, HID Usage Tables (HUT 1.5+),
Raspberry Pi RP2040 / RP2350 Datasheets, and TinyUSB documentation. Do not guess Usage IDs
or serial wire formats.

Code must keep transport settings, HID report profiles, and descriptor parameters
caller-controlled. Preserve deterministic latency and minimal resource usage:
- Hot paths (report serialization, queue push/pop, UART send/receive) must remain
  free of heap allocations, mutexes, and avoidable copies.
- Host C library ABI and firmware must adhere to clean C11.
- C++ headers and tests must adhere to modern C++20.
- All code must conform to `.clang-format` and `.clang-tidy`.

## Development Workflow

1. Configure with standard CMake presets:
   ```bash
   cmake --preset debug
   cmake --build --preset debug
   ```
2. Run the test suite:
   ```bash
   ctest --preset test
   ```
3. Format code:
   ```bash
   clang-format -i include/*.h include/*.hpp src/**/*.cpp src/**/*.hpp tests/*.cpp examples/**/*.c examples/**/*.cpp
   ```
4. Verify firmware compilation. The Raspberry Pi Pico (RP2040) and Raspberry Pi Pico 2 (RP2350) images have official support and need the ARM GCC toolchain and the Pico SDK; the other ports are listed in [README.md](README.md#firmware). The pinned SDK and TinyUSB versions are in `.github/scripts/`.
   ```bash
   cmake -B build_firmware -S firmware
   cmake --build build_firmware --parallel
   ```
5. Run the firmware core tests (they need only a TinyUSB checkout):
   ```bash
   cmake -B build_fwtest -S firmware/tests -DTINYUSB_PATH=<tinyusb>
   cmake --build build_fwtest && ctest --test-dir build_fwtest
   ```
6. Run the end-to-end tests (POSIX; they need a TinyUSB checkout):
   ```bash
   cmake -B build_e2e -S . -DMIMHID_TINYUSB_PATH=<tinyusb>
   cmake --build build_e2e && ctest --test-dir build_e2e -R e2e
   ```
7. Submit pull requests with concise descriptions and test verification evidence.
