# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-10-18

Firmware and host library must both be this version: the wire protocol is version 3.

### Changed
- **Line rate agreed at run time.** Every board starts at 115,200 baud. The host asks for a faster rate with the new `SET_BAUD` command and keeps it only after three pings each way come through, so a serial adapter that silently sends at another rate is caught; both sides fall back to 115,200 otherwise. `baud_rate` is required (`MIMHID_ERR_UNSET_FIELD` when 0) and is used exactly (`MIMHID_ERR_UNSUPPORTED` when it is not carried); `mimhid_discover` reports the fastest rate the link carries. The rate is no longer a firmware build setting (`MIMHID_BAUD_RATE` is gone); rates come from `MIMHID_BAUD_RATES`.
- **PONG (protocol 3)** adds the chip, the USB speed, the current rate, the rates the board can make, and the chip's unique id.
- **`mimhid_device_open` talks to the board** before it returns: it finds it (also when a program that went away left it on a fast rate), reads its real limits, and switches to the caller's rate. It returns `MIMHID_ERR_TIMEOUT` when no board answers and `MIMHID_ERR_PROTOCOL` for firmware on another protocol.
- **`mimhid_discover`** searches the ports in parallel, measures each board's fastest rate (`baud_rate`) and leaves it on 115,200; `timeout_ms` is now the wait for each answer. Boards on another protocol are listed with their version.
- **`mimhid_device_close`** puts the board back on 115,200.
- A serial port is opened for one process alone (`TIOCEXCL` on Linux and macOS), so a search no longer pings a board another program is using.
- **No defaults in `mimhid_device_options`.** `control_timeout_ms`, `send_timeout_ms`, `reconnect_timeout_ms`, `close_drain_timeout_ms` and `language_id` are required (`MIMHID_ERR_UNSET_FIELD` when 0). `bcd_device` and `max_power_ma` are used as given (0x0000 and 0 mA are valid). A NULL `manufacturer` or `product` leaves the string out instead of announcing "mimhid" / "mimhid Composite HID". `maximum_report_bytes` 0 still means the limit the board reports.
- `mimhid_device_info` grows `baud_rate`, `baud_mask`, `chip`, `usb_high_speed`, `unique_id_length` and `unique_id` (168 bytes).

### Fixed
- `mimhid_device_open` filled the board limits with fixed values (8 interfaces) instead of the board's: on boards with fewer interfaces (STM32F4: 3, ESP32-S3: 4) nodes were assigned to interfaces the board does not have. An interface number past the board's count is now refused with `MIMHID_ERR_OVERFLOW` before anything is sent.
- STM32F4: the USART divider is computed for each rate, with 8× oversampling where 16× cannot reach it, and rates the clock cannot make within 2 % are not offered.
- Documentation gave 921,600 baud while the firmware ran at 3,000,000.

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
