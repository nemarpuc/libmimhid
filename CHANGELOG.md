# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Removed
- `mimhid_device_set_output_report_callback` and `mimhid_device_set_set_report_callback`: `event_sink` delivers the same events.
- Android-specific API: `mimhid_android_status`, `MIMHID_ANDROID_*`, `mimhid_capability_manifest.android_status`, `mimhid_gamepad_axis_option.expected_android_axis`, and `mimhid_raw_options.acknowledges_no_android_support`.

## [0.1.0] - 2026-10-16

Initial release of `libmimhid`: ultra-low-latency serial-to-USB HID composite peripheral library and dual-core firmware.

### Highlights
- **Hardware Support**: Official dual-core support for Raspberry Pi Pico (RP2040) and Raspberry Pi Pico 2 (RP2350) with circular DMA UART reception at 3,000,000 baud (3.0 Mbps), 1024-byte circular DMA buffer, and 2048-byte report descriptor pool. Additional ports for STM32F4 (3.0 Mbps), ESP32-S3 (3.0 Mbps), and nRF52840 (1.0 Mbps hardware maximum).
- **Physical Stealth Waiting**: D+=0V stealth waiting at power-on (`tud_disconnect()`). The microcontroller stays invisible to the USB host until explicitly committed by the PC.
- **Instant Connect & Deterministic Re-enumeration**: Initial attach connects immediately with 0ms delay; dynamic re-enumeration uses a caller-specified `delay_ms` non-blocking timer.
- **Explicit Interface Control**: Zero implicit heuristics or `-1` magic values; caller specifies exact `interface_number` per node (supporting both Top-Level Collection multiplexing with Report IDs and independent interface separation).
- **Dynamic Bidirectional Pipeline**: Dynamic Interrupt IN and OUT endpoints (`ep_out_size` up to 64 bytes) with the `event_sink` callback.
- **Raw Descriptor Node Support**: Direct raw report descriptor registration via `mimhid_node_open_raw` and `mimhid_raw_submit`, allowing arbitrary custom/vendor-defined HID profiles.
- **Host Library & Tools**: Clean C ABI (`mimhid.h`), header-only C++20 forwarder (`mimhid.hpp`), CMake integration (`mimhid::mimhid`), udev rules, symbol linting, and full test suite with physical hardware verification.
