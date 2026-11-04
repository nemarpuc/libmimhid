# Changelog

All notable changes to libmimhid are recorded in this file.
This project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.6] - 2026-11-04

A device can present vendor ID 0. The wire protocol stays 6 and the firmware is unchanged; the size of `mimhid_usb_device` stays 48.

### Added

- `mimhid_usb_device.identity_set` (offset 44, in what was padding). USB 2.0 9.6.1 reserves no `idVendor` or `idProduct` value, but a zero `vendor_id` was refused because zero is also what an unset field holds. With `identity_set` = 1, `vendor_id` 0 is presented as given. `mimhid_device_config_init` leaves it 0, so a program that never set `vendor_id` is still refused with `MIMHID_ERR_UNSET_FIELD`, and a nonzero `vendor_id` needs no flag. `product_id` 0 was always accepted. A value other than 0 or 1 is `MIMHID_ERR_PARAM`.
- The Rust, Python and C# bindings carry the field; the layout checks and the ABI golden list it.

### Tests

- A device presenting vendor ID 0 and product ID 0 (the Device descriptor read back), and `identity_set` 2 refused.

## [2.0.5] - 2026-11-03

What the board answers on its own, where no control-routing rule holds the request, now follows the specifications in two more places. The wire protocol stays 6 and the host library is unchanged; reflash the firmware to get the new answers.

### Fixed

- `GET_STATUS` to an interface of the configuration returns two bytes of zero (USB 2.0 9.4.5, figure 9-5). The stack's HID driver does not answer it, so the board used to STALL a request every device must serve. It answers when the board is configured, the interface exists and `wLength` is 2; any other case is left to the stack.
- `GET_REPORT` for an Input report returns the last report sent on that interface (HID 1.11 7.2 makes `GET_REPORT` mandatory, 7.2.1 gives the answer the format of the interrupt reports). With a Report ID the report must carry that ID; an interface that has sent nothing since the configuration was committed gives no data, as before, because the board does not invent a report. It used to answer with the Report ID alone, or stall.

### Documentation

- `docs/LIMITS.md` lists who answers each control request without a rule (the program's configuration, the stack, or a STALL) and what a rule changes, and says that a rule with every mask zero hands all requests but `SET_ADDRESS` and `SET_CONFIGURATION(1)` to the program.
- `GET_DESCRIPTOR` for the BOS descriptor is not answered by the board. USB 3.2 9.2.6.6 requires it from `bcd_usb` 0x0210; `LIMITS.md` and `mimhid.h` say that such a device needs a rule that answers it, measured with 0x0210, 0x0300 and 0x0320 behind a Windows host and a Linux guest. The speed a target sees is measured by the host from the signalling, not taken from `bcd_usb`.
- `SET_INTERFACE` is acknowledged by the stack whatever the setting; the earlier notes that said it stalls were wrong.

### Tests

- Firmware: the interface status in every case it answers and every case it leaves to the stack, its rule taking precedence, and `GET_REPORT` of an Input report (last report, Report ID, `wLength`, another interface, and forgetting it on a new configuration).

## [2.0.4] - 2026-11-03

Limits brought to what the specifications and the chips allow, and values the board cannot present refused where they are given. The wire protocol stays 6; the firmware must be reflashed for the new limits (a host library newer than the firmware takes the smaller numbers the board reports).

### Changed

- `MIMHID_MAX_NODES` is 16, what a report frame can name (four bits of its type); it was 8.
- The Raspberry Pi Pico and Pico 2 firmware has 15 HID interfaces (`MIMHID_HID_ITFS`), the endpoint numbers the RP2040 and RP2350 controllers have (RP2040 datasheet 4.1.1.1.1, RP2350 datasheet 12.7.1.1.1); it was 7. Built for both chips; run on no board yet.
- `MIMHID_CONTROL_MAX_TIMEOUT_MS` and `MIMHID_CONTROL_TIMEOUT_MAX` are 5000, the 5 seconds USB 2.0 9.2.6.1 gives any command; they were 4000.
- A node with an interface class other than HID (`0x03`) is refused with `MIMHID_ERR_UNSUPPORTED` (the board answers NAK `UNSUPPORTED`); the board's USB stack opens HID interfaces only, and such a node used to be accepted and left the device unconfigured.
- A `configuration_value` other than 1 is refused with `MIMHID_ERR_UNSUPPORTED`; the board has one configuration, and the stack selects it with `SET_CONFIGURATION(1)` only.
- `iface.subclass` above 1 and `iface.protocol` above 2 are refused by the host library (`MIMHID_ERR_PARAM`), as the firmware already did (HID 1.11 4.2, 4.3). The firmware no longer refuses subclass 1 with protocol 0, which HID 1.11 4.3 lists as "None".
- Endpoint addresses are checked as USB 2.0 9.6.6 defines them (the header and docs said `0x81` to `0x8F` and `0x01` to `0x0F` but the library only looked at bit 7): the number is 1 to 15 and bits 6..4 are zero. `bm_attributes` bits 4..0 must be zero (9.6.3) and `device_subclass` must be zero under `device_class` 0 (9.6.1). A node whose interface number is beyond the board's interfaces is reported as `MIMHID_ERR_OVERFLOW` before its addresses are looked at.
- On a high-speed board `ep0_max_packet_size` must be 64 (USB 2.0 9.6.1) and a poll interval 1 to 16 (USB 2.0 9.6.6).

### Documentation

- `docs/LIMITS.md` names the source of every number: a USB-IF specification section, a chip datasheet section, or a project choice. It also says what the board does not answer by itself (Input and Output `GET_REPORT`, `SET_IDLE` repeats, the boot protocol) and how a control-routing rule hands those to the program.

### Tests

- Firmware: 15 interfaces, a high-speed build, 16 nodes, the new refusals, and the interface count seen in both include orders for the RP2040, the RP2350 and any other chip.
- End to end: 15 interfaces and a high-speed board.

## [2.0.3] - 2026-11-02

Not hardware-verified: the host library's ABI and the wire protocol (still 6) are those
of 2.0.2, and the firmware changes only its version number. The changes are in the build
files, the CMake package, the tests, the tooling and the udev rules file; the build
layout is now the one libaoahid has.

### Added

- `mimhid::cpp` and `mimhid::cpp_static`: the C++ facade of `mimhid.hpp` as a CMake target that asks for C++20, so a project that enables only C can still link `mimhid::mimhid`.
- `MIMHID_SANITIZE` (AddressSanitizer and UBSan) and `MIMHID_TSAN` (ThreadSanitizer), with the configure-time checks libaoahid has: Linux only, GCC 12 or Clang, a usable runtime, PIE. Findings stop the process (`-fno-sanitize-recover=all`).
- The `tsan` CMake preset.
- `mimhid.abi-layout-oracle`: every size, field offset and constant of the C ABI (441 entries) is compared with `tests/abi/abi_layout.golden`, so an ABI change fails a test until the golden file is edited on purpose. Unlike libaoahid's hand-written schema, this one is a recorded snapshot of the 2.0.2 header.
- `tools/check_input_provenance.py`, run by CI: every `MIMHID_*` build option that the README, CONTRIBUTING and the docs name (in a `-D` argument or, for option-shaped names, in code quotes) must be declared by the CMake files. A misspelled or removed option is otherwise accepted by CMake as an unused cache entry.
- `mimhid.package-multiarch-layout` and `mimhid.package-component-contract`: a multiarch libdir must reach the `.pc` files and the package config, and `find_package(mimhid COMPONENTS shared|static)` must refuse a component that was not built. Ported from libaoahid's tests of the same names, minus the libusb parts libmimhid has no use for.
- A Linux CI step installs the library, moves the install tree and builds a program through `pkg-config` (static and shared), which is what the relocatable `prefix=${pcfiledir}/../..` of the `.pc` files promises.

### Changed

- `MIMHID_BUILD_TESTS` and `MIMHID_BUILD_EXAMPLES` are off by default, so a project that takes libmimhid with `add_subdirectory` or `FetchContent` no longer builds them. The `dev` and `tsan` presets and every CI job turn them on.
- `MIMHID_WERROR` is on by default, tests and the ABI check included; CI also builds Windows with it.
- The CMake presets are `dev`, `release` and `tsan` (as in libaoahid), at preset version 2, which is what `cmake_minimum_required 3.20` can read. `debug` and `test` are gone.
- The linker version script is `cmake/mimhid.map` (was `mimhid.ver`). The shared objects are compiled only when the shared library is built.
- The CI job `host` is `native`.

### Fixed

- The udev rules file is `udev/51-mimhid.rules` (was `99-mimhid.rules`). `TAG+="uaccess"` only takes effect when set before `73-seat-late.rules`, where logind's uaccess builtin runs, so under the old number only the `MODE`/`GROUP` half worked and a user outside `uucp`/`dialout` got no access to the port. Remove an installed `/etc/udev/rules.d/99-mimhid.rules` when you install the new file.
- The Windows port listing and `mimhid_device_info` fill their text with `memcpy` instead of `strcpy` and `strncpy`, which MSVC reports as C4996 and `MIMHID_WERROR` turns into an error.
- `README.md` no longer carries a note about the 2.0.0 API change; `docs/TARGET_MATRIX.md` drops the version label of its control-transfer section. The history is this file.

### Removed

- `MIMHID_SANITIZERS`: use `MIMHID_SANITIZE` or `MIMHID_TSAN`.

## [2.0.2] - 2026-11-02

Not hardware-verified: the host library, the firmware and the wire protocol are
those of 2.0.1; the version number of the firmware image changes with the release
and nothing else in it. The additions are examples, bindings, tests and tooling,
and one change of code layout in `src/` that keeps the behavior (see Changed).

### Added

- `examples/c/profiles/`: one program per profile kind and one for the battery report. None opens a board; each builds a Spec, checks its manifest against the descriptor, and prints the call sequence that would send it. CTest runs them as `mimhid.example-profile-*`.
- `examples/c/verify/`: five programs (`keyboard`, `mouse`, `touch`, `toggle`, `all`) to run with a board and watch on the target. Built as `mimhid_verify_*`, never registered as CTest cases. `docs/QUICKSTART.md` has a step for them.
- `examples/README.md`: the commands to compile every example against a release archive.
- The `shared` and `static` release archives carry `share/doc/libmimhid/examples/`; the runtime bundles do not. `package_release.py` copies it and `validate_layout` requires it.
- `MIMHID_BUILD_FUZZ`: a libFuzzer target (`tests/fuzz/fuzz_descriptor.cpp`) for the descriptor builder, the serializer, the raw-descriptor validator and the frame parser, and a `fuzz-smoke` CI job.
- Language bindings for Python (`ctypes`), C# (P/Invoke) and Rust (FFI) in `bindings/`, generated from `include/mimhid.h` by `tools/bindings/generate.py`, with a C layout oracle (`tests/abi/layout_oracle.c`), a layout test per language and a short `enumerate` example each. The release workflow builds and checks the wheel and the NuGet package (reproducibly, with no native library inside) and keeps them; they are published to PyPI and NuGet only when the repository variable `MIMHID_PUBLISH_BINDINGS` is `true`. The package versions are checked against the tag. The Rust crate `mimhid` wraps `mimhid-sys` with owning Spec types and one node type per profile. C# enum members drop the `ERR_` of `Result` (`Result.Parameter`, as in libaoahid).
- `tools/release/sync_version.py` and `version_files.py`: one list of the files that carry the version, written by one command and checked by `validate_release.py`.
- `docs/INTERNALS.md`, `tools/docs/Doxyfile` with a `docs.yml` workflow that builds the API documentation, and a bug report issue template.

### Changed

- `Link::handle_frame` tests the two output-report event kinds in one condition; the behavior is the same. `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` marks the functions whose parameters follow the wire or the C ABI order. CI builds with Clang 14 and runs clang-tidy 14 on the production sources, and has `musl` and `hid-tools` jobs.

## [2.0.1] - 2026-10-30

Not run on a board: the host library's ABI and the wire protocol (still 6) are those of 2.0.0, and the firmware changes only its version number, so a board on 2.0.0 or 1.5.0 firmware works with this library. Tested by the unit, ABI and end-to-end tests, also under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

### Fixed

- **The frame parser takes only the frames a board sends.** It also took frame types 5 and 8, the numbers of the event kinds `MIMHID_EV_SET_REPORT` and `MIMHID_EV_OUTPUT_REPORT`, which travel inside an `EVENT` frame and are no frame types. No firmware sends such a frame, so this code never ran, but noise on the line that happened to form one was delivered as an event. New unit tests (`mimhid.unit`, suite `link`) cover the parser.
- `mimhid.h` said `mimhid_node_set_feature_response` takes at most 32 bytes; the limit is the board's `max_feature_bytes` (1024 with the firmware's defaults), as the library checks since 1.1.0.
- `mimhid.h` now says, as libaoahid does and as the library has always done, that switching a pen between tip and eraser while in range first sends an out-of-range report, and that a battery level not yet sent is dropped on close.
- The initializers are said to fill in values that work with a Linux target; Windows and macOS targets have not been tried (`docs/TARGET_MATRIX.md`). They were said to work with all three.
- The documentation still named fields that no longer exist (`interface_number`, `ep_in_size`, `ep_out_size`, `mimhid_node_options`) in `README.md` and `docs/PORTING.md`; the HAL was called five functions in the README and eight in `docs/PORTING.md`, which now lists all nine (`hal_control_silent_drop` was missing); `docs/ARCHITECTURE.md` gave a 512-byte receive ring (4,096 bytes), a report queue of 16 (15 reports, 7 when a report may be longer than 64 bytes) and OUT endpoints of at most 64 bytes (the board's `max_endpoint_bytes`). An internal header called the wire protocol version 3.

## [2.0.0] - 2026-10-29

Not run on a board: this release changes the API around the serial link and the profiles, not the wire protocol (still 6) or the firmware, whose only change is its version number; a board on 1.5.0 firmware works with this library. The changes are tested by the unit, ABI and end-to-end tests (the host library against the firmware core through a pseudo-terminal), also under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

The ABI changes so that libmimhid and libaoahid follow the same rules for waits, closing and initial values, and use the same names for the same things.

### Changed (breaking)

- **One ownership rule for every close and destroy.** `MIMHID_ERR_PARAM` or `MIMHID_ERR_UNSET_FIELD`: nothing was done. Any other result: the object is gone, and an error says what failed on the way out. Before, `mimhid_node_close` returned at the first failure and left the Node with the caller, so a Node whose link was lost could only be freed by closing its Device.
  - `mimhid_node_close` sends the neutral report, takes the Node out of the configuration and commits it, even when a step fails, and returns the first failure.
  - `mimhid_device_close` returns `MIMHID_ERR_NO_DEVICE` when the link was already gone, so the board could not be reset; it returned `MIMHID_OK`.
- **The Linux HID parser limits are one structure,** `mimhid_linux_hid_policy`, in `mimhid_device_config.transport.linux_hid`: `descriptor_bytes`, `fields_per_report`, `global_stack_depth`, `report_size_bits`, `usages` and `report_data_bits` replace `linux_descriptor_policy_bytes` and the five `linux_hid_*_policy` fields. The error field is `transport.linux_hid`.
- **`mimhid_node_config_init(config, interface_number)`** takes the interface number, because the values it fills in depend on it (the IN endpoint is `0x81 + interface_number`); zero for every Node would have put them all on one interface.
- **`mimhid_discover` with a zero timeout returns `MIMHID_ERR_UNSET_FIELD`**, as every other zero timeout does (it returned `MIMHID_ERR_PARAM`).
- **Nodes sharing an interface must agree.** Nodes with the same `iface.number` must have the same interface and endpoint settings, and Nodes on different interfaces different endpoint addresses; `mimhid_node_open` returns `MIMHID_ERR_PARAM` (field `node_config.iface` or `node_config.ep_in.address`) instead of sending the board one interface with two sets of settings, or two interfaces with one endpoint.
- **`mimhid_error_detail.code` is typed `mimhid_result`** (the same `int32_t`).
- **The shared library's SONAME is `libmimhid.so.2`** (`libmimhid.2.dylib`) and its ELF version node is `MIMHID_2.0`, so a program built against 1.x fails to load against this library instead of reading the moved structure fields.

### Added

- **Initial values.** `mimhid_device_config_init`, `mimhid_commit_timeouts_init` and `mimhid_node_config_init` fill in values that work with a Linux, Windows or macOS target, and every profile options structure has an initializer (`mimhid_keyboard_options_init`, ...) with the values libaoahid's have; `mimhid_integer_field_make` builds one field. The USB vendor and product ID stay zero, and the library still never puts a value of its own in place of a zero. `MIMHID_CONTROL_TIMEOUT_MS`, `MIMHID_SEND_TIMEOUT_MS`, `MIMHID_DRAIN_TIMEOUT_MS` and `MIMHID_RECONNECT_TIMEOUT_MS` are timeouts that work. All are `static inline` or macros, so the binary ABI has nothing new for them. The values are listed in "Structures and initial values" in `docs/API.md`.
- `mimhid_frame_type_name(frame_type)` names the serial frame of `mimhid_error_detail.frame_type`.
- `mimhid.hpp` returns initialized structures (`mimhid::device_config()`, `mimhid::node_config(n)`, ...) and has `mimhid::SpecPtr` and `mimhid::DiscoveryPtr`, `std::unique_ptr` types that release a Spec or a discovery snapshot.
- The CMake option `MIMHID_SANITIZERS` (for example `address,undefined` or `thread`), and CI jobs that run every test with each.
- CI checks the formatting (clang-format 14) and that every exported function has its Ownership / Blocking / Synchronization / Returns contract (`tools/check_public_contracts.py`).

### Fixed

- `mimhid.h` said `mimhid_node_open` returns `MIMHID_ERR_UNSUPPORTED` for a boot protocol, which it never did, and that the event sink must not call the library, while `mimhid_device_control_reply` is meant to be called from it.
- The `MIMHID_ERR_BUSY` cases of the input calls list a Node opened in an unfinished batch and a board that is enumerating, as the library does.
- The documentation said strings hold at most 31 characters (126) and named `max_power_ma` (`usb_config.bmax_power`, 0 to 250 in 2 mA units); `docs/PROTOCOL.md` said protocol version 5 (6).

### Migration

- Start from `mimhid_device_config_init` and set `usb_device.vendor_id` and `product_id`, or move the Linux policy values into `config.transport.linux_hid`.
- Call `mimhid_node_config_init(&config, n)` with each Node's interface number.
- Treat a Node as gone after any `mimhid_node_close` result but `MIMHID_ERR_PARAM` and `MIMHID_ERR_UNSET_FIELD`.
- Give Nodes that share an interface the same `mimhid_node_config`.

The C++ forwarders and the examples follow the same signatures.

## [1.5.0] - 2026-10-27

### Changed (breaking)
- **`MIMHID_EVENT_REPORT_LOST` is off until the program asks for it.** 1.4.0 counted every dropped report and sent the event at most every 20 ms, a time nobody chose, to programs that did not want it. `mimhid_device_set_loss_reporting(device, enabled, interval_ms, control_timeout_ms)` (new, wire command `LOSS_CONFIG`) turns it on with the program's own interval (0 to `MIMHID_LOSS_MAX_INTERVAL_MS`, 0 for as soon as possible). Off, the board neither counts nor sends anything for it. `mimhid_device_close` turns it off.

### Added
- **`mimhid_node_set_queue_depth(node, depth, control_timeout_ms)`** (wire command `QUEUE_DEPTH`): how many reports may wait on the board for the node's interface, 1 up to what its queue holds (15, or 7 on a board taking reports longer than 64 bytes; `MIMHID_ERR_OVERFLOW` beyond). A short queue bounds how late a report reaches the target. The depth stays with the node across commits; nodes sharing an interface take the least depth any of them set. Until it is called the queue is as before.

### Fixed
- **A command sent again is not run twice.** The host re-sent a command that heard nothing back under a new sequence number, so the board ran it again and the answer to the first try was thrown away. It now sends the same frame with the same sequence number, as the protocol page always said, and the board answers a repeat of the last command from the answer it kept instead of running it again; a `PING` (a new host) makes it forget.

### Removed
- Two 1.4.0 changes that served no correct use: skipping sequence number 0 (no command ever waits on a `CONTROL_REPLY` answer, so nothing could collide) and `Link::close` waking a command another thread is still waiting in (`mimhid_device_close` must not run concurrently with other calls on the device).

Both new commands need firmware 1.5.0; an older board does not answer them (`MIMHID_ERR_TIMEOUT`). A board must be flashed again.

## [1.4.0] - 2026-10-27

### Added
- **`MIMHID_EVENT_REPORT_LOST`** (wire event `REPORT_LOST`, 11). A board whose report queue for an interface is full drops the newest report; it used to do so without a word, so a lost key-up or button release left the target with a stuck key. The board now counts the drops and tells the host (at most every 20 ms), and `value` is the count. Older boards never send it. A board must be flashed again.

### Fixed
- **A signal during a blocked write is not an I/O error.** `SerialPort::write_all` took `EINTR` from `poll` as a failure (`MIMHID_ERR_IO`, possibly mid-frame); it waits again. An error reported by `poll` itself is `EIO` instead of a stale `errno`.
- **A late ACK of an earlier attempt answers the command.** A command re-sent after a timeout now accepts the ACK or NAK of any of its attempts, not only the last, so a slow board is not reported as a timeout after it did the work.
- **Sequence number 0 is skipped** (it marks a `CONTROL_REPLY` nobody waits for).
- **`Link::close` wakes a waiting command** instead of leaving it for its whole timeout.
- **A partial board frame is given up after 10 ms of silence** (was 50 ms), the same quiet time the board allows itself.

## [1.3.6] - 2026-10-24

### Fixed
- **Endpoint 0 really runs at 8, 16 and 32 bytes.** Since 1.3.1 `usb_device.ep0_max_packet_size` reached the device descriptor, but the stack did not follow it: TinyUSB gives the controller up to `CFG_TUD_ENDPOINT0_SIZE` (64) bytes per transfer, the RP2040/RP2350 driver opens endpoint 0 at 64, and the `tud_control_ep0_size_cb` the firmware defined is not a TinyUSB callback and was never called. A board announcing 8 therefore answered the 18-byte device descriptor in one packet, which the target takes as a babble, and an IN data stage of a multiple of the packet size shorter than `wLength` had no zero-length packet. The firmware now wraps `dcd_edpt_xfer` (`-Wl,--wrap=dcd_edpt_xfer`, in every port; TinyUSB itself is not modified, new `firmware/common/ep0.c`): on endpoint 0 the controller is given one packet of the announced size at a time, the next started from the stack's task, the stack sees one completion for the whole, and the zero-length packet is added where the target waits for one. At 64 nothing changes. A board must be flashed again.
- **The board refuses an endpoint 0 size USB does not have.** `DEVICE_CONFIG` with a size other than 8, 16, 32 or 64 is answered with `MIMHID_NAK_PARAM`; the host library already refused it.
- **A held device-to-host request answered with less than `wLength` ends with a zero-length packet when its last packet is full.** The answer was given to the stack with `wLength` set to its own length, so the stack took it as complete (for example 64 bytes asked as 255). `wLength` now stays as the target sent it unless the answer is longer.

### Known limits
- Tested on the build host against the firmware core (`firmware/tests`). Only the RP2040/RP2350 images are built here; the STM32F4, nRF52840 and ESP32-S3 ports get the same link option but are not built or tried.

## [1.3.5] - 2026-10-24

### Changed (breaking)
- **`transport.maximum_report_bytes` and `transport.log_level` have no default.** Zero was "the board's limit" and "disabled"; both are now `MIMHID_ERR_UNSET_FIELD`. A `log_level` other than `MIMHID_LOG_DISABLED` needs a `log_sink` (`MIMHID_ERR_PARAM`). `maximum_report_bytes` above the board's `max_report_bytes` or `MIMHID_HOST_MAX_REPORT_BYTES` (new, 1024) is `MIMHID_ERR_OVERFLOW`.
- **`reserved` after `struct_size`** in `mimhid_device_config`, `mimhid_node_config`, `mimhid_commit_timeouts` and `mimhid_board_limits`, as in libaoahid. It must be zero (`MIMHID_ERR_PARAM`). The `_init` helpers set it.

### Fixed
- Documentation named structures and fields that no longer exist (`mimhid_node_options`, `mimhid_device_options`, `interface_number`, `ep_in_size`, `ep_out_size`); README, API, EXAMPLES, ARCHITECTURE, PROTOCOL, PROFILES, LIMITS and the header comments now use `mimhid_node_config`, `mimhid_device_config`, `iface.number` and `ep_in` / `ep_out`.

## [1.3.4] - 2026-10-24

### Changed (breaking)
- **Every timeout is an argument of the call; none is set at open and none has a default.** The four `mimhid_transport_options` fields `control_timeout_ms`, `send_timeout_ms`, `reconnect_timeout_ms` and `close_drain_timeout_ms` are removed, and a zero timeout is refused with `MIMHID_ERR_UNSET_FIELD` before anything is sent. New signatures:
  - `mimhid_node_open`, `mimhid_node_open_raw`, `mimhid_device_commit`, `mimhid_device_connect`, `mimhid_device_batch_end` and `mimhid_device_set_usb_identity` take a `const mimhid_commit_timeouts*` (new: `struct_size`, `control_timeout_ms`, `reconnect_timeout_ms`; `mimhid_commit_timeouts_init`). `mimhid_node_close` takes it and `drain_timeout_ms`.
  - `mimhid_device_close(device, control_timeout_ms, drain_timeout_ms)`; `mimhid_device_disconnect`, `mimhid_device_remote_wakeup`, `mimhid_device_set_control_routing` and `mimhid_node_set_feature_response` take `control_timeout_ms`.
  - `mimhid_node_submit`, `mimhid_raw_submit`, `mimhid_report_send` and `mimhid_device_control_reply` take `send_timeout_ms`. `mimhid_node_submit_blocking` keeps its `timeout_ms` and now also bounds each write by what is left of it.
  - `mimhid_transport_options.handshake_timeout_ms` (required) replaces the fixed 50 ms wait for each answer while the board is searched for in `mimhid_device_open`.
  - The C++ forwarders in `mimhid.hpp` follow. A refused `mimhid_device_close` timeout consumes nothing.
  - The internal defaults of `Link::commit`, `connect` and `disconnect` (2000 ms) are gone.

### Fixed
- **A zero-length device-to-host request answered with data gets its data stage.** `settle()` and `tud_vendor_control_xfer_cb()` treated every `wLength` 0 request as a status stage alone, so the data of an `ANSWER` was dropped. With data the answer now goes out whole, as it already did when `wLength` was shorter than the answer; an empty answer is the status stage as before. This is what a device does that ignores a zero length (the host then reports `EPROTO` or an overrun, depending on its controller; that is the host's, and untested here).

### Known limits
- A zero-length request that the real device refuses with a bus error, and not with data, still cannot be copied: the RP2040 USB controller answers every token with ACK, NAK or STALL on its own and cannot send a corrupt or absent handshake.

### Documentation
- `README.md`, `docs/API.md` (new Timeouts section; the device settings now describe `mimhid_device_config`), `docs/EXAMPLES.md` and `docs/PROTOCOL.md` follow the new signatures; their samples use `mimhid_device_config` and `mimhid_node_config`. The header comments of `mimhid.h` say which wait each call takes.

## [1.3.3] - 2026-10-23

### Fixed
- **A device answer longer than the host's `wLength` is sent whole.** The answer to a held device-to-host control request was cut to `wLength`; it now goes out as the real device sends it, so the host sees the same overrun (`EOVERFLOW`) as with the real device (for example `GET_STATUS` with `wLength` 1).
- **`MIMHID_CONTROL_ACTION_DROP` no longer stalls EP0 for a host-to-device request.** After the data stage the stack stalled EP0 on a drop; the firmware now wraps `dcd_edpt_stall` (`-Wl,--wrap=dcd_edpt_stall`, TinyUSB itself is not modified) and disarms EP0 through the new `hal_control_silent_drop`, so EP0 keeps NAKing as a silent drop is documented to do. Only the RP2040/RP2350 port implements it; the STM32F4, nRF52840 and ESP32-S3 ports have an empty stub and still stall.

### Known limits
- A zero-length device-to-host request (for example `GET_STATUS` with `wLength` 0) that the real device refuses with a bus error (`EPROTO`, -71 on Linux) cannot be copied: the RP2040 USB controller answers every token with ACK, NAK or STALL on its own and cannot send a corrupt or absent handshake. The host sees a STALL (`EPIPE`) instead.

## [1.3.2] - 2026-10-23

### Fixed
- **Transparent control transfer passthrough support.** Standard and non-standard control requests (including Interface `GET_STATUS` and zero-length control transfers with unexpected direction bits) are cleanly intercepted and held at the DCD layer, enabling full passthrough to target hardware without internal stack interference or deadlocks.

## [1.3.1] - 2026-10-22

### Added
- **USB 2.0 and HID 1.11 descriptor layout decoupling.** Transport options and USB descriptor specifications are cleanly decoupled into `mimhid_device_config` and `mimhid_node_config` with zero implicit preset or inference.
- **Dynamic Endpoint 0 packet size (`bMaxPacketSize0`) and USB specification version (`bcdUSB`).** Supports Full-Speed EP0 packet sizes (8, 16, 32, 64) and USB BCD versions (e.g. 0x0110, 0x0200, 0x0210) without clamping. Non-standard values are rejected strictly with `MIMHID_ERR_PARAM`.
- **Dynamic Interface numbering and arbitrary Interrupt IN/OUT endpoint addresses.** Interfaces and endpoints can be assigned arbitrary numbers (supporting non-consecutive interface IDs) and endpoint addresses matching target hardware layouts, with independent IN and OUT polling intervals (`bInterval`).
- **USB specification compliant string descriptors.** Strings support up to the USB 2.0 specification maximum of 126 characters (254-byte string descriptor including 2-byte header with UTF-16LE characters, preventing 1-byte `bLength` overflow) in zero-terminated ASCII buffers. Strings exceeding 126 characters are rejected strictly.
- **Runtime USB identity updates.** `mimhid_device_set_usb_identity` allows updating USB device and configuration descriptors without re-establishing the serial link.


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
