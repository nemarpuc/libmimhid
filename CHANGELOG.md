# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html); until 1.0.0 a
change of the minor number may change the API, the ABI or the wire protocol.

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
