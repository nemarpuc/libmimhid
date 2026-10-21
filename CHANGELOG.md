# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.3] - 2026-10-21

### Fixed
- **Pure passthrough for `SET_CONFIGURATION`.** `SET_CONFIGURATION` is now matched by control routing rules and reported to the host/application, allowing full passthrough to target devices and accurate replication of configuration state transitions.
- **Immediate Status stage handshake for zero-length control transfers.** Control transfers with `wLength == 0` now always complete with a standard IN Zero-Length Packet (ZLP) handshake regardless of setup direction, resolving deadlock when answering zero-length descriptor and feature requests.
- **Dynamic configuration descriptor boundary check.** `tud_descriptor_configuration_cb` validates configuration index dynamically against `g_device.bNumConfigurations`, cleanly stalling requests for non-existent configurations.

## [1.2.2] - 2026-10-21

### Fixed
- **Strict qualification for unheld control requests (`is_fixed`).** Standard `SET_ADDRESS` and `SET_CONFIGURATION` requests are only exempted from control routing hold when they are valid standard OUT requests (`bmRequestType == 0x00`, `wLength == 0`, `wIndex == 0`). Non-standard, vendor, or IN-direction variants (such as `0x80 0x05`) are now matched by control routing rules and reported to the host/application instead of being intercepted by TinyUSB.

## [1.2.1] - 2026-10-21

### Added
- **Silent drop control action (`MIMHID_CONTROL_ACTION_DROP`).** `mimhid_device_control_reply` and `mimhid_control_rule.when_late` support dropping a held transfer silently without returning data or issuing a STALL handshake. The board resets the hold state while leaving EP0 un-armed, allowing hardware flow control (NAK) to continue uninterrupted until the target host aborts the transfer, matching the behavior of physical devices that ignore unsupported vendor requests.

## [1.2.0] - 2026-10-19

Firmware and host library must both be this version: the wire protocol is version 5 (the PONG grew by three bytes).

### Added
- **Control transfers of the target.** Everything the target sends on endpoint 0 (descriptor reads, class and vendor requests, `GET_REPORT`, `SET_REPORT`, feature selectors) can be reported, and the answer to chosen requests can come from the program. `mimhid_device_set_control_routing` sets up to 8 rules (a mask and value for `bmRequestType`, `bRequest`, `wValue`, `wIndex`; a time; what to do when the time runs out) and whether every transfer is reported. `MIMHID_EVENT_CONTROL_SETUP` carries the setup packet, a token and flags; `mimhid_device_control_reply` answers the held transfer with data, an acknowledgement, a stall, or leaves it to the board's own handling. A host-to-device transfer with a data stage reports the data (`MIMHID_EVENT_CONTROL_DATA`) and waits for a verdict. Nothing changes until the program asks: with no rule and no report flag every request goes to TinyUSB as before. Rules and times have no defaults; `mimhid_device_close` removes the rules.
- `mimhid_board_limits` gains `max_control_data_bytes` and `max_control_rules`; `mimhid_event` gains `control_token` and `control_flags`.
- SET_ADDRESS and SET_CONFIGURATION are reported and never held: they change the state of the board's own USB controller. A host-to-device data stage longer than `max_control_data_bytes` is reported as too long and never held.

### Changed
- **Every firmware port links with `-Wl,--wrap=dcd_event_handler`** and compiles `common/control.c` (the RP2040, RP2350, STM32F4 and nRF52840 projects and the ESP32-S3 component do). The firmware takes the setup packet where TinyUSB receives it, and a held request is handed to TinyUSB later, either unchanged or retyped as a vendor request that `tud_vendor_control_xfer_cb` answers; TinyUSB's control state machine is not replaced. A port with its own `tud_vendor_control_xfer_cb` conflicts with it.
- The PONG carries `max_control_data_bytes` and `max_control_rules`; the unique id moved to byte 27.

## [1.1.0] - 2026-10-19

Firmware and host library must both be this version: the wire protocol is version 4. A board on protocol 3 is listed by `mimhid_discover` with its version, to be flashed again, and refused by `mimhid_device_open` with `MIMHID_ERR_PROTOCOL`.

### Added
- **The board's limits are its own and are reported.** The PONG carries `max_report_bytes`, `max_feature_bytes`, `max_endpoint_bytes` and `max_frame_payload` as two-byte values. New `mimhid_board_limits` and `mimhid_device_limits()` read them. A firmware is built with the largest the chip can hold: a Feature answer of up to 1024 bytes (it was 32), so a Feature report of 644 bytes can be answered; endpoint packets and reports of 1024 bytes on a high-speed chip (64 on the full-speed chips of today, untried on high speed for want of a board).
- **Frames longer than 127 bytes.** `LEN` takes a second byte; a shorter frame is the frame of protocol 3.

### Changed
- `NODE_BEGIN` carries both endpoint sizes as two bytes. The OUT report event carries a two-byte length.
- `mimhid_node_open`/`_open_raw` check `ep_in_size` and `ep_out_size` against the board's `max_endpoint_bytes` (`MIMHID_ERR_OVERFLOW`); `mimhid_node_set_feature_response` against its `max_feature_bytes`.
- `mimhid_device_info.max_report_bytes` stays a byte and saturates at 255; use `mimhid_device_limits` for the exact figure.

## [1.0.2] - 2026-10-19

### Changed
- **Endpoint packet sizes are free.** `ep_in_size` is any 1 to 64 and `ep_out_size` is 0 or any 1 to 64, written to the endpoint descriptor as given; they were limited to 8, 16, 32 and 64. A device that announces an endpoint of 6 bytes can now be copied exactly. The firmware's check changed too (`core.c`), so a board must be flashed again: an older firmware answers a size outside the old set with a refusal (`MIMHID_ERR_PARAM`, "The board refused the node's interface settings"). The wire protocol is unchanged.

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
