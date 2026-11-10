# Internals

How the host implementation behind `include/mimhid.h` works: which object owns what, which lock guards what, what runs on each path, and why the internal mechanisms are built the way they are. It is for people changing the library. The public model is in [ARCHITECTURE.md](ARCHITECTURE.md), the wire protocol in [PROTOCOL.md](PROTOCOL.md), and every limit in [LIMITS.md](LIMITS.md); this page does not repeat them.

## Source layout

| Directory | Holds | Depends on |
| --- | --- | --- |
| `src/api` | The ABI boundary, split across four files by what an entry point works on: `c_api_device.cpp` (discovery, Device lifecycle, control transfers), `c_api_node.cpp` (opening, configuring, and closing Nodes), `c_api_send.cpp` (report submission and the serial write path), and `device_ops.cpp` (the internals the entry points share: option checks, interface assignment, commit, event delivery). `support.hpp` declares what crosses files and holds the exception-barrier macros; `internal.hpp` defines the handles; `error_detail.cpp` holds the per-thread error record. | everything below |
| `src/profiles` | `spec.cpp` builds a Spec from the options of each profile. `state.cpp` holds the per-Node state machines and the bit-exact serializers. | `src/hid` |
| `src/hid` | The report descriptor builder (`descriptor_builder`, `item_writer`), the layout it records (`report_layout.hpp`), and the validator that parses a descriptor back (`validator.cpp`). | nothing |
| `src/transport` | `frame.*` (`FrameParser`, `encode_frame`, `decode_pong`), `Link` (ACK and retry, reader thread), `Handshaker` (finds a board and agrees a line rate), and `SerialPort` with one implementation per platform (`serial_posix.cpp`, `serial_baud_linux.cpp`, `serial_win32.cpp`) and port listing (`ports_linux.cpp`, `ports_macos.cpp`, `ports_win32.cpp`). | `proto/mimhid_proto.h` |

`proto/mimhid_proto.h` is shared with the firmware: frame types, limits, and the `PONG` layout. `c_api_device.cpp` checks the copies of these constants in `mimhid.h` against it with `static_assert`. No serial-port type appears above `src/transport/serial_port.hpp`.

## Handles

| Handle | Definition | Holds |
| --- | --- | --- |
| `mimhid_spec` | `src/api/internal.hpp` | An atomic reference count, the descriptor bytes, the `ReportLayout`, the `DescriptorRequirements`, the report capabilities, the copied options, and the optional battery layout. |
| `mimhid_discovery` | `src/api/internal.hpp` | A vector of `mimhid_device_info`, a snapshot. |
| `mimhid_device` | `src/api/internal.hpp` | A copy of the config, the board's answer (`BoardInfo`), the `Link` (and with it the port and the reader thread), the copied USB strings, `slots` (the Nodes by board node id, at most `MIMHID_MAX_NODES`), and the state flags below. |
| `mimhid_node` | `src/api/internal.hpp` | A retained Spec, the `NodeState` variant, the node id and interface number, the Feature answers, the submitted-report bookkeeping, and the completion fields. |

There is no Context: a Device owns its port and one reader thread, so two Devices share nothing. A Node keeps its own reference to its Spec, which is why the caller may release a Spec after `mimhid_node_open`.

## Locks and atomics

| Name | Guards | Notes |
| --- | --- | --- |
| `mimhid_device::nodes_mutex` | `slots` | The reader thread reads it to find the Node an event belongs to; Node open and close write it. `live_nodes` copies the pointers under it. |
| `Link::tx_mutex_` | One frame on the wire at a time | Reports, commands, and control replies all go through it. |
| `Link::cmd_mutex_` | One command in flight | A command waits for its ACK or NAK while holding it. |
| `Link::state_mutex_`, `state_cv_` | The reply slot (`reply_ready_`, `reply_nak_`, `reply_code_`), `mounted_`, and the pong | The reader thread fills the slot; the command waits on the condition variable. |
| `reconfiguring`, `closing`, `latched`, `loss_reported`, `control_routed` | Flags the reader thread and the caller both read | Atomics. `closing` is set once by close; `latched` holds `MIMHID_ERR_NO_DEVICE` after a serial failure. |
| `mimhid_node::transfer_inflight` and the `completion_*` fields | The one report a Node may have in flight | `exchange(true)` on entry is the check that refuses a second submit with `MIMHID_ERR_BUSY`. |

Order, where they nest: `cmd_mutex_`, then `tx_mutex_`, then `state_mutex_` (`Link::ping` waits for the pong holding `tx_mutex_`). `nodes_mutex` is never held across a `Link` call.

Apart from these, the library relies on the caller. A Device and its Nodes form one domain, and the caller makes sure no two calls in a domain run at once (see [API.md](API.md#threading-and-domains)). The Node mutators (`mimhid_keyboard_key` and the others in `state.cpp`) take no lock for that reason. The reader thread touches only the atomics, `nodes_mutex`, the `Link` internals, and the event sink; it never reads Node state.

The error record is `thread_local` (`error_detail.cpp`): each call clears it on entry and sets it on failure, and `mimhid_last_error` reads the calling thread's copy. The reader thread has its own, which nobody reads.

## The report path

`mimhid_node_submit` (`c_api_send.cpp`) builds and writes one report, synchronously:

1. Check the arguments and the profile (a Raw Node must use `mimhid_raw_submit`).
2. `link_available`: refuse when the serial link is gone, a batch is open, or the board is re-enumerating.
3. `transfer_inflight.exchange(true)`; refuse with `MIMHID_ERR_BUSY` when it was already set.
4. `consume_completion`: surface the result of the previous report.
5. Pick the report: pending input first, then the battery report if its level changed.
6. For a touch Node, derive Scan Time from a steady-clock epoch.
7. `serialize_node` or `serialize_battery` writes the bytes into a stack buffer. Nothing is allocated on this path.
8. With `transport.validate_reports` set to `1`, `validate_serialized_report` checks them against the Spec.
9. `write_report` calls `Link::send_report` (one `write` under `tx_mutex_`, bounded by the caller's timeout) and settles the Node with `transfer_complete`.

Reports are not acknowledged by the board. A failed write rolls the scan time back and leaves the pending state in place, so the same call can be repeated. `mimhid_node_submit_blocking` repeats `mimhid_node_submit` until the Node has nothing left to send or the deadline passes (`flush_node`).

## Commit

`commit_device` (`src/api/device_ops.cpp`) applies every staged change and is the only place the board's configuration changes:

1. It takes a `ReconfigGuard`, which sets `reconfiguring = true` so input calls return `MIMHID_ERR_BUSY` meanwhile.
2. With no live Node left, it sends a `COMMIT` with byte `1` (`{1, 0, 0}`) that detaches the board from the USB bus, sets `device->configured = false`, and returns.
3. When `device->configured` is already `true`, it first detaches the board by calling `Link::disconnect` (`COMMIT` `{1, 0, 0}`) and, if `usb_config.reconnect_delay_ms > 0`, sleeps `reconnect_delay_ms` on the host so the target USB host observes the disconnect cleanly.
4. It encodes and sends `DEVICE_CONFIG`, then for each live Node sends `NODE_BEGIN`, one or more `NODE_DATA` chunks (up to 240 bytes each), `QUEUE_DEPTH` (when `node->queue_depth != 0`), and every registered `FEATURE` answer.
5. It calls `Link::connect(reconnect_delay_ms, reconnect_timeout_ms)`, which sends `COMMIT` `{0, delay_lo, delay_hi}`, waits up to `500 ms` (`kDefaultCommandTimeoutMs`) for the board's `ACK`, and then waits up to `reconnect_timeout_ms` on `state_cv_` for `mounted_` (`EVENT MOUNTED`).
6. Each command is tried up to `kCommandAttempts` (3) times with the same sequence number; the board answers a repeated sequence number again instead of running the command twice.

`mimhid_device_batch_begin` sets `batch_open`; while it is set, Node open and close only stage their change (`staged_only`, `dirty`) and `mimhid_device_batch_end` commits once.

## Events

The reader thread (`Link::reader_loop`) feeds bytes to a `FrameParser`, completes the pending command on an ACK or NAK, and passes every other frame to `handle_frame`, which hands events to `deliver_event` in `device_ops.cpp`. That function turns a `BoardEvent` into a `mimhid_event`, finds the Node by interface under `nodes_mutex`, and calls `transport.event_sink` on the reader thread. The data pointer is valid only during the call. The one library call the sink may make is `mimhid_device_control_reply`, which only writes a frame (`Link::send_control_reply`) and never waits for an answer.

`FrameParser` keeps a 16 KiB ring. It skips garbage one byte at a time, accepts only the frame types a board sends, and drops an incomplete frame byte by byte when the line has been idle for 10 ms (`MIMHID_PARSER_IDLE_RESET_MS`).

## Close

`mimhid_device_close` sets `closing` first, so later calls on the Device fail, then sends the neutral report of every Node (within `drain_timeout_ms`), takes the board off the bus unless `keep_attached` is `1`, puts the board back on the boot rate (`115,200`), stops the reader thread, and closes the port. A failed step does not stop the next one, and the first error is returned.

## Language bindings

`tools/bindings/generate.py` reads `include/mimhid.h` through `clang -Xclang -ast-dump=json` (structures, function pointer types, exported functions, enumerations) and `clang -E -dM` (integer macros), and writes seven files: the C layout oracle `tests/abi/layout_oracle.c`, and for each of Python, C#, and Rust the declarations and a layout test. Nothing in them is written by hand, so a change to the header is a regeneration, not a transcription.

- An enumeration is the anonymous `enum` after an `int32_t` typedef; it becomes `int32_t` plus constants (C#: an `enum : int` whose member names drop the prefix the constants share).
- Only exported functions are bound: `static inline` functions are skipped.
- In C#, a function with a structure pointer has two forms with the same entry point: `in`/`ref` for the common case, `nint` where `null` is allowed. An array in a structure is a `fixed` buffer, which keeps the structure blittable (no marshalling copy, and a default structure is valid). A `size_t*` named `out_*` is `out nuint`, any other is `ref nuint`.
- `bindings/python/tests/test_contract.py` is hand-written: what the package exports, and that the binding fills no option field. The layout tests also check that a new options structure has `struct_size` 0.
- `tools/bindings/generate.py` is shared with `libaoahid` (the two copies differ only in the copyright line); `TYPE_RENAMES` and `CS_MEMBER_RENAMES` hold the few names that are not the C name with its prefix cut, kept because `libaoahid` published them first.
- The layout test of each language runs the oracle and compares every structure size, field offset, and constant. `generate.py --check` fails when a committed file differs from the header; CI runs both (`bindings` job).
- `tools/release/version_files.py` lists the version sites of the hand-written files; the constants inside the generated files are not on it, because `--check` pins them to the header.
- The generator refuses what it cannot bind (an unknown type, a field named like a Python keyword, two types with one short name) instead of guessing.

## Design decisions

Each internal design decision below states the mechanism, its reason, the alternative that was rejected, and where it is documented in the repository.

### 1. Zero heap allocation on the mutation and report-submit paths, and one report in flight per Node

- **Decision:** Mutating a Node (`src/profiles/state.cpp`) updates fixed-size arrays inside `NodeState`. `mimhid_node_submit` (`src/api/c_api_send.cpp:181-207`) serializes the report into a stack buffer (`std::array<uint8_t, kMaxStackReport>`), and `Link::send_report` (`src/transport/link.cpp:240-257`) encodes the wire frame into a stack buffer (`uint8_t buf[MIMHID_PROTO_MAX_PAYLOAD + 8]`) before calling `SerialPort::write` under `tx_mutex_`. `mimhid_node::transfer_inflight` permits at most one report submission in flight per Node at a time.
- **Reason:** Input submission runs at up to 1000 Hz per interface; avoiding heap allocation and dynamic queueing on the submit path keeps host CPU cost deterministic and eliminates unbounded host-side queue buildup if the serial port stalls (`docs/ARCHITECTURE.md:47`, `docs/LATENCY.md:64`, `docs/LIMITS.md:108`, `src/api/c_api_send.cpp:181-207`).
- **Rejected alternative:** Allocating a `std::vector<uint8_t>` per report or queueing reports on a host-side background writer thread, which would add allocator jitter and hide serial backpressure from the caller.

### 2. Pending-edge guard (`MIMHID_ERR_BUSY`) on opposite state transitions

- **Decision:** Each profile state machine tracks which keys, buttons, toggle usages, or contact slots have an unsent transition (`keys_edge`, `buttons_edge`, `usages_edge`, `pending_edge`, `tip_edge`, `barrel_edge` in `src/profiles/state.cpp`). Attempting to reverse a transition (for example, pressing and releasing a key, or lifting and re-placing the same touch slot) before `mimhid_node_submit` has sent the first transition returns `MIMHID_ERR_BUSY` without mutating state.
- **Reason:** Because state mutators update a snapshot that is serialized only when `mimhid_node_submit` is called, allowing both a press and a release before a submit would collapse the two opposite edges into a no-op and silently drop the tap or click on the target OS (`include/mimhid.h:1433-1435`, `src/profiles/state.cpp:110-114,193-194,274-275,382-383,465-469`).
- **Rejected alternative:** Silently overwriting the previous unsent state (which loses short taps/clicks when a caller forgets to submit between press and release) or maintaining an unbounded internal event queue per Node.

### 3. Lock-free SPSC ring per HID interface on the board, dropping the newest report with `REPORT_LOST` when full

- **Decision:** In `firmware/common/core.c:104-149`, each USB HID interface has a fixed-size single-producer single-consumer ring (`MIMHID_REPORT_QUEUE_LEN = 16`, holding 15 reports) indexed by `head` and `tail` with C11 `__atomic_load_n(__ATOMIC_ACQUIRE)` and `__atomic_store_n(__ATOMIC_RELEASE)`. On RP2040 and RP2350 (`firmware/rp/main.c:8-25`, `firmware/rp/hal_rp.c:34-99`), core 1 produces into the ring and core 0 pops and calls `tud_hid_n_report`, with the UART DMA buffer placed in `.scratch_y` SRAM. When a ring is full, `queue_push` drops the incoming report and increments `dropped_reports` (reported via `EVENT REPORT_LOST` when enabled by `LOSS_CONFIG`).
- **Reason:** Running UART parsing on core 1 and TinyUSB on core 0 with acquire/release SPSC rings avoids disabling interrupts or taking locks across cores, and placing the DMA buffer in `.scratch_y` prevents bus contention with core 0. Dropping the newest report when full preserves already-queued state transitions in order and lets the host observe saturation through `MIMHID_EVENT_REPORT_LOST` (`docs/ARCHITECTURE.md:58-65`, `docs/LATENCY.md:65,67`, commits `ab98886` and `8add634`).
- **Rejected alternative:** Sharing a mutex-protected queue across cores or overwriting the oldest queued report on overflow (which could drop a key-down or contact-lift edge while keeping a later motion frame).

### 4. Two-copy configuration staging (`staging` vs. `active`) and a single-slot command mailbox

- **Decision:** `firmware/common/core.c:72-97,151-158` maintains two complete `mimhid_config_state_t` structures (`staging` and `active`) and a single-slot command mailbox (`cmd_slot`). The UART receive context (`mimhid_rx_poll`) writes `DEVICE_CONFIG`, `NODE_BEGIN`, and `NODE_DATA` directly into `staging`, and posts commands (`COMMIT`, `FEATURE`, `RESET`, `REMOTE_WAKEUP`, `CONTROL_CONFIG`, `LOSS_CONFIG`, `QUEUE_DEPTH`) to `cmd_slot`. Only the USB context (`mimhid_usb_poll`) reads `cmd_slot` and copies `staging` to `active` during `COMMIT`.
- **Reason:** On dual-core ports (RP2040, RP2350), core 0 serves USB descriptor requests from `active` inside `tud_task()` while core 1 parses UART frames. Keeping `staging` separate from `active` and executing `COMMIT` on core 0 ensures no core ever mutates a descriptor table while the other core is serving it to the USB host (`docs/PROTOCOL.md:53`, `firmware/common/core.c:72-97,568-602`).
- **Rejected alternative:** Mutating the live descriptor state directly from the UART receive context or taking a cross-core lock on every USB descriptor read.

### 5. Command sequence numbers (`1..255`) with single-slot deduplication on the board

- **Decision:** Every command frame (except `PING` and `PROBE`) begins with a 1-byte sequence number (`1..255`, wrapping from `255` to `1`). The host (`Link::send_command` in `src/transport/link.cpp:150-220`) retries an unacknowledged command up to 3 times (`kCommandAttempts`) with the **same** sequence number. The board (`firmware/common/core.c:568-589`) caches the last executed `(seq, type, is_nak, code, data)` and, if the same `(seq, type)` arrives again, resends the cached `ACK`/`NAK` without re-executing the command. A `PING` clears `last_cmd_seq`.
- **Reason:** If a board's `ACK` is corrupted on the UART return path (or delayed past the host's timeout by a USB-UART adapter latency timer) and the host retransmits the command, re-executing a side-effecting command like `COMMIT` would cause a second USB bus disconnect/re-enumeration, and re-executing `RESET` would drop newly queued reports (`docs/PROTOCOL.md:45`, `src/transport/link.cpp:150-220`, commit `8add634`).
- **Rejected alternative:** Stateless command handling on the board (where every retry re-runs the command) or sequence numbers without reset on `PING` (which would collide when a new host process connects and starts at sequence number `1`).

### 6. Endpoint 0 control-transfer interception and packet splitting via linker `--wrap` instead of forking TinyUSB

- **Decision:** `firmware/common/control.c` and `firmware/common/ep0.c` intercept TinyUSB's device controller driver boundary using GNU ld's `-Wl,--wrap=dcd_event_handler`, `-Wl,--wrap=dcd_edpt_xfer`, and `-Wl,--wrap=dcd_edpt_stall`. Matched setup packets are held before TinyUSB sees them, and when answered with `ANSWER` or `STALL` their `bmRequestType` type field is rewritten to Vendor so `tud_vendor_control_xfer_cb` serves the host's payload or stalls. When `bMaxPacketSize0 < 64`, `__wrap_dcd_edpt_xfer` slices each endpoint 0 transfer into packets of `bMaxPacketSize0` bytes and schedules subsequent slices via `usbd_defer_func`.
- **Reason:** Wrapping the three `dcd_*` symbols at link time works identically across every MCU port's USB controller driver (RP2040/RP2350, STM32 DWC2, ESP32-S3 DWC2, nRF52840) and allows an unmodified upstream TinyUSB checkout (`MIMHID_TINYUSB_PATH`) to be used (`docs/PROTOCOL.md:124`, `docs/PORTING.md:27-29`, `firmware/common/control.h:13-17`, `firmware/common/ep0.h:19-26`, commits `b449e39` and `9f32c91`).
- **Rejected alternative:** Patching TinyUSB's `usbd.c` or per-MCU `dcd_*.c` drivers in a forked submodule, which would have to be maintained across every MCU family and TinyUSB upgrade.

### 7. Two-pass descriptor construction (`counting_builder` then `emitting_builder`) and self-validation

- **Decision:** `build_with_two_passes` (`src/profiles/spec.cpp:232-269`) runs each profile's descriptor recipe twice: first against a `counting_builder` (which tracks byte count and layout without allocating), then `reserve(counted_bytes)` on a single `std::vector<uint8_t>` followed by an `emitting_builder`, and finally parses the generated descriptor back through `validate_generated` (`src/hid/validator.cpp`). Every report section is padded to a whole byte boundary (`pad_to_byte_boundary`).
- **Reason:** Two-pass construction allocates the exact descriptor buffer once and verifies that the counted size and emitted size match (`MIMHID_ERR_INTERNAL` if they diverge), while running the independent HID parser/validator (`validate_generated`) computes the exact `DescriptorRequirements` (`descriptor_bytes`, `fields_per_report`, `global_stack_depth`, `report_size_bits`, `usages`, `report_data_bits`) and catches any malformed HID item before a Spec can be created (`src/profiles/spec.cpp:132-137,232-269,757-760,935-954`).
- **Rejected alternative:** Single-pass growth with repeated vector reallocations and trusting the builder's output without parsing it back through the HID validator.

## How the tests reach this code

- `mimhid.unit` links `mimhid_test_objects`, a second compile of the sources, so it can call the internal functions (`serialize_node`, `FrameParser`, the descriptor builder) that the shared library does not export.
- `mimhid.scripted` (`tests/scripted/`) runs the host library over a pseudo-terminal against a scripted board peer to test protocol edge cases, NAKs, timeouts, and hangups.
- `mimhid.sim-*` (`tests/sim/`) runs the real firmware core against the host library through a pseudo-terminal (`tests/sim/fake_board.c`) for 15-, 7-, and 3-interface Full-Speed configurations and a High-Speed configuration. It needs a TinyUSB checkout (`MIMHID_TINYUSB_PATH`).
- `tests/fuzz/fuzz_descriptor.cpp` (`MIMHID_BUILD_FUZZ=ON`, Clang) feeds the descriptor builder, the serializer, the raw-descriptor validator, and the `FrameParser`.
- `bindings/*/tests` compare each binding's layout with `tests/abi/layout_oracle.c`, compiled from the header.
- `mimhid.example-profile-*` build a Spec of every profile kind and check its manifest against the descriptor.
- CI formats with `clang-format` 14 and lints the production sources with `clang-tidy` 14 (`.clang-tidy`; the Windows sources cannot be parsed on Linux and are skipped). A newer version of either tool reports and lays out some things differently. `bugprone-easily-swappable-parameters` is silenced with `NOLINTNEXTLINE` on functions whose parameters follow the wire or the C ABI order, as in `libaoahid`; the other checks are fixed in the code.
