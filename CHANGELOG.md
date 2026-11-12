# Changelog

All notable changes to libmimusb are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html); until 1.0.0 a
change of the minor number may change the API, the ABI or the wire protocol.

## [Unreleased]

### Changed

- **An interface of a Node is a list of alternate settings, and a setting is a sequence of
  items** (`mimusb_usb_interface.settings`, `mimusb_usb_setting`, `mimusb_usb_item`): an endpoint
  or one whole descriptor, in bus order. This replaces `class_descriptors`, `endpoints` and
  `endpoint_count`, and takes class descriptors that sit between or after endpoints (CDC, audio,
  video, MIDI). `mimusb_usb_endpoint` is 12 bytes: it gains `audio_form`, `refresh`,
  `synch_address`, `sync_type` and `usage_type` for the 9-byte audio endpoint form and the
  isochronous synchronization and usage types.
- A Node can carry isochronous endpoints, in a setting other than 0; `INTERFACE_ALTERNATE` names
  the Node that owns the interface (`node_id`). Nodes and Devices given as bytes share one
  endpoint check.

- **Renamed from libmimhid to libmimusb**, because the library is no longer about HID alone. Every
  public name changes with it: the `mimhid_` prefix is `mimusb_`, `MIMHID_` is `MIMUSB_`, the
  headers are `mimusb.h` and `mimusb.hpp`, the CMake package is `mimusb` (targets `mimusb::mimusb`,
  `mimusb::mimusb_static`, `mimusb::cpp`), the pkg-config files are `mimusb.pc` and
  `mimusb-static.pc`, the CMake options are `MIMUSB_*`, the Python, Rust and C# packages are
  `mimusb`, `mimusb-sys` and `MimUsb`, and the repository is `nemarpuc/libmimusb`. The wire
  protocol and the firmware behaviour are unchanged; the names in `proto/mimusb_proto.h` change
  with the prefix. Code that uses 0.4.0 changes names only.

## [0.4.0] - 2026-11-12

**Not hardware-verified as `tests/hw/README.md` defines it; what was run on a board is listed under
"Run on a board" below.** The wire protocol changes (a new event, a new bit in the PONG's
`transfer_types`): a 0.3.x board and this library do not talk to each other, and the firmware of
this release is needed to use isochronous endpoints.

### Run on a board

2026-11-12, a Raspberry Pi Pico (RP2040), a Prolific PL2303 adapter, a Windows 10 machine as the
target. Not run: Linux, macOS or Android as the target, the RP2350, the other boards, audio
classes, an isochronous OUT endpoint, the `mimusb_verify_*` programs.

- A UVC camera (128x96 YUY2, 256-byte isochronous packets, descriptors as bytes) enumerated by
  Windows, opened by its camera application, and streamed: four openings and closings of the
  stream in a row, then a program restart with the stream open, three times, with no stop of the
  board and no error on the link counters.
- Before the fix under "Fixed", the second opening of the stream stopped the board (its USB core:
  commands that run there timed out while the serial link still answered) until it was unplugged.

### Added

- Alternate settings and isochronous endpoints in a Device given as bytes
  (`mimusb_device_set_descriptors`): interfaces may have settings 0, 1, 2, ... in order, an endpoint
  may be named by several settings of its interface, and a board whose `transfer_types` has
  `MIMUSB_TRANSFER_ISOCHRONOUS` (the RP2040 and RP2350 firmware: packets up to 1023 bytes) takes
  isochronous endpoints, which is what the audio and video classes stream on. Endpoint descriptors
  may be 7 bytes or the 9-byte audio-class form. The target's `SET_INTERFACE` switches the
  isochronous endpoints of the interface and is reported as `MIMUSB_EVENT_INTERFACE_ALTERNATE`
  (wire event `SET_INTERFACE`, 14); `GET_INTERFACE` answers the selected setting. The board adds
  nothing to a stream: no padding, no pacing, no resampling.
- `mimusb_device_endpoint_write` refuses an isochronous transfer longer than the endpoint's packet.
- Simulated-board scenario `mimusb.sim-iso`, host walker and firmware tests for settings and
  isochronous descriptors.

### Fixed

- A target that leaves an alternate setting with an isochronous endpoint and selects it again (a
  camera application closed and opened again) stopped the RP2040 and RP2350 board: the transfer
  armed on the controller was never taken by the target, and TinyUSB's driver stops with "ep was
  already available" when the next one is armed on top of it. `SET_INTERFACE` now takes the armed
  transfer off the controller of every isochronous endpoint it stops or starts
  (`hal_usb_clear_endpoint`). Seen on a board: four openings and closings of a UVC stream in a row,
  and a detach while streaming followed by three attaches, with no stop; before, the second opening
  stopped the board until it was unplugged.

### Changed

- A Node still has bulk and interrupt endpoints only: an isochronous `transfer_type` in a Node is
  `MIMUSB_ERR_UNSUPPORTED` (it needs settings, which a Device given as bytes has).
- On a board with isochronous endpoints `max_transfer_bytes` is its isochronous packet (1023, 64
  before), so a long write to a bulk endpoint goes in frames of up to 960 bytes (a multiple of the
  endpoint's packet size) instead of 64; the target sees the same transfer.
- A configuration for the RP2040 and RP2350 is checked against the controller's 3712 bytes of
  packet memory at `COMMIT` (`NAK_OVERFLOW`): the driver stopped the board when it ran out.
- The wire protocol gains an event and the PONG's `transfer_types` a bit; the firmware version
  number is not changed here (the release's to choose).

## [0.3.0] - 2026-11-12

**Not hardware-verified as `tests/hw/README.md` defines it; what was run
on a board is listed under "Run on a board" below.** The wire protocol and the C ABI change: a
0.2.1 board and this library do not talk to each other. The board becomes a generic USB device: the
host composes every descriptor, or is given them as bytes, and the firmware has no HID driver.

### Run on a board

2026-11-12, a Raspberry Pi Pico (RP2040), a Prolific PL2303 adapter, the target the same Linux
machine (a VMware guest, both USB devices passed through). Not run: a second machine as the target,
Windows or macOS, the `mimusb_verify_*` programs (they type into the target, which here is the
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
- A device given as bytes (`tests/hw/hw_bytes bytes`): the device, configuration and BOS descriptors and
  two strings read back by Linux byte for byte, the vendor request answered by a rule of the Device,
  bulk IN with its boundaries and a zero-length packet, bulk OUT as events of node 0 (a transfer of 70
  bytes is two packets), Halt and clear. A composed Device with a BOS descriptor and `bcdUSB` 0x0210
  (`hw_bytes bos`). The BOS of both names the Microsoft OS 2.0 capability, so the target asks for its
  descriptor set and the Device has a rule that answers; without it, or with `bcdUSB` 0x0210 and no BOS,
  the target did not enumerate the device. Not run: Windows.
- `hw_bulk wait|newest|oldest`, `hw_control_rules` and `hw_composite` again on the same firmware; the
  latency of ten runs in `docs/LATENCY.md`.
- `mimusb_device_enter_bootloader`, used to write the firmware some forty times.

### Wire protocol

- **The host sends every descriptor as bytes.** `DESC_RESET` (`0x10`), `DESC_BEGIN` (`0x11`) and
  `DESC_DATA` (`0x12`) carry the device, configuration, string, BOS and `ANSWER` descriptors
  (`MIMUSB_DESC_*`); they replace `DEVICE_CONFIG`, `NODE_BEGIN` and `NODE_DATA`. At `COMMIT` the board
  walks the staged configuration once and refuses one that does not hold together (`NAK_DESCRIPTOR`, the
  previous configuration kept; [docs/PROTOCOL.md](docs/PROTOCOL.md#the-commit-walk)).
- **A BOS descriptor** is a descriptor kind (`MIMUSB_DESC_BOS`, 6) the board serves byte for byte; it needs
  `bcdUSB` 0x0201 or more.
- **Data frames name endpoints.** `EP_IN` (`0x41`..`0x4F`) and `EP_OUT` (`0xC1`..`0xCF`) carry the endpoint
  number, not a node id. New commands `EP_CONFIG` (`0x1B`, replaces `QUEUE_DEPTH`: address, depth, policy
  drop-newest, drop-oldest or wait), `EP_STALL` (`0x1E`), `EP_FLUSH` (`0x1F`), `RULE` (`0x18`, was
  `CONTROL_CONFIG`), `RULES_APPLY` (`0x21`), `RULE_DATA` (`0x22`), `BOOTLOADER` (`0x20`); new events
  `EV_EP_DONE`, `EV_EP_HALT`; `REPORT_LOST` became `TRANSFER_LOST`, which names the endpoint.
  `DESC_HID_REPORT` (4), `FEATURE` (`0x14`) and the events `SET_REPORT`, `SET_PROTOCOL`, `SET_IDLE`,
  `OUTPUT_REPORT` (5 to 8) are gone and not reused.
- **An `ANSWER` rule has a capacity and a length** (`RULE` bytes 14..15 and 16..17). The board reserves the
  capacity, `RULE_DATA` sets any length up to it, so a shortened answer can grow again after the table
  is rebuilt.
- **A `COMMIT` that attaches is refused with `NAK_STATE` while the board is attached**: it would change
  the descriptors under a device the target has enumerated. `mimusb_device_connect` on a configured
  Device sends nothing and returns `MIMUSB_OK`.
- **`PONG`** (`MIMUSB_PONG_MIN_LEN` 49): bytes 4, 5, 21..22 and 25..26 are reserved, zero; new fields
  `max_endpoint_number` (34), `max_interfaces_any`, `max_config_descriptor_bytes`, `max_strings`,
  `max_transfer_bytes`, `endpoint_queue_slots`, `transfer_types` (36 to 44), `max_answer_bytes` (45..46)
  and `max_bos_bytes` (47..48); the unique id starts at 49. The board's limits: 160 rules, 4096 bytes of
  answers, 32 `ANSWER_LAST_SENT` rules, 256 bytes of BOS (`MIMUSB_CONTROL_RULES`, `MIMUSB_ANSWER_BYTES`,
  `MIMUSB_LAST_SENT_SLOTS`, `MIMUSB_MAX_BOS_BYTES`).

### API

- **`mimusb_node_open` takes interfaces of any class, HID included** (`mimusb_node_config`: interfaces,
  bulk and interrupt endpoints, class descriptors, an interface association, control rules). Profile
  Nodes (keyboard, mouse, ...) are `mimusb_node_open_profile` with `mimusb_hid_node_config`. Helpers
  `mimusb_hid_descriptor_bytes` and `mimusb_hid_descriptor_rule`; examples `vendor_bulk.c` and
  `hid_interface.c`.
- **A Device can be given as bytes:** `mimusb_device_set_descriptors` / `_clear_descriptors`
  (`mimusb_device_descriptors`), raw strings `mimusb_device_set_string` / `_clear_string` (any index and
  language), and the endpoint calls by address `mimusb_device_endpoint_set_policy`, `_write`, `_stall`,
  `_clear_stall`, `_flush`. The library checks the bytes without reading any class and names the field
  and the byte offset of what is wrong. The OUT data of such a device is `MIMUSB_EVENT_ENDPOINT_DATA`
  with node id 0. It is exclusive with Nodes (`MIMUSB_ERR_BUSY` both ways). Example
  `examples/c/device_from_descriptors.c`.
- **BOS descriptor:** `mimusb_device_set_bos` for a composed Device, or in `mimusb_device_descriptors`;
  `mimusb_board_limits.max_bos_bytes`. A commit with a BOS and a `bcd_usb` below 0x0201 is
  `MIMUSB_ERR_PARAM`. Example `examples/c/winusb_bulk.c` (the Microsoft OS 2.0 platform capability and
  the vendor request that answers it): **not run on Windows**.
- **Control rules replace control routing:** `mimusb_device_set_control_rules`, `mimusb_device_set_rule_data`,
  `mimusb_node_set_rule_data`; actions `MIMUSB_CONTROL_ANSWER`, `_ANSWER_LAST_SENT`, `_ACCEPT`, `_ASK`
  (`timeout_ms`, `when_late`), `_STALL`; the rules of the Nodes come first, then the Device's. A request
  no rule matches is stalled apart from the standard requests of the stack. New flag
  `MIMUSB_CONTROL_FLAG_ACCEPTED`. `mimusb_*_set_rule_data` takes any length up to the rule's capacity.
- **Endpoint calls of a Node:** `mimusb_node_endpoint_write`, `_stall`, `_clear_stall`, `_flush`,
  `_set_policy` (replacing `mimusb_node_set_queue_depth`); a profile Node's IN endpoint takes them too, with
  any of the three policies; a write longer than the board's frame limit is split, and a timeout of 0 is
  accepted with a drop policy. `mimusb_device_add_string` / `_remove_string`. New events
  `MIMUSB_EVENT_ENDPOINT_DATA`, `_ENDPOINT_HALT`, `_TRANSFER_LOST` (replacing `REPORT_LOST`).
- **`iface.number` is the real `bInterfaceNumber`.** The numbers in use are 0, 1, 2, ... without a gap;
  otherwise the commit fails with `MIMUSB_ERR_PARAM` before anything is sent, the missing number in
  `mimusb_error_detail.offset`. Open Nodes in number order (or in one batch) and close the highest first.
- **Limits:** the 16-Node limit is gone (up to 65534 within the board's limits);
  `mimusb_device_info.max_nodes` and `mimusb_board_limits.max_hid_interfaces` are removed
  (`max_answer_bytes` and the new fields replace them).
- **Removed:** `mimusb_node_open_raw`, `mimusb_report_send`, `mimusb_device_set_control_routing`,
  `MIMUSB_CONTROL_MAX_RULES`, `MIMUSB_HID_ITFS`, `MIMUSB_NODE_DATA_CHUNK` (now `MIMUSB_DESC_DATA_CHUNK`),
  `MIMUSB_DEVFLAG_SERIAL_FROM_ID` (now `MIMUSB_DESC_F_SERIAL_FROM_ID`).
- **Changed for the target and the program:**
  - A Feature report read with no answer set is stalled (it was an empty answer).
  - `GET_REPORT` of an Input report returns the last report sent with that Report ID.
  - A `GET_IDLE` straight after a `SET_IDLE` may still return the old value for one round trip.
  - `MIMUSB_EVENT_OUTPUT_REPORT` / `_FEATURE_REPORT` for a `SET_REPORT` arrive when its data stage has.
  - Nodes on a shared interface that use the wait policy count against the first live Node of it.
  - A Device with no Node holds no rules: they are kept until the first commit; inside a batch a change
    of rules marks the Device for the commit at the end.
  - A `COMMIT` applies the staged rules with `report_all` 0; the library sends `RULES_APPLY` with it
    straight after an attach when it is set.

### Firmware

- **The firmware builds no descriptor and has no HID driver** (`CFG_TUD_HID` is 0). Every interface runs
  on a generic driver (`firmware/common/generic.c`, TinyUSB 0.18.0's `usbd_edpt_*`); what HID asks of
  endpoint 0 is a table of control rules (`control.c`: two copies, `RULES_APPLY` swaps them and
  `RULE_DATA` changes an answer in place, both `NAK_BUSY` while an answer is being sent). The ESP32-S3
  port's `hid_device.c` is removed.
- A BOS descriptor is served by `tud_descriptor_bos_cb`; none is a stall.
- `firmware/tests/itfs_probe.c` is now `port_probe.c`.

### A board a program left attached

- **The board leaves the bus before every configuration, the first of a Device too.** A program that
  ended without closing its Device (or closed it with `keep_attached`) leaves the board on the
  target's bus. The next program's first configuration used to reach a board that was attached:
  its descriptors changed under a device the target had already enumerated, the target never saw
  the new device, and `mimusb_device_wait_ready` timed out. Found on a Raspberry Pi Pico; the
  simulated board now enumerates only when the pull-up appears, as a real one does.

### Entering the bootloader

- **`mimusb_device_enter_bootloader`** (`BOOTLOADER`, `0x20`): the board takes its USB device off the
  target and restarts into the chip's own bootloader, so new firmware can be written without the
  BOOTSEL button. Run on a Raspberry Pi Pico (RP2040). The other ports have it too and were built
  but not run: the STM32F4 resets into its system memory bootloader, the ESP32-S3 into the ROM's
  download mode (written, not built: no ESP-IDF here), and the nRF52840 into the bootloader its
  board carries in flash, when the build states the value that bootloader looks for
  (`MIMUSB_NRF_BOOTLOADER_GPREGRET`); without one the board answers `MIMUSB_ERR_UNSUPPORTED`.

### Bindings, tests and documentation

- Bindings regenerated (Python, C#, Rust: `GenericNodeRef.write` replaces the descriptor Node of the Rust
  crate); `tests/abi/abi_layout.golden` updated; versions 0.3.0 (CMake, Cargo, pyproject, csproj).
- Tests: the composer against the descriptors captured from the previous firmware builder, the
  descriptor walker (every refusal and its offset), the firmware's commit walk, the byte device and BOS
  against the simulated board, a repeated `DESC_DATA`, a serial number made from the chip id, a gap in the
  interface numbers, more than 16 Nodes, the least queue depth of an interface, an answer that shrinks and
  grows again, an attach refused while attached, and the loss counts of a held endpoint.
- Documentation read against the code: node and interface limits, interface numbering, queue depths, the
  removed calls, and the descriptors a board serves ([docs/LIMITS.md](docs/LIMITS.md)).

## [0.2.1] - 2026-11-10

**Not hardware-verified.** The wire protocol and the C ABI are those of 0.2.0 (a 0.2.0 board and this library
talk to each other); this release fixes four bugs, changes the release packaging and adds tests and
documentation. None of it has run on a board. Asset names changed: a script that downloads the old
`-shared` / `-static` names must be changed.


### Fixed

- **Null options and configuration pointers are recorded in `mimusb_last_error`.** Passing a null
  `options` or `config` pointer to the nine `mimusb_spec_create_*` factories, `mimusb_device_open`,
  `mimusb_node_open`, `mimusb_node_open_raw`, or the calls taking `mimusb_commit_timeouts` now
  populates `mimusb_last_error()` with `MIMUSB_ERR_PARAM` and the structure name instead of leaving
  the thread-local error detail at `MIMUSB_OK`.
- **Frame type names.** `mimusb_frame_type_name` now returns `"PROBE"` for `MIMUSB_T_PROBE`
  (`0x1C`) and `"COUNTERS"` for `MIMUSB_T_COUNTERS` (`0x1D`).
- **3-byte detach `COMMIT` on close.** `mimusb_device_close` now sends the standard 3-byte
  `{1, 0, 0}` detach `COMMIT` payload matching `Link::disconnect` and `commit_device` instead of a
  1-byte `{1}` payload.
- **`device->closing` check across all device calls.** `mimusb_device_wait_ready`,
  `mimusb_device_remote_wakeup`, `mimusb_device_board_info`, `mimusb_device_link_counters`, and
  `mimusb_device_limits` now reject a closing `mimusb_device*` with `MIMUSB_ERR_PARAM`, matching
  the other device operations.

### Changed

- **Tests.** A scripted board on a pseudo-terminal (`mimusb.scripted`) tests the transport and the C
  ABI when the board NAKs, stays silent, refuses a rate or hangs up; new unit suites cover every
  factory and input refusal. The pseudo-terminal tests of the real firmware core are now called
  sim (`mimusb.sim-*`). A Coverage CI job measures `src/` with gcovr. `tests/README.md` describes
  the unit, scripted, sim and hardware layers; `tests/hw/README.md` is the hardware procedure;
  `docs/VERIFICATION.md` summarizes what is verified across all four layers.
- **The release no longer publishes `*-runtime` bundles.** The `*-shared` archive already carries the
  shared library with the same license documents, and nothing in the project consumed the smaller
  bundle. A release now has 35 assets instead of 41.
- **One archive per platform, one SBOM per release; asset names change.** Each platform's archive
  is now `libmimusb-X.Y.Z-<platform>-<arch>[-ubuntu22.04].tar.gz|zip`, without the `-shared` /
  `-static` suffix, and holds both libraries (the static one is `libmimusb_static.a` /
  `mimusb_static.lib`), the CMake package files of both components and both pkg-config files;
  `find_package(mimusb COMPONENTS shared)` and `COMPONENTS static` both work from it. The twelve
  `.spdx.json` sidecars are replaced by `libmimusb-X.Y.Z.spdx.json`, one SPDX 2.3 document with a
  package per archive. A release has 18 assets instead of 35; a script that downloads the old
  names must be changed. The release workflow also gained a `workflow_dispatch` dry run
  (`dry_run`, on by default) that builds and checks every asset and keeps them as a workflow
  artifact without publishing anything.

## [0.2.0] - 2026-11-08

The wire protocol, the C ABI and the firmware build interface change; a 0.1.0 board and a 0.2.0
library do not talk to each other (`MIMUSB_ERR_PROTOCOL`), and the reverse. **Not
hardware-verified**: the changes are tested with the firmware core's own tests, with the simulated
board over a pseudo-terminal, and by building every port (RP2040, RP2350, nRF52840, STM32F4;
the ESP32-S3 build is left to CI); none of it has run on a board yet.

- **Nothing about the chip is assumed by the firmware build.** Each port states the interfaces
  (`MIMUSB_HID_ITFS`), the highest endpoint number (`MIMUSB_MAX_ENDPOINT_NUMBER`), the endpoint
  packet size (`MIMUSB_MAX_ENDPOINT_BYTES`), `CFG_TUSB_OS`, `CFG_TUSB_RHPORT0_MODE` and
  `CFG_TUD_MAX_SPEED` on the command line; a missing one is a compile error that names it. The
  nRF52840 no longer gets 7 by default, `core.h` no longer picks 15 for the RP chips from
  `PICO_RP2040`, and `tusb_config.h` no longer defaults to full speed and no OS. The Pico build
  needs `PICO_PLATFORM`, `PICO_BOARD`, `MIMUSB_UART_ID`, `MIMUSB_UART_TX_PIN` and
  `MIMUSB_UART_RX_PIN`; the ESP32-S3 build both pins; the nRF52840 and STM32F4 builds `-DBOARD`
  (the STM32F4 port records the endpoint count of each board's MCU and refuses an unlisted board,
  and gives the F412 boards 5 interfaces, not 3).
- **A number the controller does not have is refused.** Until now an endpoint address such as
  `0x8F` was accepted by the host and the board, TinyUSB then failed the target's
  SET_CONFIGURATION, and the commit ended in `MIMUSB_ERR_TIMEOUT` (on the nRF52840 the endpoint
  number 8 was opened as a corrupt isochronous endpoint without an error). The PONG now carries
  `max_endpoint_number` (byte 34; the unique id moved to 35 and 36..; `MIMUSB_PONG_MIN_LEN` 36),
  `mimusb_device_info.reserved8` became `max_endpoint_number`, `mimusb_board_limits` gained
  `max_endpoint_number` (its size is 36), and `mimusb_node_open` returns `MIMUSB_ERR_OVERFLOW`
  for a larger number. The firmware refuses it with `NAK_OVERFLOW`.
- **The firmware fills nothing in.** A zero LANGID, `bcdUSB`, `bcdHID`, IN endpoint address, size
  or interval, an `bmAttributes` without bit 7, and an OUT endpoint that is only partly given are
  refused (`NAK_PARAM`); a COMMIT whose nodes of one interface differ, or that uses an endpoint
  number twice in one direction, is refused (`NAK_DESCRIPTOR`). The host library already
  required all of these, so a program that works with the library is not affected.
- **The initializers fill nothing in.** Every `mimusb_*_init` function (and its `mimusb::` twin)
  zeroes the structure and sets `struct_size`, and nothing else: the pid.codes test IDs, the
  strings, the boot rate, the handshake and commit timeouts, the report size, the Linux HID limits,
  `bm_attributes`, the node's class, `bcdHID`, endpoint and interval, and every profile's fields
  (the mouse buttons and ranges, the keyboard Usages, the gamepad axes, the touch and pen
  fields) are the program's to set, and a required one left at zero is refused with
  `MIMUSB_ERR_UNSET_FIELD`. `mimusb_node_config_init` and `mimusb::node_config()` lost their
  `interface_number` argument (set `iface.number` and `ep_in.address` yourself), and the four
  constants `MIMUSB_CONTROL_TIMEOUT_MS`, `MIMUSB_SEND_TIMEOUT_MS`, `MIMUSB_DRAIN_TIMEOUT_MS` and
  `MIMUSB_RECONNECT_TIMEOUT_MS` are gone. The README, `docs/API.md` (which lists what each
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
  `include/mimusb.h`, header-only C++ forwarders in `mimusb.hpp`, Python, C# and Rust bindings) and
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
  `MIMUSB_BAUD_FASTEST` measures the fastest rate the port, adapter and board carry without loss
  (counters on the board judge it). A UART BREAK returns a board to the boot rate, so a program
  that went away cannot lose it. `mimusb_device_link_counters` reads the board's error counters.
- **Versions.** The firmware and the library of one release agree on the protocol: a host talks to
  a board whose major and minor version equal its own. A board of another version is listed by
  `mimusb_discover` with only its version and refused with `MIMUSB_ERR_PROTOCOL`.
- **Control transfers.** Control routing rules hand chosen requests to the program, which answers
  them (a proxy of a real device is built on this); Feature reports, idle and protocol requests,
  output reports and the other events are reported to the program.
- **Reports** are sent through a per-Node queue with optional loss reporting; batches commit a
  configuration change once.
- **Build and release.** CMake presets (`dev`, `release`, `tsan`), an ABI golden file, symbol
  lint, and release archives for Linux, macOS and Windows with the firmware images.
