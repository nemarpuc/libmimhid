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
| `mimhid_device` | `src/api/internal.hpp` | A copy of the config, the board's answer (`BoardInfo`), the `Link` (and with it the port and the reader thread), the copied USB strings, `slots` (the Nodes by node id, a vector that grows with the Nodes, at most 65534), and the state flags below. |
| `mimhid_node` | `src/api/internal.hpp` | A retained Spec, the `NodeState` variant, the node id (an index into `slots`; the board does not know it) and interface numbers (the real `bInterfaceNumber`s) and, for a Node of the general open, its interfaces with each endpoint's runtime (wait count and policy, guarded by the Device's `endpoint_mutex` and signalled on `endpoint_cv`), the Feature answers, the submitted-report bookkeeping, and the completion fields. |

There is no Context: a Device owns its port and one reader thread, so two Devices share nothing. A Node keeps its own reference to its Spec, which is why the caller may release a Spec after `mimhid_node_open`.

## Locks and atomics

| Name | Guards | Notes |
| --- | --- | --- |
| `mimhid_device::nodes_mutex` | `slots` | The reader thread reads it to find the Node an event belongs to; Node open and close write it. `live_nodes` copies the pointers under it. |
| `Link::tx_mutex_` | One frame on the wire at a time | Reports, commands, and control replies all go through it. |
| `Link::cmd_mutex_` | One command in flight | A command waits for its ACK or NAK while holding it. |
| `Link::state_mutex_`, `state_cv_` | The reply slot (`reply_ready_`, `reply_nak_`, `reply_code_`, `reply_data_`) | The reader thread fills the slot; the command waits on the condition variable. |
| `Link::ready_`, `failed_`, `stop_` | Mounted state, a failed port, the reader thread's stop request | Atomics. The reader thread sets `ready_` on `EVENT MOUNTED`; `mimhid_device_is_ready` and `mimhid_device_wait_ready` read it. |
| `reconfiguring`, `closing`, `latched`, `loss_reported`, `control_routed` | Flags the reader thread and the caller both read | Atomics. `closing` is set once by close; `latched` holds `MIMHID_ERR_NO_DEVICE` after a serial failure. |
| `mimhid_node::transfer_inflight` and the `completion_*` fields | The one report a Node may have in flight | `exchange(true)` on entry is the check that refuses a second submit with `MIMHID_ERR_BUSY`. |

`Link::command` holds `cmd_mutex_` for the whole command and, inside it, takes `tx_mutex_` for the write and then, after releasing it, `state_mutex_` for the wait; `tx_mutex_` and `state_mutex_` are not held together.

Apart from these, the library relies on the caller. A Device and its Nodes form one domain, and the caller makes sure no two calls in a domain run at once (see [API.md](API.md#threading-and-domains)). The Node mutators (`mimhid_keyboard_key` and the others in `state.cpp`) take no lock for that reason. The reader thread touches only the atomics, `nodes_mutex`, the `Link` internals, and the event sink; it never reads Node state.

The error record is `thread_local` (`error_detail.cpp`): each call clears it on entry and sets it on failure, and `mimhid_last_error` reads the calling thread's copy. The reader thread has its own, which nobody reads.

## The report path

`mimhid_node_submit` (`c_api_send.cpp`) builds and writes one report, synchronously:

1. Check the arguments and the profile (a Raw Node must use `mimhid_raw_submit`).
2. `link_available`: refuse when the serial link is gone, the Node was opened in a batch that has not ended, or the board is re-enumerating.
3. `transfer_inflight.exchange(true)`; refuse with `MIMHID_ERR_BUSY` when it was already set.
4. `consume_completion`: surface the result of the previous report.
5. Pick the report: pending input first, then the battery report if its level changed.
6. For a touch Node, derive Scan Time from a steady-clock epoch.
7. `serialize_node` or `serialize_battery` writes the bytes into a stack buffer of `kMaximumReportBytes` (`MIMHID_HOST_MAX_REPORT_BYTES`, `src/api/support.hpp`).
8. With `transport.validate_reports` set to `1`, `validate_serialized_report` checks them against the Spec.
9. `write_report` calls `Link::send_report` (one `write` under `tx_mutex_`, bounded by the caller's timeout) and settles the Node with `transfer_complete`.

Reports are not acknowledged by the board. A failed write rolls the scan time back and leaves the pending state in place, so the same call can be repeated. `mimhid_node_submit_blocking` repeats `mimhid_node_submit` until the Node has nothing left to send or the deadline passes (`flush_node`).

## Commit

`commit_device` (`src/api/device_ops.cpp`) applies every staged change and is the only place the board's configuration changes:

1. It takes a `ReconfigGuard`, which sets `reconfiguring`, so input calls return `MIMHID_ERR_BUSY` meanwhile.
2. With no live Node left, it sends a detach `COMMIT` (`{1, reconnect_delay_ms & 0xFF, reconnect_delay_ms >> 8}`; the firmware ignores the last two bytes) when the board is configured, sets `configured = false`, and returns.
3. It checks the descriptors together against the board's `max_descriptor_bytes` (`MIMHID_ERR_OVERFLOW`), composes every descriptor (`compose_descriptors` in `src/usb/descriptor_composer.cpp`, which also checks that the interface numbers are 0, 1, 2, ... without a gap, the Report IDs of Nodes that share an interface, and the endpoint addresses), and sends `DESC_RESET`, then the device descriptor, the configuration descriptor, the strings and the report descriptor of every interface (each a `DESC_BEGIN` and `DESC_DATA` chunks of up to `MIMHID_DESC_DATA_CHUNK` (240) bytes), every registered `FEATURE` answer (keyed by interface number), and an `EP_CONFIG` with `MIMHID_EP_FULL_DROP_NEWEST` for each interface whose Nodes set a `queue_depth` (the least of them). Each frame goes through `Link::command` with `kCommandAttempts` (3) and `control_timeout_ms`; the board answers a repeated sequence number again instead of running the command twice.
4. When the board is configured, it calls `Link::disconnect` (detach `COMMIT` `{1, 0, 0}`, one attempt, `control_timeout_ms`) and, when `reconnect_delay_ms` is not 0, sleeps that long on the host.
5. It calls `Link::connect` (attach `COMMIT` `{0, 0, 0}`, one attempt), which waits up to `reconnect_timeout_ms` for the board's `ACK`. It does not wait for `EVENT MOUNTED`; `mimhid_device_wait_ready` does.

`mimhid_device_batch_begin` sets `batch_open`; while it is set, Node open and close only stage their change (`staged_only`, `dirty`) and `mimhid_device_batch_end` commits once.

## Events

The reader thread (`Link::reader_loop`) feeds bytes to a `FrameParser`, completes the pending command on an ACK or NAK, and passes every other frame to `handle_frame`, which hands events to `deliver_event` in `device_ops.cpp`. That function turns a `BoardEvent` into a `mimhid_event`, finds the Node by interface under `nodes_mutex`, and calls `transport.event_sink` on the reader thread. The data pointer is valid only during the call. The one library call the sink may make is `mimhid_device_control_reply`, which only writes a frame (`Link::send_control_reply`) and never waits for an answer.

`FrameParser` keeps a 16 KiB ring (`kCapacity`). It skips garbage one byte at a time, accepts only the frame types a board sends (`PONG`, `ACK`, `NAK`, `EVENT`), and drops an incomplete frame one byte at a time (`FrameParser::idle`) when a read of 10 ms, the reader thread's wait while a frame is incomplete, brings nothing.

## Close

`mimhid_device_close` sets `closing` first, so later calls on the Device fail, then sends the neutral report of every Node (within `drain_timeout_ms`), takes the board off the bus unless `keep_attached` is `1`, puts the board back on the boot rate (`115,200`), stops the reader thread, and closes the port. A failed step does not stop the next one, and the first error is returned.

## Language bindings

`tools/bindings/generate.py` reads `include/mimhid.h` through `clang -Xclang -ast-dump=json` (structures, function pointer types, exported functions, enumerations) and `clang -E -dM` (integer macros), and writes seven files: the C layout oracle `tests/abi/layout_oracle.c`, and for each of Python, C#, and Rust the declarations and a layout test. Nothing in them is written by hand, so a change to the header is a regeneration, not a transcription.

- An enumeration is the anonymous `enum` after an `int32_t` typedef; it becomes `int32_t` plus constants (C#: an `enum : int` whose member names drop the prefix the constants share).
- Only exported functions are bound: `static inline` functions are skipped.
- In C#, a function with a structure pointer has two forms with the same entry point: `in`/`ref` for the common case, `nint` where `null` is allowed. An array in a structure is a `fixed` buffer, which keeps the structure blittable (no marshalling copy, and a default structure is valid). A `size_t*` named `out_*` is `out nuint`, any other is `ref nuint`.
- `bindings/python/tests/test_contract.py` is hand-written: what the package exports, and that the binding fills no option field. The layout tests also check that a new options structure has `struct_size` 0.
- `libaoahid` has its own copy of `tools/bindings/generate.py`; the two differ in more than the copyright line (compare them before changing one). `TYPE_RENAMES` and `CS_MEMBER_RENAMES` hold the few names that are not the C name with its prefix cut.
- The layout test of each language runs the oracle and compares every structure size, field offset, and constant. `generate.py --check` fails when a committed file differs from the header; CI runs both (`bindings` job).
- `tools/release/version_files.py` lists the version sites of the hand-written files; the constants inside the generated files are not on it, because `--check` pins them to the header.
- The generator refuses what it cannot bind (an unknown type, a field named like a Python keyword, two types with one short name) instead of guessing.

## Design decisions

Each decision states what the code does. A reason is given only where the repository records one (a code comment, `CHANGELOG.md`, or a commit message), and says where.

### Reports are written synchronously, one in flight per Node

- **Decision:** The Node mutators in `src/profiles/state.cpp` change fixed-size arrays inside `NodeState`. `mimhid_node_submit` serializes the report into a stack buffer (`std::array<std::uint8_t, kMaximumReportBytes>`) and `Link::send_report` writes the frame under `tx_mutex_` on the caller's thread. `mimhid_node::transfer_inflight` allows one report of a Node in flight at a time.
- **Reason:** Not recorded.
- **Rejected:** Not recorded.

### An opposite transition waits for the report of the first

- **Decision:** Each profile state keeps which keys, buttons, controls, or lifecycle changes have not yet been sent (`key_transitions`, `modifier_transitions`, `button_transitions`, `transitions`, `hat_transition_pending`, `lifecycle_transition_pending`, `barrel_transitions`, `in_range_transition_pending`, `tip_transition_pending`, `eraser_transition_pending`, `tool_switch_departure`). Reversing one of them before a report has carried it (`pending_transition` in `state.cpp`) returns `MIMHID_ERR_BUSY` and changes nothing.
- **Reason:** Not recorded. `mimhid.h` states the result ("an opposite edge is unsent").
- **Rejected:** Not recorded.

### One report queue per interface on the board; a full queue drops the new report

- **Decision:** Each HID interface has a single-producer single-consumer ring (`report_queue_t`, `MIMHID_QUEUE_DEPTH` slots, `head` and `tail` read and written with `__atomic_load_n` and `__atomic_store_n`). The receive side writes it and the USB side reads it; on the RP2040 and RP2350 these are core 1 and core 0. A report that finds the queue full is dropped and, once `LOSS_CONFIG` turned loss reporting on, counted in `g_lost` and reported as `EVENT TRANSFER_LOST` (the host maps the endpoint address to a Node and reports `MIMHID_EVENT_TRANSFER_LOST`).
- **Reason:** Recorded for the depth: `core.c` says "A short queue bounds how late a report can reach the target". Commit `ab98886` ("tell the host about dropped reports") added the count. Not recorded for the queue design or for dropping the new report rather than the oldest.
- **Rejected:** Not recorded.

### Staged and active configuration on the USB side, behind a command mailbox

- **Decision:** The receive side passes every command except `PING`, `SET_BAUD`, `COUNTERS`, `PROBE`, and `CONTROL_REPLY` to the USB side through `g_cmd` (four slots, three commands). The USB side (`run_command`) fills the staged configuration (`g_staged`: descriptors as bytes, from `DESC_RESET`, `DESC_BEGIN`, `DESC_DATA`), and on an attach `COMMIT` walks it and, when it holds together, copies it to the active one (`mimhid_apply_config` in `usb_descriptors.c`), which serves the TinyUSB descriptor callbacks from those bytes.
- **Reason:** Recorded for the commands kept on the receive side: `SET_BAUD` "Runs on the receive side: the ACK has to leave at the old rate before the switch", `COUNTERS` because "the counters belong to it", and `CONTROL_REPLY` because "The USB side may be waiting in a TinyUSB callback, so the reply is taken here" (all `core.c`). Not recorded for the rest.
- **Rejected:** Not recorded.

### A repeated command is answered, not run again

- **Decision:** Every command frame starts with a one-byte sequence number; the host counts it up per command and lets it wrap (`Link::command`, `seq_`). The host resends an unanswered command with the same number up to `kCommandAttempts` (3) times. The board keeps the answer to the last command it ran (`g_answer_last`: sequence number, type, NAK code, and the `PING` count) and answers a command with the same number and type again without running it. A `PING` makes it forget.
- **Reason:** `docs/PROTOCOL.md` gives the effect ("a `COMMIT` does not re-enumerate, a `RESET` drops nothing more") and "A `PING` makes the board forget it, since a new host starts its numbers anew". Commit `8add634` states the decision without a reason.
- **Rejected:** Commit `ab98886` skipped sequence number 0; commit `8add634` removed the skip. Why is not recorded.

### Endpoint 0 is reached by wrapping TinyUSB's controller driver calls

- **Decision:** `firmware/common/control.c` and `firmware/common/ep0.c` wrap `dcd_event_handler`, `dcd_edpt_xfer`, and `dcd_edpt_stall` with the linker (`-Wl,--wrap=...`). A held request is kept from TinyUSB; for `ANSWER` and `STALL` its type bits are set to vendor so that `tud_vendor_control_xfer_cb` serves it. With a `bMaxPacketSize0` below 64, `__wrap_dcd_edpt_xfer` sends endpoint 0 transfers one packet at a time, starting the next from the stack's task (`usbd_defer_func`).
- **Reason:** Recorded for the packet splitting: `ep0.h` says TinyUSB and the RP2040 driver use 64 bytes, "so with a smaller size the target would see packets longer than it was told". `control.h` says "TinyUSB's control state machine is never bypassed". Not recorded for wrapping rather than changing TinyUSB.
- **Rejected:** Not recorded.

### A generated descriptor is built twice and parsed back

- **Decision:** `build_generated_spec` (`src/profiles/spec.cpp`) runs each profile's recipe against a counting `DescriptorBuilder` (`counting_builder`) for the size, the layout, and the `DescriptorRequirements`, then against an emitting one (`emitting_builder`) into a buffer of that size. Any difference in size or requirements between the two passes is `MIMHID_ERR_INTERNAL`. The result is then parsed back by `validate_generated` (`src/hid/validator.cpp`).
- **Reason:** Not recorded.
- **Rejected:** Not recorded.

## How the tests reach this code

- `mimhid.unit` links `mimhid_test_objects`, a second compile of the sources, so it can call the internal functions (`serialize_node`, `FrameParser`, the descriptor builder) that the shared library does not export.
- `mimhid.scripted` (`tests/scripted/`) runs the host library over a pseudo-terminal against a scripted board peer to test protocol edge cases, NAKs, timeouts, and hangups.
- `mimhid.sim-*` (`tests/sim/`) runs the real firmware core against the host library through a pseudo-terminal (`tests/sim/fake_board.c`) for 15-, 7-, and 3-interface Full-Speed configurations and a High-Speed configuration. It needs a TinyUSB checkout (`MIMHID_TINYUSB_PATH`).
- `tests/fuzz/fuzz_descriptor.cpp` (`MIMHID_BUILD_FUZZ=ON`, Clang) feeds the descriptor builder, the serializer, the raw-descriptor validator, and the `FrameParser`.
- `bindings/*/tests` compare each binding's layout with `tests/abi/layout_oracle.c`, compiled from the header.
- `mimhid.example-profile-*` build a Spec of every profile kind and check its manifest against the descriptor.
- CI formats with `clang-format` 14 and lints the production sources with `clang-tidy` 14 (`.clang-tidy`; the Windows sources cannot be parsed on Linux and are skipped). A newer version of either tool reports and lays out some things differently. `bugprone-easily-swappable-parameters` is silenced with `NOLINTNEXTLINE` on functions whose parameters follow the wire or the C ABI order, as in `libaoahid`; the other checks are fixed in the code.
