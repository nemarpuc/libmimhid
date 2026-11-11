# Building

Requirements, CMake options and presets, firmware builds, tests, sanitizers,
coverage, and how to consume an installed package or build the language
bindings.

## Host library requirements

- A C11 compiler and a C++20 compiler: GCC 11+ (GCC 12+ for ThreadSanitizer),
  Clang 14+, AppleClang 14+, or Visual Studio 2022 (`cl.exe`).
- CMake 3.20 or later.
- Python 3.9 or later when `MIMHID_BUILD_TESTS` is enabled (used by the C ABI
  layout oracle check).
- No third-party runtime libraries. `libmimhid` talks to the microcontroller
  board over an operating-system serial port: `termios` / `termios2` and `poll`
  on Linux, `termios` and `IOSSIOSPEED` on macOS (`pthread` on both), and
  `CreateFileW` + overlapped I/O + `SetupDi*` on Windows (`setupapi` and
  `advapi32`).

## Building the host library from source

```sh
cmake -S . -B build -DMIMHID_BUILD_SHARED=ON -DMIMHID_BUILD_STATIC=ON
cmake --build build --config Release
cmake --install build --prefix staging --config Release
```

At least one of `MIMHID_BUILD_SHARED` and `MIMHID_BUILD_STATIC` must be `ON`.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `MIMHID_BUILD_SHARED` | `ON` | Build the shared library (`mimhid`, alias `mimhid::shared`) |
| `MIMHID_BUILD_STATIC` | `ON` | Build the static library (`mimhid_static`, alias `mimhid::static`) |
| `MIMHID_BUILD_TESTS` | `OFF` | Build the unit, scripted-board, golden-descriptor, ABI-layout, and CMake package tests |
| `MIMHID_BUILD_EXAMPLES` | `OFF` | Build the C and C++ example programs under [examples/](../examples) |
| `MIMHID_WERROR` | `ON` | Treat compiler warnings as errors (`-Werror` on GCC/Clang, `/WX` on MSVC) |
| `MIMHID_SANITIZE` | `OFF` | Instrument every target with AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=address,undefined -fno-sanitize-recover=all`) |
| `MIMHID_TSAN` | `OFF` | Instrument every target with ThreadSanitizer (`-fsanitize=thread -fno-sanitize-recover=all`, Linux with Clang or GCC 12+ and PIE support) |
| `MIMHID_COVERAGE` | `OFF` | Instrument every target with `--coverage -O0 -fprofile-update=atomic` for `gcov` / `gcovr` line and branch coverage (GCC or Clang) |
| `MIMHID_BUILD_FUZZ` | `OFF` | Build the `mimhid_fuzz_descriptor` libFuzzer target ([tests/fuzz/fuzz_descriptor.cpp](../tests/fuzz/fuzz_descriptor.cpp), Clang only) |
| `MIMHID_TINYUSB_PATH` | `""` | A cache variable (`cmake/mimhid-tests.cmake`), not an `option()`: the path to a TinyUSB source checkout. When set on a non-Windows host with `MIMHID_BUILD_TESTS=ON`, it builds the simulated-board test executables (`mimhid_sim_15`, `mimhid_sim_7`, `mimhid_sim_3`, `mimhid_sim_hs`; CTest `mimhid.sim-*`) |

`MIMHID_SANITIZE`, `MIMHID_TSAN`, `MIMHID_COVERAGE`, and `MIMHID_BUILD_FUZZ`
are mutually exclusive: CMake stops during configuration if more than one is
enabled.

## CMake presets

[CMakePresets.json](../CMakePresets.json) defines four Ninja presets:

| Preset | Build type | Cache variables | Purpose |
|---|---|---|---|
| `dev` | `Debug` | `MIMHID_BUILD_TESTS=ON`, `MIMHID_BUILD_EXAMPLES=ON` | Local development and unit/scripted testing in `build/dev` |
| `release` | `Release` | `MIMHID_BUILD_TESTS=OFF` | Optimized shared + static build in `build/release` |
| `tsan` | `RelWithDebInfo` | `MIMHID_BUILD_TESTS=ON`, `MIMHID_BUILD_EXAMPLES=ON`, `MIMHID_TSAN=ON` | ThreadSanitizer race-detection build and test run in `build/tsan` |
| `coverage` | `Debug` | `MIMHID_BUILD_TESTS=ON`, `MIMHID_BUILD_EXAMPLES=OFF`, `MIMHID_COVERAGE=ON` | Line and branch coverage build in `build/coverage` |

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For the complete test, sanitizer, and coverage commands, see
[VERIFICATION.md](VERIFICATION.md#reproducing-the-checks) and
[tests/README.md](../tests/README.md#tests).

## Using the installed package

### CMake `find_package`

An installation produced with both `MIMHID_BUILD_SHARED=ON` and
`MIMHID_BUILD_STATIC=ON` (including every official release archive) exports two
components, `shared` and `static`:

```cmake
find_package(mimhid 0.3 CONFIG REQUIRED COMPONENTS shared)
target_link_libraries(my_c_app PRIVATE mimhid::mimhid)
```

Available imported targets:

| Target | Component | Description |
|---|---|---|
| `mimhid::mimhid` | `shared` (or `static` when only `static` is requested) | Shared library (`libmimhid.so`, `libmimhid.dylib`, `mimhid.dll`), or alias for `mimhid::mimhid_static` when only the `static` component is loaded |
| `mimhid::mimhid_static` | `static` | Static archive (`libmimhid_static.a` when built alongside `shared`, or `libmimhid.a` in a static-only build; `mimhid_static.lib` on Windows) along with its required interface link libraries (`pthread` + C++ runtime on POSIX, `setupapi` + `advapi32` on Windows) |
| `mimhid::cpp` | `shared` (or `static` in a static-only install) | Header-only C++20 interface target ([include/mimhid.hpp](../include/mimhid.hpp)) linked against `mimhid::mimhid` |
| `mimhid::cpp_static` | `static` | Header-only C++20 interface target linked against `mimhid::mimhid_static` |

While the major version is `0`, `mimhid-config-version.cmake` uses
`SameMinorVersion` compatibility: a `find_package(mimhid 0.3)` request is
satisfied by `0.2.x` installations only.

### `pkg-config`

- Shared library:
  ```sh
  pkg-config --cflags --libs mimhid
  ```
- Static library (in a combined shared + static installation or release archive):
  ```sh
  pkg-config --static --cflags --libs mimhid-static
  ```
  Even when called from pure C, the static archive contains C++ object files
  compiled with `-fno-rtti` and requires the platform C++ standard library
  (`-lstdc++` or `-lc++`) and `-lpthread` on POSIX (`-lsetupapi -ladvapi32` on
  Windows), which `mimhid-static.pc` lists under `Libs.private`.

## Building the firmware

The firmware lives under [firmware/](../firmware). One portable C core
([firmware/common/](../firmware/common)) implements the serial framing, SPSC
report queues, dynamic USB descriptors, and endpoint 0 control-transfer handler
on top of [TinyUSB](https://github.com/hathach/tinyusb); each MCU port supplies
the twelve functions in [firmware/common/hal.h](../firmware/common/hal.h).

Nothing about the chip, the board, or the UART pinout is guessed by the build:
every port requires its chip/board and UART pins on the command line and fails
at configure or compile time if any required setting is omitted.

| Target | Source directory | USB controller | `MIMHID_HID_ITFS` | `MIMHID_MAX_ENDPOINT_NUMBER` | `MIMHID_MAX_ENDPOINT_BYTES` | Max UART rate (`hal_uart_max_baud`) | Cores |
|---|---|---|---:|---:|---:|---:|---|
| RP2040 | `firmware/` (`rp/`) | Full-Speed device | 15 | 15 | 64 | `clk_peri / 16` (7,812,500 at 125 MHz) | Core 0 USB, Core 1 UART + DMA ring |
| RP2350 | `firmware/` (`rp/`) | Full-Speed device | 15 | 15 | 64 | `clk_peri / 16` (9,375,000 at 150 MHz) | Core 0 USB, Core 1 UART + endless DMA ring |
| nRF52840 | `firmware/ports/nrf52840` | Full-Speed USBD | 7 | 7 | 64 | 1,000,000 baud | Single loop (`UARTE1` 1-byte EasyDMA + ISR software ring, `UARTE0` disabled) |
| STM32F4 | `firmware/ports/stm32f4` | Full-Speed OTG_FS | 3 (5 on F412) | 3 (5 on F412) | 64 | PCLK1 / 8 | Single loop (`USART2` + `DMA1_Stream5` circular DMA ring) |
| ESP32-S3 | `firmware/ports/esp32s3` | Full-Speed OTG | 4 | 6 | 64 | 5,000,000 baud | Single loop (`UART1` hardware FIFO + driver ring) |

### RP2040 and RP2350 (Pico SDK)

Requires `PICO_SDK_PATH` and an `arm-none-eabi-gcc` toolchain. Specify
`PICO_PLATFORM` (`rp2040`, `rp2350`, or `rp2350-arm-s`), `PICO_BOARD`,
`MIMHID_UART_ID` (`uart0` or `uart1`), `MIMHID_UART_TX_PIN`, and
`MIMHID_UART_RX_PIN`:

```sh
# RP2040 (Raspberry Pi Pico, UART0 on GP0 TX / GP1 RX)
cmake -S firmware -B build-rp2040 \
  -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico \
  -DMIMHID_UART_ID=uart0 -DMIMHID_UART_TX_PIN=0 -DMIMHID_UART_RX_PIN=1
cmake --build build-rp2040

# RP2350 (Raspberry Pi Pico 2, UART0 on GP0 TX / GP1 RX)
cmake -S firmware -B build-rp2350 \
  -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 \
  -DMIMHID_UART_ID=uart0 -DMIMHID_UART_TX_PIN=0 -DMIMHID_UART_RX_PIN=1
cmake --build build-rp2350
```

Each build produces `mimhid_firmware.uf2`. Hold `BOOTSEL` while plugging the
board's USB port into the host and copy the `.uf2` file to the mass-storage
volume.

### nRF52840 and STM32F4 (TinyUSB BSP)

Requires a TinyUSB checkout (`TINYUSB_PATH`) with the family's vendor HAL
fetched via `python3 $TINYUSB_PATH/tools/get_deps.py nrf` or `stm32f4`, plus
`arm-none-eabi-gcc`:

```sh
# nRF52840 (Nordic PCA10056 DK)
cmake -S firmware/ports/nrf52840 -B build-nrf52840 -G Ninja \
  -DTINYUSB_PATH=/path/to/tinyusb -DBOARD=pca10056
cmake --build build-nrf52840

# STM32F4 (WeAct Black Pill V2.0, STM32F411CEU6)
cmake -S firmware/ports/stm32f4 -B build-stm32f4 -G Ninja \
  -DTINYUSB_PATH=/path/to/tinyusb -DBOARD=stm32f411blackpill
cmake --build build-stm32f4
```

### ESP32-S3 (ESP-IDF; CI builds with v5.4.2)

Requires an activated ESP-IDF environment (`$IDF_PATH/export.sh`) and
`TINYUSB_PATH` in the environment. Both UART GPIO numbers must be specified:

```sh
export TINYUSB_PATH=/path/to/tinyusb
idf.py -C firmware/ports/esp32s3 -B build-esp32s3 \
  -DMIMHID_UART_TX_PIN=17 -DMIMHID_UART_RX_PIN=18 set-target esp32s3
idf.py -C firmware/ports/esp32s3 -B build-esp32s3 \
  -DMIMHID_UART_TX_PIN=17 -DMIMHID_UART_RX_PIN=18 build
```

### Firmware core host tests

[firmware/tests/](../firmware/tests) compiles `core.c`, `control.c`, `ep0.c`,
and `usb_descriptors.c` on the build host (without an MCU toolchain) across the
interface counts `15`, `7`, `4`, and `3`, a High-Speed configuration, header
inclusion order checks, and missing-macro compile failure checks:

```sh
cmake -S firmware/tests -B build-fw-tests -DTINYUSB_PATH=/path/to/tinyusb
cmake --build build-fw-tests
ctest --test-dir build-fw-tests --output-on-failure
```

## Language bindings

- **Python** ([bindings/python/](../bindings/python)): pure `ctypes` package
  (`mimhid`). It reads no environment variable: pass the full path of
  `libmimhid.so`, `libmimhid.dylib`, or `mimhid.dll` to `mimhid.load(path)`. Build a wheel with `python3 -m build --wheel bindings/python`.
- **C#** ([bindings/csharp/MimHid/](../bindings/csharp/MimHid)): .NET 8 P/Invoke
  binding (`MimHid`). Pack with `dotnet pack bindings/csharp/MimHid/MimHid.csproj -c Release`.
- **Rust** ([bindings/rust/mimhid-sys/](../bindings/rust/mimhid-sys)): raw FFI
  crate (`mimhid-sys`). Set `MIMHID_LIB_DIR` to the directory that holds the
  library before running `cargo build` or `cargo test` (its `build.rs` reads no
  other variable).

All three bindings are generated from [include/mimhid.h](../include/mimhid.h) by
`tools/bindings/generate.py`. Verify that the checked-in bindings match
the header with:

```sh
python3 tools/bindings/generate.py --check
```
