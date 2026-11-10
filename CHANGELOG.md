# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html); until 1.0.0 a
change of the minor number may change the API, the ABI or the wire protocol.

## [Unreleased]

## [0.2.1] - 2026-11-10

**Not hardware-verified.** The wire protocol and the C ABI are those of 0.2.0 (a 0.2.0 board and this library
talk to each other); this release fixes four bugs, changes the release packaging and adds tests and
documentation. None of it has run on a board. Asset names changed: a script that downloads the old
`-shared` / `-static` names must be changed.


### Fixed

- **Null options and configuration pointers are recorded in `mimhid_last_error`.** Passing a null
  `options` or `config` pointer to the nine `mimhid_spec_create_*` factories, `mimhid_device_open`,
  `mimhid_node_open`, `mimhid_node_open_raw`, or the calls taking `mimhid_commit_timeouts` now
  populates `mimhid_last_error()` with `MIMHID_ERR_PARAM` and the structure name instead of leaving
  the thread-local error detail at `MIMHID_OK`.
- **Frame type names.** `mimhid_frame_type_name` now returns `"PROBE"` for `MIMHID_T_PROBE`
  (`0x1C`) and `"COUNTERS"` for `MIMHID_T_COUNTERS` (`0x1D`).
- **3-byte detach `COMMIT` on close.** `mimhid_device_close` now sends the standard 3-byte
  `{1, 0, 0}` detach `COMMIT` payload matching `Link::disconnect` and `commit_device` instead of a
  1-byte `{1}` payload.
- **`device->closing` check across all device calls.** `mimhid_device_wait_ready`,
  `mimhid_device_remote_wakeup`, `mimhid_device_board_info`, `mimhid_device_link_counters`, and
  `mimhid_device_limits` now reject a closing `mimhid_device*` with `MIMHID_ERR_PARAM`, matching
  the other device operations.

### Changed

- **Tests.** A scripted board on a pseudo-terminal (`mimhid.scripted`) tests the transport and the C
  ABI when the board NAKs, stays silent, refuses a rate or hangs up; new unit suites cover every
  factory and input refusal. The pseudo-terminal tests of the real firmware core are now called
  sim (`mimhid.sim-*`). A Coverage CI job measures `src/` with gcovr. `tests/README.md` describes
  the unit, scripted, sim and hardware layers; `tests/hw/README.md` is the hardware procedure;
  `docs/VERIFICATION.md` summarizes what is verified across all four layers.
- **The release no longer publishes `*-runtime` bundles.** The `*-shared` archive already carries the
  shared library with the same license documents, and nothing in the project consumed the smaller
  bundle. A release now has 35 assets instead of 41.
- **One archive per platform, one SBOM per release; asset names change.** Each platform's archive
  is now `libmimhid-X.Y.Z-<platform>-<arch>[-ubuntu22.04].tar.gz|zip`, without the `-shared` /
  `-static` suffix, and holds both libraries (the static one is `libmimhid_static.a` /
  `mimhid_static.lib`), the CMake package files of both components and both pkg-config files;
  `find_package(mimhid COMPONENTS shared)` and `COMPONENTS static` both work from it. The twelve
  `.spdx.json` sidecars are replaced by `libmimhid-X.Y.Z.spdx.json`, one SPDX 2.3 document with a
  package per archive. A release has 18 assets instead of 35; a script that downloads the old
  names must be changed. The release workflow also gained a `workflow_dispatch` dry run
  (`dry_run`, on by default) that builds and checks every asset and keeps them as a workflow
  artifact without publishing anything.

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
