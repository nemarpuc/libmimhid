# Target Matrix

Only what has actually been run is marked as verified. "Builds" means the firmware or library is compiled and linked by the CI workflow (`.github/workflows/ci.yml`); it says nothing about behavior on hardware. No release is hardware-verified ([CHANGELOG.md](../CHANGELOG.md), [VERIFICATION.md](VERIFICATION.md#hardware-verification-status)). See [VERIFICATION.md](VERIFICATION.md) for the feature-by-layer matrix.

## Hardware

| Board (CI build) | Interfaces | Status |
|---|---|---|
| Raspberry Pi Pico (RP2040; `PICO_BOARD=pico`) | 15 (max) | Builds, and the firmware core passes the simulated-board tests; not hardware-verified. `CHANGELOG.md` `[0.1.0]` records that an earlier firmware ran on a Raspberry Pi Pico. |
| Raspberry Pi Pico 2 (RP2350; `PICO_BOARD=pico2`) | 15 (max) | Builds, and the firmware core passes the simulated-board tests; not verified on hardware. |
| nRF52840 (`-DBOARD=pca10056`, the DK) | 7 (max) | Builds; not run on hardware. |
| STM32F4 (`-DBOARD=stm32f411blackpill`) | 3 (max; 5 on the F412 boards) | Builds; not run on hardware. |
| ESP32-S3 | 4 (max) | Builds (CI builds with ESP-IDF v5.4.2); not run on hardware. |
| Other boards | | Not tested. See [PORTING.md](PORTING.md). |

The portable firmware core is tested on the build host against a fake HAL for 15, 7, 4, and 3 interfaces and once built for high speed (`firmware/tests`), and together with the real host library through a pseudo-terminal for 15, 7, and 3 interfaces and once for high speed (`tests/sim`). The high-speed rules are checked on the build host only.

## Control transfers

The control-transfer routing is the only part of the firmware that sits between TinyUSB and the USB controller, so its status is listed apart. Its logic is tested by the firmware core's host-side tests (`firmware/tests`) and the simulated-board tests (`tests/sim`, a simulated TinyUSB around the same `control.c` and `ep0.c`).

| Board | Builds with `-Wl,--wrap=dcd_event_handler` | Run on a board |
|---|---|---|
| RP2040, RP2350 | Yes (`firmware/CMakeLists.txt`) | No. |
| STM32F4 | Yes (`firmware/ports/stm32f4/CMakeLists.txt`) | No. |
| nRF52840 | Yes (`firmware/ports/nrf52840/CMakeLists.txt`) | No. |
| ESP32-S3 | Yes (`firmware/ports/esp32s3/main/CMakeLists.txt`) | No. |

`DROP` disarms endpoint 0 only on the RP2040 and RP2350: the other ports' `hal_control_silent_drop` is empty.

## Host

| OS | Status |
|---|---|
| Ubuntu (CI, `ubuntu-24.04`) | Library, tests, and examples build; tests pass, including the simulated-board (`sim`) and scripted pseudo-terminal suites. |
| macOS (CI, `macos-15`) | Builds and runs the tests, including the pseudo-terminal suites (`sim` and `scripted`). Not run against a board. |
| Windows (CI, `windows-2025`) | Builds and runs the unit tests. The serial backend is not run against a COM port or a board. |

## Target devices

| Target | Status |
|---|---|
| Linux with packed interfaces (Report IDs sharing an interface) | Tested in `tests/sim`; not tested on hardware. |
| Any target | Not hardware-verified. |
