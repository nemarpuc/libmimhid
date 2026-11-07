# Target Matrix

Only what has actually been run is marked as verified. "Builds" means the firmware or library compiled and linked in CI or in the development environment; it says nothing about behavior on hardware.

## Hardware

Official support is provided for both the Raspberry Pi Pico (RP2040) and Raspberry Pi Pico 2 (RP2350).

| Board | Interfaces | Status |
|---|---|---|
| Raspberry Pi Pico / Pico H (RP2040) | 15 interfaces (max) | Official support. Verified on hardware with GP0/GP1 UART at 7 interfaces, the maximum of an earlier firmware: all seven enumerate and every profile's input arrives. Also verified (2026-11-07) with firmware 0.1.0 at 15 interfaces: 15 mouse nodes in one commit enumerated on the host as 15 interfaces, and the board's link counters (CRC, framing, UART overrun, ring) all read 0 after the link had run at 7,812,500 baud for 14 s. Only that enumeration was checked at 15 interfaces, not every profile's input. |
| Raspberry Pi Pico 2 (RP2350) | 15 interfaces (max) | Official support. Dual-core RP architecture with `.scratch_y` DMA ring; builds and verified in CI / e2e tests. |
| RP2040 `pico_w` | 15 interfaces (max) | Builds; not run. |
| nRF52840 (`pca10056` DK by default) | 7 interfaces (max) | Builds; not run. |
| STM32F4 (`stm32f411blackpill` by default) | 3 interfaces (max) | Builds; not run. |
| ESP32-S3 | 4 interfaces (max) | Builds with ESP-IDF 5.4; not run. |
| Other boards | | Not tested. See [PORTING.md](PORTING.md). |

The portable firmware core is tested on the build host against a fake HAL for 15, 7, 4, and 3 interfaces and once built for high speed (`firmware/tests`), and together with the real host library through a pseudo-terminal for 15, 7, and 3 interfaces and once for high speed (`tests/e2e`). The one hardware run at 15 interfaces (RP2040, 7,812,500 baud) is the only run at high speed; the other high-speed rules are checked on the build host only.

## Control transfers

The control-transfer routing is the only part of the firmware that sits between TinyUSB and the USB controller, so its status is listed apart. "Logic" means the firmware core's host-side tests and the end-to-end test (a simulated TinyUSB around the same `control.c`), which run for every target. The last column says what has run on a board with a real USB host.

| Board | Builds (wrap applied) | Run on a board |
|---|---|---|
| RP2040 | Yes | Verified with a Linux 7.2 host (libusb): every request reported (string and report descriptor reads, `SET_IDLE`); a held vendor read answered with data in 2.5 ms; an unrouted vendor read stalled. Rule timeout, host-to-device transfers and `LEAVE` not run on the board. |
| RP2350 | Yes | No. |
| STM32F4 | Yes | No. |
| nRF52840 | Yes | No. |
| ESP32-S3 | Yes (ESP-IDF 5.4.2; the disassembly shows the DWC2 driver calling `__wrap_dcd_event_handler`) | No. |

Not run anywhere: a high-speed endpoint 0, a host that gives up while a request is held, and Windows or macOS hosts.

## Host

| OS | Status |
|---|---|
| Linux (Arch, x86_64) | Library and tests build and pass; run against a Pico H. |
| Ubuntu (CI) | Library, tests, and examples build; tests pass, including a pseudo-terminal loopback of the serial backend. |
| macOS (CI, Apple silicon) | Builds and runs the tests in CI, including the pseudo-terminal loopback. Not run against a board. |
| Windows (CI, MSVC) | Builds and runs the unit tests in CI. The serial backend is not run against a COM port or a board. |

## Target devices

| Target | Status |
|---|---|
| Linux 7.2 PC (`hid-generic`, `hid-multitouch`) | Verified (seven interfaces): keyboard, mouse, touchscreen (declared maxima and slot count applied), touchpad, pen, gamepad, consumer control, reset, and release on close. The kernel logs `hid-multitouch: failed to fetch feature 0` for the two touch interfaces; the driver then uses the descriptor's Logical Maximum, so behavior is unaffected. |
| Linux with packed interfaces (Report IDs sharing an interface) | Not tested. |
| Windows, macOS | Not tested. |
