# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html); until 1.0.0 a
change of the minor number may change the API, the ABI or the wire protocol.

## [Unreleased]

**Version 0.3.0, not released. Not hardware-verified as `tests/hw/README.md` defines it; what was run
on a board is listed under "Run on a board" below.** The wire protocol and the C ABI change: a
0.2.1 board and this library do not talk to each other.

### Run on a board

2026-11-12, a Raspberry Pi Pico (RP2040), a Prolific PL2303 adapter, the target the same Linux
machine (a VMware guest, both USB devices passed through). Not run: a second machine as the target,
Windows or macOS, the `mimhid_verify_*` programs (they type into the target, which here is the
session), the other boards.

- Discovery and the rate search (7,812,500 baud, no errors on the link counters in any run).
- A keyboard profile Node: enumerated by Linux, 10 runs of 400 presses and 400 releases with the
  event device grabbed, none lost; latency in `docs/LATENCY.md`, not different from 0.2.1.
- A vendor Node with bulk IN and OUT: transfers of 1 to 292 bytes with their boundaries, a
  zero-length packet, 262,100 bytes without loss under the wait policy, 200 packets from the target,
  the drop-newest and drop-oldest policies with a target that does not read, stall and clear.
- An HID interface written by hand: every control rule action answered to the host (`ANSWER`,
  `ANSWER_LAST_SENT`, `ACCEPT` with a data stage, `ASK`, a stall for a request no rule matches), a
  rule's bytes replaced, and an Output report on the OUT endpoint.
- A composite device opened in one batch: a keyboard profile and a two-interface Node with an
  interface association.
- `mimhid_device_enter_bootloader`, used to write the firmware some forty times.

### A board a program left attached

- **The board leaves the bus before every configuration, the first of a Device too.** A program that
  ended without closing its Device (or closed it with `keep_attached`) leaves the board on the
  target's bus. The next program's first configuration used to reach a board that was attached:
  its descriptors changed under a device the target had already enumerated, the target never saw
  the new device, and `mimhid_device_wait_ready` timed out. Found on a Raspberry Pi Pico; the
  simulated board now enumerates only when the pull-up appears, as a real one does.

### Entering the bootloader

- **`mimhid_device_enter_bootloader`** (`BOOTLOADER`, `0x20`): the board takes its USB device off the
  target and restarts into the chip's own bootloader, so new firmware can be written without the
  BOOTSEL button. Run on a Raspberry Pi Pico (RP2040). The other ports have it too and were built
  but not run: the STM32F4 resets into its system memory bootloader, the ESP32-S3 into the ROM's
  download mode (written, not built: no ESP-IDF here), and the nRF52840 into the bootloader its
  board carries in flash, when the build states the value that bootloader looks for
  (`MIMHID_NRF_BOOTLOADER_GPREGRET`); without one the board answers `MIMHID_ERR_UNSUPPORTED`.

### Step 3a: no HID driver on the board

TinyUSB's HID driver is removed from the firmware. Every interface runs on the generic driver, and what
HID asks of endpoint 0 is a table of control rules that the host writes. The target sees the same
descriptors and the same answers for profile Nodes, apart from the differences listed under "Changed".

- **`mimhid_node_open` takes interfaces of any class, HID included.** A HID interface of the caller's own
  is class 0x03 with its HID descriptor in `class_descriptors` and its control rules in the new
  `mimhid_node_config.rules` / `rule_count` (`mimhid_node_config` is 56 bytes). Helpers:
  `mimhid_hid_descriptor_bytes`, `mimhid_hid_descriptor_rule`. New example `examples/c/hid_interface.c`.
- **Control rules replace control routing.** `mimhid_device_set_control_routing` is replaced by
  `mimhid_device_set_control_rules` and `mimhid_device_set_rule_data` (and `mimhid_node_set_rule_data`).
  `mimhid_control_rule` has an `action`: `MIMHID_CONTROL_ANSWER` (bytes from the board's memory),
  `_ANSWER_LAST_SENT` (the last transfer on an IN endpoint, optionally the last with a given first byte),
  `_ACCEPT` (takes the data stage), `_ASK` (the earlier hold: `timeout_ms`, `when_late` STALL or DROP) and
  `_STALL`. The rules of the Nodes come first, in node order, then the Device's. A request no rule matches
  is stalled apart from the standard requests of the stack. New flag `MIMHID_CONTROL_FLAG_ACCEPTED`.
- **Profile Nodes get their HID behaviour from rules the library writes** (`compose_hid_rules` in
  `src/usb/hid_layer.cpp`): `GET_DESCRIPTOR` of the report and HID descriptors, `GET_REPORT` of the last
  Input report (per Report ID) and of a Feature answer, `SET_REPORT`, `SET_IDLE`, `SET_PROTOCOL`, and the one-byte
  `GET_IDLE` and `GET_PROTOCOL`. `MIMHID_EVENT_OUTPUT_REPORT`, `_FEATURE_REPORT`, `_SET_IDLE` and
  `_SET_PROTOCOL` are made by the library from what the board reports; the idle rate and the protocol are put
  back to 0 and 1 at each mount.
- **Removed:** `mimhid_node_open_raw` and `mimhid_report_send` (use `mimhid_node_open` with class 0x03 and
  `mimhid_node_endpoint_write`; a profile Node's IN endpoint takes `mimhid_node_endpoint_write` and
  `mimhid_node_endpoint_set_policy` as well), `mimhid_device_set_control_routing`,
  `mimhid_board_limits.max_hid_interfaces` (replaced by `max_answer_bytes`, the bytes of every `ANSWER` rule
  together), `MIMHID_CONTROL_MAX_RULES`, `MIMHID_HID_ITFS` (the build states `MIMHID_MAX_ENDPOINT_NUMBER`
  only), `firmware/tests/itfs_probe.c` (now `port_probe.c`). The limits `max_interfaces` and
  `max_descriptor_bytes` of `mimhid_device_info` now mean `max_interfaces_any` and `max_answer_bytes`.
- **Wire:** `RULE` (`0x18`, was `CONTROL_CONFIG`), `RULES_APPLY` (`0x21`), `RULE_DATA` (`0x22`); the
  descriptor kind `ANSWER` (5) carries the bytes of an answer; `DESC_HID_REPORT` (4), `FEATURE` (`0x14`) and
  the events `SET_REPORT`, `SET_PROTOCOL`, `SET_IDLE`, `OUTPUT_REPORT` (5 to 8) are gone and not reused;
  `CONTROL_REPLY` keeps its actions; the event flag `0x10` (accepted). `PONG` bytes 5, 21..22 and 25..26 are
  reserved, zero; `max_answer_bytes` is at bytes 45..46 and the unique id moves to 47. The board's limits:
  160 rules, 4096 bytes of answers, 32 `ANSWER_LAST_SENT` rules (`MIMHID_CONTROL_RULES`,
  `MIMHID_ANSWER_BYTES`, `MIMHID_LAST_SENT_SLOTS`).
- **Firmware:** `CFG_TUD_HID` is 0. `control.c` holds two copies of the rule table and answers from them;
  `RULES_APPLY` swaps them and `RULE_DATA` changes the bytes of an answer in place, both answering `NAK_BUSY`
  while an answer is being sent. The ESP32-S3 port's `hid_device.c` is removed.
- **Changed for the target and the program:**
  - A Feature report read with no answer set is stalled (it was an empty answer).
  - `GET_REPORT` of an Input report returns the last report sent with that Report ID (it was the last report
    whatever its ID, stalled or empty when it had another).
  - A `GET_IDLE` straight after a `SET_IDLE` may still return the old value for one round trip.
  - `MIMHID_EVENT_OUTPUT_REPORT` / `_FEATURE_REPORT` for a `SET_REPORT` arrive when its data stage has, not at
    the acknowledgement.
  - Nodes on a shared interface that use the wait policy count against the first live Node of the interface.
  - A profile Node's IN endpoint takes any of the three policies (it was drop-newest only), and
    `mimhid_node_endpoint_write` splits a write longer than the board's frame limit and accepts a timeout of 0
    with a drop policy.
  - A rule given fewer bytes with `mimhid_*_set_rule_data` keeps that length when the table is built again.
  - A Device with no Node holds no rules: they are kept until the first commit.
  - Inside a batch a change of rules marks the Device for the commit at the end.
  - A `COMMIT` applies the staged rules with `report_all` 0; the library sends `RULES_APPLY` with
    `report_all` straight after an attach when it is set.
- Bindings regenerated (Python, C#, Rust: `GenericNodeRef.write` replaces the descriptor Node of the Rust
  crate); `tests/abi/abi_layout.golden` updated.

### Step 2: generic USB interfaces

- **`mimhid_node_open` takes interfaces of any class but HID** (until step 3a) (`mimhid_node_config`: interfaces, bulk
  and interrupt endpoints, class descriptors, an interface association). HID moved to
  `mimhid_node_open_profile` and `mimhid_node_open_raw` with `mimhid_hid_node_config`. The composer knows
  no class; `src/usb/hid_layer.cpp` makes the HID interfaces.
- **Endpoint calls:** `mimhid_node_endpoint_write`, `_stall`, `_clear_stall`, `_flush`, `_set_policy`
  (replacing `mimhid_node_set_queue_depth`); policies `MIMHID_FULL_DROP_NEWEST`, `_DROP_OLDEST`, `_WAIT`;
  `mimhid_device_add_string` / `_remove_string`. New events `MIMHID_EVENT_ENDPOINT_DATA`,
  `MIMHID_EVENT_ENDPOINT_HALT`, `MIMHID_EVENT_TRANSFER_LOST` (replacing `REPORT_LOST`; the event has
  `endpoint_address` and `interface_number`).
- **Wire:** `EP_STALL` (`0x1E`), `EP_FLUSH` (`0x1F`), `EP_OUT` (`0xC1`..`0xCF`), `EV_EP_DONE`,
  `EV_EP_HALT`; `PONG` gains `max_interfaces_any`, `max_config_descriptor_bytes`, `max_strings`,
  `max_transfer_bytes`, `endpoint_queue_slots`, `transfer_types` (bytes 36 to 44; the unique id moves to
  45). Interface names take the lowest free string index from 5 up.
- **Firmware:** a generic driver (`firmware/common/generic.c`) on TinyUSB 0.18.0's `usbd_edpt_*`.
- Versions 0.3.0 (CMake, Cargo, pyproject, csproj); the bindings are regenerated.

### Step 1 (earlier in this release)

### Changed

- **The host composes every USB descriptor and sends it to the board as bytes.** New frames
  `DESC_RESET` (`0x10`), `DESC_BEGIN` (`0x11`) and `DESC_DATA` (`0x12`) carry the device, configuration,
  string and HID report descriptors; they replace `DEVICE_CONFIG`, `NODE_BEGIN` and `NODE_DATA`.
  `src/usb/descriptor_composer.cpp` makes the descriptors, byte for byte what the firmware used to
  build. The firmware builds no descriptor: at `COMMIT` it walks the staged configuration once and
  refuses (`NAK_DESCRIPTOR`, the previous configuration kept) one that does not hold together
  ([docs/PROTOCOL.md](docs/PROTOCOL.md#the-commit-walk)). `mimhid_device_limits` still reports the room
  for report descriptors; the room for the configuration descriptor and the strings is
  `MIMHID_MAX_CONFIG_BYTES`, `MIMHID_MAX_STRINGS` and `MIMHID_MAX_STRING_BYTES` (`docs/LIMITS.md`).
- **Data frames name endpoints.** `EP_IN` frames (`0x41`..`0x4F`, formerly `REPORT_BASE`) carry the IN
  endpoint number, not a node id; `0x40` is refused. `FEATURE` names the interface number.
  `QUEUE_DEPTH` became `EP_CONFIG` (endpoint address, depth, policy; only
  `MIMHID_EP_FULL_DROP_NEWEST` is taken). The event `REPORT_LOST` (board to host) became
  `TRANSFER_LOST`, which names the endpoint address; `MIMHID_EVENT_REPORT_LOST` is unchanged for
  programs. `PONG` byte 4 is reserved, zero.
- **`iface.number` is the real `bInterfaceNumber`.** The numbers in use must be 0, 1, 2, ... without a
  gap; otherwise the commit fails with `MIMHID_ERR_PARAM` before anything is sent, with the missing
  number in `mimhid_error_detail.offset`. Before, the board renumbered the interfaces densely, so a
  gap closed and the number the target saw differed from `iface.number`. Open Nodes in number order
  (or in one batch) and close the highest number first.
- **The 16-Node limit is gone.** A Device holds up to 65534 Nodes within the board's interface and
  descriptor limits; `mimhid_node_id` is unchanged.
- **`mimhid_device_info.max_nodes` is removed** (the ABI layout changed; the bindings and the golden
  file are regenerated).
- **`MIMHID_NODE_DATA_CHUNK` is now `MIMHID_DESC_DATA_CHUNK`**, and `MIMHID_DEVFLAG_SERIAL_FROM_ID` is
  `MIMHID_DESC_F_SERIAL_FROM_ID` on a string.

### Added

- Tests: the composer against the descriptors captured from the previous firmware builder, the
  firmware's commit walk (every refusal), a repeated `DESC_DATA`, a serial number made from the chip
  id, a gap in the interface numbers, more than 16 Nodes, and the least queue depth of an interface.

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
