# Internals

How the host implementation behind `include/mimhid.h` works: which object owns what, which lock guards what, and what runs on each path. It is for people changing the library. The public model is in [ARCHITECTURE.md](ARCHITECTURE.md), the wire protocol in [PROTOCOL.md](PROTOCOL.md), and every limit in [LIMITS.md](LIMITS.md); this page does not repeat them. The firmware is not covered here; `firmware/common/core.c` is described in ARCHITECTURE.md.

## Source layout

| Directory | Holds | Depends on |
| --- | --- | --- |
| `src/api` | the ABI boundary, in four files by what an entry point works on: `c_api_device.cpp` (discovery, Device life cycle, control transfers), `c_api_node.cpp` (opening, configuring and closing Nodes), `c_api_send.cpp` (report submission and the serial write path), and `device_ops.cpp` (the internals the entry points share: option checks, interface assignment, commit, event delivery). `support.hpp` declares what crosses files and holds the exception-barrier macros; `internal.hpp` defines the handles; `error_detail.cpp` holds the per-thread error record. | everything below |
| `src/profiles` | `spec.cpp` builds a Spec from the options of each profile. `state.cpp` holds the per-Node state machines and the bit-exact serializers. | `src/hid` |
| `src/hid` | The report descriptor builder (`descriptor_builder`, `item_writer`), the layout it records (`report_layout.hpp`), and the validator that parses a descriptor back (`validator.cpp`). | nothing |
| `src/transport` | `frame.*` (`FrameParser`, `encode_frame`, `decode_pong`), `Link` (ACK and retry, reader thread), `Handshaker` (finds a board and agrees a line rate), and `SerialPort` with one implementation per platform (`serial_posix.cpp`, `serial_win32.cpp`) and port listing (`ports_*.cpp`). | `proto/mimhid_proto.h` |

`proto/mimhid_proto.h` is shared with the firmware: frame types, limits and the PONG layout. `c_api_device.cpp` checks the copies of these constants in `mimhid.h` against it with `static_assert`. No serial-port type appears above `src/transport/serial_port.hpp`.

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
| `Link::tx_mutex_` | One frame on the wire at a time | Reports, commands and control replies all go through it. |
| `Link::cmd_mutex_` | One command in flight | A command waits for its ACK or NAK while holding it. |
| `Link::state_mutex_`, `state_cv_` | The reply slot (`reply_ready_`, `reply_nak_`, `reply_code_`) and the pong | The reader thread fills the slot; the command waits on the condition variable. |
| `reconfiguring`, `closing`, `latched`, `loss_reported`, `control_routed` | Flags the reader thread and the caller both read | Atomics. `closing` is set once by close; `latched` holds `MIMHID_ERR_NO_DEVICE` after a serial failure. |
| `mimhid_node::transfer_inflight` and the `completion_*` fields | The one report a Node may have in flight | `exchange(true)` on entry is the check that refuses a second submit with `MIMHID_ERR_BUSY`. |

Order, where they nest: `cmd_mutex_`, then `tx_mutex_`, then `state_mutex_` (`Link::ping` waits for the pong holding `tx_mutex_`). `nodes_mutex` is never held across a `Link` call.

Apart from these, the library relies on the caller. A Device and its Nodes form one domain, and the caller makes sure no two calls in a domain run at once (see [API.md](API.md)). The Node mutators (`mimhid_keyboard_key` and the others in `state.cpp`) take no lock for that reason. The reader thread touches only the atomics, `nodes_mutex`, the `Link` internals and the event sink; it never reads Node state.

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
8. With `transport.validate_reports` 1, `validate_serialized_report` checks them against the Spec.
9. `write_report` calls `Link::send_report` (one `write` under `tx_mutex_`, bounded by the caller's timeout) and settles the Node with `transfer_complete`.

Reports are not acknowledged by the board. A failed write rolls the scan time back and leaves the pending state in place, so the same call can be repeated. `mimhid_node_submit_blocking` repeats `mimhid_node_submit` until the Node has nothing left to send or the deadline passes (`flush_node`).

## Commit

`commit_device` applies every staged change and is the only place the board's configuration changes. It holds a `ReconfigGuard`, which sets `reconfiguring` so input calls return `MIMHID_ERR_BUSY` meanwhile. It sends `DEVICE_CONFIG`, one `NODE_BEGIN` and its `NODE_DATA` frames per live Node, the Feature answers, and `COMMIT`, then waits for the ACK the board sends after it has re-enumerated. Each command is tried up to `kCommandAttempts` (3) times with the same sequence number; the board answers a repeated number again instead of running the command twice. With no live Node left, it sends a `COMMIT` that detaches the board from the bus.

`mimhid_device_batch_begin` sets `batch_open`; while it is set, Node open and close only stage their change (`staged_only`, `dirty`) and `mimhid_device_batch_end` commits once.

## Events

The reader thread (`Link::reader_loop`) feeds bytes to a `FrameParser`, completes the pending command on an ACK or NAK, and passes every other frame to `handle_frame`, which hands events to `deliver_event` in `device_ops.cpp`. That function turns a `BoardEvent` into a `mimhid_event`, finds the Node by interface under `nodes_mutex`, and calls `transport.event_sink` on the reader thread. The data pointer is valid only during the call. The one library call the sink may make is `mimhid_device_control_reply`, which only writes a frame (`Link::send_control_reply`) and never waits for an answer.

`FrameParser` keeps a 16 KiB ring. It skips garbage one byte at a time, accepts only the frame types a board sends, and drops an incomplete frame byte by byte when the line has been idle.

## Close

`mimhid_device_close` sets `closing` first, so later calls on the Device fail, then sends the neutral report of every Node (within the drain timeout), takes the board off the bus unless `keep_attached` is 1, puts the board back on the boot rate, stops the reader thread and closes the port. A failed step does not stop the next one, and the first error is returned.

## Language bindings

`tools/bindings/generate.py` reads `include/mimhid.h` through `clang -Xclang -ast-dump=json` (structures, function pointer types, exported functions, enumerations) and `clang -E -dM` (integer macros), and writes seven files: the C layout oracle `tests/abi/layout_oracle.c`, and for each of Python, C# and Rust the declarations and a layout test. Nothing in them is written by hand, so a change to the header is a regeneration, not a transcription.

- An enumeration is the anonymous `enum` after an `int32_t` typedef; it becomes `int32_t` plus constants (C#: an `enum : int` whose member names drop the prefix the constants share).
- Only exported functions are bound: `static inline` functions are skipped.
- In C#, a function with a structure pointer has two forms with the same entry point: `in`/`ref` for the common case, `nint` where `null` is allowed. An array in a structure is a `fixed` buffer, which keeps the structure blittable (no marshalling copy, and a default structure is valid). A `size_t*` named `out_*` is `out nuint`, any other is `ref nuint`.
- `bindings/python/tests/test_contract.py` is hand-written: what the package exports, and that the binding fills no option field. The layout tests also check that a new options structure has `struct_size` 0.
- `tools/bindings/generate.py` is shared with libaoahid (the two copies differ only in the copyright line); `TYPE_RENAMES` and `CS_MEMBER_RENAMES` hold the few names that are not the C name with its prefix cut, kept because libaoahid published them first.
- The layout test of each language runs the oracle and compares every structure size, field offset and constant. `generate.py --check` fails when a committed file differs from the header; CI runs both (`bindings` job).
- `tools/release/version_files.py` lists the version sites of the hand-written files; the constants inside the generated files are not on it, because `--check` pins them to the header.
- The generator refuses what it cannot bind (an unknown type, a field named like a Python keyword, two types with one short name) instead of guessing.

## How the tests reach this code

- `mimhid.unit` links `mimhid_test_objects`, a second compile of the sources, so it can call the internal functions (`serialize_node`, `FrameParser`, the descriptor builder) that the shared library does not export.
- `mimhid.e2e-*` runs the real firmware core against the host library through a pseudo-terminal (`tests/e2e/fake_board.c`). It needs a TinyUSB checkout (`MIMHID_TINYUSB_PATH`).
- `tests/fuzz/fuzz_descriptor.cpp` (`MIMHID_BUILD_FUZZ=ON`, Clang) feeds the descriptor builder, the serializer, the raw-descriptor validator and the `FrameParser`.
- `bindings/*/tests` compare each binding's layout with `tests/abi/layout_oracle.c`, compiled from the header.
- `mimhid.example-profile-*` build a Spec of every profile kind and check its manifest against the descriptor.
- CI formats with clang-format 14 and lints the production sources with clang-tidy 14 (`.clang-tidy`; the Windows sources cannot be parsed on Linux and are skipped). A newer version of either tool reports and lays out some things differently. `bugprone-easily-swappable-parameters` is silenced with `NOLINTNEXTLINE` on functions whose parameters follow the wire or the C ABI order, as in libaoahid; the other checks are fixed in the code.
