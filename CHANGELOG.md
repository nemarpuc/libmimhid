# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html); until 1.0.0 a
change of the minor number may change the API, the ABI or the wire protocol.

## [Unreleased]

- **The release no longer publishes `*-runtime` bundles.** The `*-shared` archive already carries the
  shared library with the same license documents, and nothing in the project consumed the smaller
  bundle. A release now has 35 assets instead of 41.

## [0.2.0] - 2026-11-08

The wire protocol, the C ABI and the firmware build interface change; a 0.1.0 board and a 0.2.0
library do not talk to each other (`MIMHID_ERR_PROTOCOL`), and the reverse. **Not
hardware-verified**: the changes are tested with the firmware core's own tests, with the simulated
board over a pseudo-terminal, and by building every port (RP2040, RP2350, nRF52840, STM32F4;
the ESP32-S3 build is left to CI); none of it has run on a board yet.

- **Nothing about the chip is assumed by the firmware build.** Each port states the interfaces
  (`MIMHID_HID_ITFS`), the highest endpoint number (`MIMHID_MAX_ENDPOINT_NUMBER`), the endpoint
  packet size (`MIMHID_MAX_ENDPOINT_BYTES`), `CFG_TUSB_OS`, `CFG_TUSB_RHPORT0_MODE` and
  `CFG_TUD_MAX_SPEED` on the command line; a missing one is a compile error that names it. The
  nRF52840 no longer gets 7 by default, `core.h` no longer picks 15 for the RP chips from
  `PICO_RP2040`, and `tusb_config.h` no longer defaults to full speed and no OS. The Pico build
  needs `PICO_PLATFORM`, `PICO_BOARD`, `MIMHID_UART_ID`, `MIMHID_UART_TX_PIN` and
  `MIMHID_UART_RX_PIN`; the ESP32-S3 build both pins; the nRF52840 and STM32F4 builds `-DBOARD`
  (the STM32F4 port records the endpoint count of each board's MCU and refuses an unlisted board,
  and gives the F412 boards 5 interfaces, not 3).
- **A number the controller does not have is refused.** Until now an endpoint address such as
  `0x8F` was accepted by the host and the board, TinyUSB then failed the target's
  SET_CONFIGURATION, and the commit ended in `MIMHID_ERR_TIMEOUT` (on the nRF52840 the endpoint
  number 8 was opened as a corrupt isochronous endpoint without an error). The PONG now carries
  `max_endpoint_number` (byte 34; the unique id moved to 35 and 36..; `MIMHID_PONG_MIN_LEN` 36),
  `mimhid_device_info.reserved8` became `max_endpoint_number`, `mimhid_board_limits` gained
  `max_endpoint_number` (its size is 36), and `mimhid_node_open` returns `MIMHID_ERR_OVERFLOW`
  for a larger number. The firmware refuses it with `NAK_OVERFLOW`.
- **The firmware fills nothing in.** A zero LANGID, `bcdUSB`, `bcdHID`, IN endpoint address, size
  or interval, an `bmAttributes` without bit 7, and an OUT endpoint that is only partly given are
  refused (`NAK_PARAM`); a COMMIT whose nodes of one interface differ, or that uses an endpoint
  number twice in one direction, is refused (`NAK_DESCRIPTOR`). The host library already
  required all of these, so a program that works with the library is not affected.
- **The initializers fill nothing in.** Every `mimhid_*_init` function (and its `mimhid::` twin)
  zeroes the structure and sets `struct_size`, and nothing else: the pid.codes test IDs, the
  strings, the boot rate, the handshake and commit timeouts, the report size, the Linux HID limits,
  `bm_attributes`, the node's class, `bcdHID`, endpoint and interval, and every profile's fields
  (the mouse buttons and ranges, the keyboard Usages, the gamepad axes, the touch and pen
  fields) are the program's to set, and a required one left at zero is refused with
  `MIMHID_ERR_UNSET_FIELD`. `mimhid_node_config_init` and `mimhid::node_config()` lost their
  `interface_number` argument (set `iface.number` and `ep_in.address` yourself), and the four
  constants `MIMHID_CONTROL_TIMEOUT_MS`, `MIMHID_SEND_TIMEOUT_MS`, `MIMHID_DRAIN_TIMEOUT_MS` and
  `MIMHID_RECONNECT_TIMEOUT_MS` are gone. The README, `docs/API.md` (which lists what each
  structure requires) and the examples show every value set.
- **A vendor ID of zero is refused.** `usb_device.vendor_id` joined `bcd_usb`, `language_id` and
  `ep0_max_packet_size` as a field the library will not guess, so a program that forgot it no
  longer presents `0000:xxxx` (or, until now, the pid.codes test IDs the initializer used to put
  in). A product ID of zero is still a valid one.
- **nRF52840:** the D+ pull-up is taken down again after every USB power event until a
  configuration is committed, so a board that has not been committed no longer enumerates as
  `CAFE:4000` with no interface when the cable is plugged in.

## [0.1.0] - 2026-11-07

The first release of this line. **Not hardware-verified**: the host library, the protocol and the
firmware core are tested with a simulated board over a pseudo-terminal and with the firmware
core's own tests; an earlier firmware ran on a Raspberry Pi Pico, this one has not yet.

- **What it is.** A PC-managed USB emulation coprocessor: a host library (C ABI in
  `include/mimhid.h`, header-only C++ forwarders in `mimhid.hpp`, Python, C# and Rust bindings) and
  the firmware of a board that turns serial frames into the reports of a USB HID composite
  device. Boards: RP2040 and RP2350 (the reference), STM32F4, nRF52840 and ESP32-S3 (build-only
  ports).
- **Devices and Nodes.** A Device is one board on one serial port. Its Nodes are HID interfaces
  with an immutable specification: keyboard, mouse, touchscreen, touchpad, pen, digitizer, gamepad,
  toggle controls, or a raw report descriptor. Descriptors, interface and endpoint numbers, polling intervals, strings
  and the USB identity are given by the program; nothing is inferred.
- **Strings are Unicode.** UTF-8 on the API, UTF-16 in the string descriptor (USB 2.0 9.6.7), up
  to 126 code units; control characters and malformed text are refused.
- **The line is as fast as it carries.** Frames end in a CRC-16/CCITT-FALSE. There is no table of
  rates: the board starts at 115,200 baud and makes any rate its UART divider gives within 2 %;
  `MIMHID_BAUD_FASTEST` measures the fastest rate the port, adapter and board carry without loss
  (counters on the board judge it). A UART BREAK returns a board to the boot rate, so a program
  that went away cannot lose it. `mimhid_device_link_counters` reads the board's error counters.
- **Versions.** The firmware and the library of one release agree on the protocol: a host talks to
  a board whose major and minor version equal its own. A board of another version is listed by
  `mimhid_discover` with only its version and refused with `MIMHID_ERR_PROTOCOL`.
- **Control transfers.** Control routing rules hand chosen requests to the program, which answers
  them (a proxy of a real device is built on this); Feature reports, idle and protocol requests,
  output reports and the other events are reported to the program.
- **Reports** are sent through a per-Node queue with optional loss reporting; batches commit a
  configuration change once.
- **Build and release.** CMake presets (`dev`, `release`, `tsan`), an ABI golden file, symbol
  lint, and release archives for Linux, macOS and Windows with the firmware images.
