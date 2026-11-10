# API Reference

`libmimhid` exposes a stable C11 ABI in [include/mimhid.h](../include/mimhid.h)
and an optional header-only C++20 facade in
[include/mimhid.hpp](../include/mimhid.hpp). Every exported symbol begins with
`mimhid_`. The function reference below follows the comment above each
declaration in `mimhid.h`; where this page and the header differ, the code
decides, and the difference is a bug in one of them.

## Objects and ownership

The API has four opaque handle types:

| Handle | Created by | Released by | Ownership and lifetime |
|---|---|---|---|
| `mimhid_discovery*` | `mimhid_discover` | `mimhid_discovery_destroy` | A snapshot of the `mimhid_device_info` records found by one search. Independent of any Device. |
| `mimhid_spec*` | `mimhid_spec_create_*` | `mimhid_spec_release` | An immutable, reference-counted HID report descriptor and report layout. Independent of any Device; one Spec may be used by several Nodes and threads. |
| `mimhid_device*` | `mimhid_device_open` | `mimhid_device_close` | One board on one serial port: the port, the reader thread, and the USB device the board presents. Closing it closes its Nodes. |
| `mimhid_node*` | `mimhid_node_open`, `mimhid_node_open_raw` | `mimhid_node_close` or `mimhid_device_close` | One HID collection on one of the board's USB interfaces. A Node opened from a Spec holds a reference to it. After `mimhid_device_close` its Nodes are gone and their handles must not be used. |

## Rules every function follows

### Return codes and `mimhid_last_error()`

Every function that can fail returns a `mimhid_result` (`MIMHID_OK`, 0, on
success) or, for the few that return a value, says in its Returns line what it
returns on failure. Every call except `mimhid_last_error` replaces the calling
thread's record, `mimhid_result_name` and `mimhid_frame_type_name` included, so
read the record before making another call. `mimhid_last_error()` returns
`const mimhid_error_detail*`:

- `code`: the `mimhid_result` of the last call (`MIMHID_OK` when it succeeded).
- `field`: a static string naming the argument, field, or step concerned (for
  example `"usb_device.vendor_id"` or `"keyboard.usage_range"`), or `NULL`.
- `reason`: one English sentence.
- `transport_status`: the `errno` or Win32 error of a failed serial call, or the
  board's `MIMHID_NAK_*` code (`proto/mimhid_proto.h`: 1 `PARAM`, 2 `OVERFLOW`,
  3 `STATE`, 4 `DESCRIPTOR`, 5 `UNSUPPORTED`, 6 `BUSY`).
- `frame_type`: the serial frame concerned (`mimhid_frame_type_name`).
- `node_id`, `report_id`, `offset`, `length`: set for Node, report, and
  descriptor errors.

A `NAK` from the board becomes a result like this (`command_error` in
`src/api/device_ops.cpp`):

| Board answer | Result |
|---|---|
| `MIMHID_NAK_PARAM` | `MIMHID_ERR_PARAM` |
| `MIMHID_NAK_OVERFLOW` | `MIMHID_ERR_OVERFLOW` |
| `MIMHID_NAK_STATE`, `MIMHID_NAK_BUSY` | `MIMHID_ERR_BUSY` |
| `MIMHID_NAK_DESCRIPTOR` | `MIMHID_ERR_DESCRIPTOR_REJECTED` |
| `MIMHID_NAK_UNSUPPORTED` | `MIMHID_ERR_UNSUPPORTED` |
| any other code | `MIMHID_ERR_INTERNAL` |
| no answer within the timeout | `MIMHID_ERR_TIMEOUT` |
| port closed or unplugged | `MIMHID_ERR_NO_DEVICE` |
| write cut short | `MIMHID_ERR_SHORT_TRANSFER` |
| other serial error | `MIMHID_ERR_IO` |

### Structures and initial values (`*_init`)

Every options or output structure with `struct_size` has a static inline
`mimhid_<type>_init` in `mimhid.h` (`mimhid_device_config_init`,
`mimhid_commit_timeouts_init`, `mimhid_node_config_init`,
`mimhid_board_limits_init`, `mimhid_link_counters_init`,
`mimhid_keyboard_options_init`, `mimhid_mouse_options_init`,
`mimhid_toggle_options_init`, `mimhid_gamepad_options_init`,
`mimhid_touchscreen_options_init`, `mimhid_touchpad_options_init`,
`mimhid_pen_options_init`, `mimhid_digitizer_options_init`,
`mimhid_raw_options_init`, and `mimhid_capability_manifest_init`).

> [!IMPORTANT]
> Every `*_init` function **only zeroes the structure and sets `struct_size`**.
> It fills in no vendor ID, baud rate, timeout, endpoint address, or profile
> range. A required field left zero is refused with `MIMHID_ERR_UNSET_FIELD`.
> Every enumeration starts at 1, so a zeroed field never selects a mode by
> accident. `reserved` fields must stay zero, and a flag must be 0 or 1.

| Structure | Required (zero is refused) | Optional |
|---|---|---|
| `mimhid_transport_options` | `baud_rate` (or `MIMHID_BAUD_FASTEST`), `handshake_timeout_ms`, `maximum_report_bytes`, the six `linux_hid` fields, `log_level` (`MIMHID_LOG_DISABLED` to `MIMHID_LOG_TRACE`; any level but `MIMHID_LOG_DISABLED` also needs `log_sink`) | `validate_reports`, `keep_attached`, `event_sink`, `event_user`, `log_user` |
| `mimhid_usb_device` | `bcd_usb`, `ep0_max_packet_size` (8, 16, 32 or 64; 64 on a high-speed board), `vendor_id`, `language_id` | `device_class`, `device_subclass`, `device_protocol`, `product_id`, `bcd_device`, `manufacturer`, `product`, `serial`, `serial_from_chip_id` |
| `mimhid_usb_config` | `configuration_value` (must be 1), `bm_attributes` (bit 7 set, bits 4..0 clear) | `bmax_power` (0 to 250, in 2 mA units), `string` |
| `mimhid_commit_timeouts` | `control_timeout_ms`, `reconnect_timeout_ms` | — |
| `mimhid_node_config` | `iface.class_code` (must be `0x03`), `iface.bcd_hid`, `ep_in.address` (`0x81` to `0x8F`), `ep_in.poll_interval_ms`, `ep_in.max_packet_size` | `iface.number`, `iface.subclass`, `iface.protocol`, `iface.country_code`, `iface.name`, `ep_out` (all zero when there is no OUT endpoint) |
| `mimhid_control_rule` | `timeout_ms` (1 to `MIMHID_CONTROL_MAX_TIMEOUT_MS`), `when_late` (a `MIMHID_CONTROL_ACTION_*`) | the masks and values of `bmRequestType`, `bRequest`, `wValue`, and `wIndex` |

### The close rule

`mimhid_device_close` and `mimhid_node_close` follow one rule:

- `MIMHID_ERR_PARAM` or `MIMHID_ERR_UNSET_FIELD` (a null or closed handle, a
  zero timeout): **nothing was done** and the handle is still the caller's.
- **Any other result:** the object is gone and its handle must not be used
  again; an error says what failed on the way out.

`mimhid_discovery_destroy` and `mimhid_spec_release` return nothing: for a
non-null handle they always free the snapshot or drop the reference.

Writes to the serial port finish within the call, so a close never leaves
anything pending.

### Threading and domains

- **Device domain:** a `mimhid_device*` and its `mimhid_node*` handles form one
  domain. No two calls in one domain may run at the same time, except where a
  function says so (`mimhid_device_is_ready` from any thread while the Device is
  open, and `mimhid_device_control_reply` from inside `transport.event_sink`).
- **Outside any domain:** Specs and Discovery snapshots. `mimhid_spec_retain`,
  `mimhid_spec_release`, `mimhid_spec_descriptor`, and `mimhid_spec_manifest` may
  be called from any thread.
- **Callbacks (`event_sink`, `log_sink`):** they run on the Device's reader
  thread or on the thread making the call. `event->data` is valid only during
  the callback. A sink must not call back into `libmimhid`, except
  `mimhid_device_control_reply` from the event sink.

---

## Lifecycle and USB bus behavior

### 1. Before the first commit (`0 V` stealth state)

From power-up, and after `mimhid_device_open` has found the board, the board's
USB `D+` pull-up stays off (`0 V`): the target sees no USB device.
`mimhid_device_open` sends no configuration.

### 2. Opening Nodes and committing

`mimhid_node_open`, `mimhid_node_open_raw`, `mimhid_node_close`,
`mimhid_device_set_usb_identity`, `mimhid_device_batch_end`, and
`mimhid_device_commit` commit (`commit_device` in `src/api/device_ops.cpp`):

1. When Nodes are left, the library sends `DEVICE_CONFIG`, then for each Node
   `NODE_BEGIN`, its descriptor in `NODE_DATA` chunks, a `FEATURE` frame for
   each Feature answer, and `QUEUE_DEPTH` when a depth was set. Each frame waits
   up to `control_timeout_ms` for the board's answer and is sent up to three
   times. The board stores these frames; nothing changes on the target yet.
2. When the board's USB device is already on the target, the library sends a
   detach `COMMIT` (D+ off), waiting up to `control_timeout_ms`, and then waits
   `reconnect_delay_ms` on the host when `mimhid_device_commit` was given one
   (the other calls use 0).
3. The library sends an attach `COMMIT` and waits up to `reconnect_timeout_ms`
   for its answer. The board builds the USB descriptors from what it stored,
   turns its D+ pull-up on, and answers. The call returns then: it does not
   wait for the target to enumerate the device. `mimhid_device_wait_ready`
   waits for that (the board's `MOUNTED` event, sent when the target selects
   the configuration).

When the last Node is closed, the library sends only a detach `COMMIT`, and the
board's USB device leaves the target.

### 3. Sharing and separating USB interfaces

- Up to `MIMHID_MAX_NODES` (16) Nodes can be open on a Device, on at most the
  board's `max_interfaces` interfaces (15 on the RP2040 and RP2350, 7 on the
  nRF52840, 4 on the ESP32-S3, 3 on the STM32F4; see [LIMITS.md](LIMITS.md)).
- Nodes with the **same** `iface.number` share one USB HID interface: their
  `iface`, `ep_in`, and `ep_out` settings must match, and each must have a
  Report ID, all different.
- Nodes on **different** interfaces need different endpoint numbers (1 to the
  board's `max_endpoint_number`).

---

## Function reference

Each entry gives the declaration and the four lines of the comment above it in
`mimhid.h`: what the call owns or copies, what it waits for, which threads may
call it, and what it returns. Any call can also return `MIMHID_ERR_INTERNAL`,
and the link errors (`MIMHID_ERR_TIMEOUT`, `MIMHID_ERR_NO_DEVICE`,
`MIMHID_ERR_SHORT_TRANSFER`, `MIMHID_ERR_IO`) come from any call that talks to
the board. Per-profile rules for the Spec factories and input calls are in
[PROFILES.md](PROFILES.md).

### Diagnostics and version

#### `mimhid_last_error`

```c
const mimhid_error_detail* mimhid_last_error(void);
```

- **Ownership:** Returns the calling thread's record, valid until that thread's next call.
- **Blocking:** None.
- **Synchronization:** The record belongs to the calling thread; do not pass it to another.
- **Returns:** Never null.

#### `mimhid_result_name`

```c
const char* mimhid_result_name(mimhid_result result);
```

- **Ownership:** The string is static.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** The constant's name, such as "`MIMHID_ERR_TIMEOUT`", with `MIMHID_OK` recorded; for a value that is not a result, "`MIMHID_RESULT_UNKNOWN`" with `MIMHID_ERR_PARAM` recorded.

#### `mimhid_frame_type_name`

```c
const char* mimhid_frame_type_name(int32_t frame_type);
```

- **Ownership:** The string is static.
- **Blocking:** None.
- **Synchronization:** Any thread. Names the serial frame type of `mimhid_error_detail`.frame_type (docs/PROTOCOL.md).
- **Returns:** The frame's name, such as "COMMIT" or "REPORT", with `MIMHID_OK` recorded; for a value that is not a frame type, "UNKNOWN" with `MIMHID_ERR_PARAM` recorded.

#### `mimhid_version`

```c
uint32_t mimhid_version(void);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** (major << 24) | (minor << 16) | patch of the library that is running.

### Discovery

#### `mimhid_discover`

```c
mimhid_result mimhid_discover(uint32_t timeout_ms, mimhid_discovery** out_discovery);
```

- **Ownership:** The caller destroys the snapshot with `mimhid_discovery_destroy`.
- **Blocking:** Searches every USB serial port at once, one thread each, waiting up to timeout_ms for each answer; a port another program has open is skipped. On each port it pings at the boot rate and, when nothing answers, sends a BREAK (which returns a board a program left on a fast rate to the boot rate) and pings again, so a serial device that is not a board receives a few pings and a BREAK. On a board it measures the fastest rate the link carries: from the board's max_baud downward, it sends bursts of test frames and trusts a rate only when the board's own error counters stay at zero (baud_rate), and it leaves the board on the boot rate. The search takes a few seconds at most. A board whose firmware has another major or minor version is listed with only its version and firmware_compatible 0, so that it can be flashed again.
- **Synchronization:** Any thread.
- **Returns:** `MIMHID_OK`, also when no board was found; `MIMHID_ERR_PARAM` for a null out_discovery; `MIMHID_ERR_UNSET_FIELD` for a zero timeout.

#### `mimhid_discovery_count`

```c
size_t mimhid_discovery_count(const mimhid_discovery* discovery);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread, but not during `mimhid_discovery_destroy`.
- **Returns:** The number of boards with `MIMHID_OK` recorded, or 0 with `MIMHID_ERR_PARAM` recorded for a null snapshot.

#### `mimhid_discovery_get`

```c
const mimhid_device_info* mimhid_discovery_get(const mimhid_discovery* discovery, size_t index);
```

- **Ownership:** The entry belongs to the snapshot.
- **Blocking:** None.
- **Synchronization:** As `mimhid_discovery_count`.
- **Returns:** The entry with `MIMHID_OK` recorded, or null with `MIMHID_ERR_PARAM` recorded for a null snapshot or an index out of range.

#### `mimhid_discovery_destroy`

```c
void mimhid_discovery_destroy(mimhid_discovery* discovery);
```

- **Ownership:** Frees the snapshot and every entry of it.
- **Blocking:** None.
- **Synchronization:** No other call may use the snapshot meanwhile.
- **Returns:** Nothing. `MIMHID_OK` is recorded, or `MIMHID_ERR_PARAM` for null (nothing is done).

### Device lifecycle, commit, bus control, and queries

#### `mimhid_device_open`

```c
mimhid_result mimhid_device_open(const char* port_path, const mimhid_device_config* config, mimhid_device** out_device);
```

- **Ownership:** Copies config; the caller closes the Device with `mimhid_device_close`.
- **Blocking:** Opens port_path (such as "/dev/ttyACM0" or "COM3") for this process alone, looks for the board at the boot rate (and after a BREAK, for one left on a fast rate; waiting transport.handshake_timeout_ms for each answer), reads its limits, moves the link to transport.baud_rate and checks it with a short burst of test frames (`MIMHID_BAUD_FASTEST` measures instead, which takes up to a few seconds), and starts the reader thread. It sends no configuration: the board's USB device stays off the target until a Node is opened.
- **Synchronization:** Any thread, one call per port.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null pointer, a wrong struct_size or a bad value; `MIMHID_ERR_UNSET_FIELD` for a required field left zero; `MIMHID_ERR_UNSUPPORTED` for a rate the board cannot make, or one the adapter or wiring did not carry; `MIMHID_ERR_OVERFLOW` for a maximum_report_bytes the board does not take; `MIMHID_ERR_TIMEOUT` when no board answered; `MIMHID_ERR_PROTOCOL` for firmware of another major or minor version; `MIMHID_ERR_ACCESS` for a port that cannot be opened (it does not exist, the permission is missing, or another program has it); `MIMHID_ERR_IO` for a port that cannot be set up.

#### `mimhid_device_close`

```c
mimhid_result mimhid_device_close(mimhid_device* device, uint32_t control_timeout_ms, uint32_t drain_timeout_ms);
```

- **Ownership:** Follows the "Closing" rule; the Device's Nodes go with it.
- **Blocking:** Sends the neutral report of every open Node (keys and buttons up, contacts lifted, axes at rest) within drain_timeout_ms, takes the board's USB device off the target unless transport.keep_attached is 1, removes the control routing and loss reporting, puts the board back on the boot rate and closes the port. Each configuration frame waits up to control_timeout_ms. A failed step does not stop the next one.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null or closing Device; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_NO_DEVICE` when the link was already gone, so the board was not reset; otherwise the first link error on the way out.

#### `mimhid_device_set_usb_identity`

```c
mimhid_result mimhid_device_set_usb_identity(mimhid_device* device, const mimhid_usb_device* usb_device, const mimhid_usb_config* usb_config, const mimhid_commit_timeouts* timeouts);
```

- **Ownership:** Copies usb_device and usb_config.
- **Blocking:** Commits (see [Lifecycle and USB bus behavior](#lifecycle-and-usb-bus-behavior)), waiting as timeouts says; inside a batch it only stages the change.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null pointer, a wrong struct_size or a bad value; `MIMHID_ERR_UNSET_FIELD` for a required field or timeout left zero; the results of `mimhid_device_commit`.

#### `mimhid_device_latched_error`

```c
mimhid_result mimhid_device_latched_error(mimhid_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Reads the error the reader thread recorded for the link, and forgets it unless it is `MIMHID_ERR_NO_DEVICE`, which stays.
- **Returns:** `MIMHID_OK` when there is none; `MIMHID_ERR_PARAM` for an invalid Device; `MIMHID_ERR_NO_DEVICE` after the port failed or was unplugged; `MIMHID_ERR_IO` for another link error.

#### `mimhid_device_is_ready`

```c
int32_t mimhid_device_is_ready(const mimhid_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread while the Device is open.
- **Returns:** 1 while the target has the board's USB device mounted and takes reports, else 0.

#### `mimhid_device_wait_ready`

```c
mimhid_result mimhid_device_wait_ready(mimhid_device* device, uint32_t timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Until the target mounts the board's USB device, or timeout_ms.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK` once mounted; `MIMHID_ERR_PARAM` for an invalid Device; `MIMHID_ERR_TIMEOUT`; `MIMHID_ERR_NO_DEVICE` when the link fails meanwhile.

#### `mimhid_device_batch_begin`

```c
mimhid_result mimhid_device_batch_begin(mimhid_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None; nothing is sent. Until `mimhid_device_batch_end`, `mimhid_node_open`, `mimhid_node_close` and `mimhid_device_set_usb_identity` only stage their change, and a Node opened in the batch refuses reports with `MIMHID_ERR_BUSY`.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device; `MIMHID_ERR_BUSY` inside a batch.

#### `mimhid_device_batch_end`

```c
mimhid_result mimhid_device_batch_end(mimhid_device* device, const mimhid_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** Commits every change staged since `mimhid_device_batch_begin` at once, waiting as timeouts says.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device, a null timeouts or a wrong struct_size; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_BUSY` outside a batch; the results of `mimhid_device_commit`.

#### `mimhid_device_commit`

```c
mimhid_result mimhid_device_commit(mimhid_device* device, uint32_t reconnect_delay_ms, const mimhid_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** Sends the configuration of every open Node and commits, waiting as timeouts says. `mimhid_node_open`, `mimhid_node_close` and `mimhid_device_batch_end` do this already; call it to retry after a failed commit, or for reconnect_delay_ms: with a nonzero delay on an attached device the library takes the board's USB device off the target (D+ at 0 V), waits that many milliseconds on the host, and then puts it back. Inside a batch it commits the staged changes and ends the batch.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device or timeouts; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_OVERFLOW` when the Nodes do not fit the board; `MIMHID_ERR_DESCRIPTOR_REJECTED` when the board refuses the configuration; a link error.

#### `mimhid_device_connect`

```c
mimhid_result mimhid_device_connect(mimhid_device* device, const mimhid_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** Puts the board's USB device on the target (D+ pull-up on) and waits for the board's answer. A Device that has not been configured yet commits instead.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device or timeouts; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; a link error; the results of `mimhid_device_commit` when it commits.

#### `mimhid_device_disconnect`

```c
mimhid_result mimhid_device_disconnect(mimhid_device* device, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Takes the board's USB device off the target (D+ at 0 V, as if unplugged), waiting up to control_timeout_ms for the board's answer. The next connect or commit configures it again.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; a link error.

#### `mimhid_device_remote_wakeup`

```c
mimhid_result mimhid_device_remote_wakeup(mimhid_device* device, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Waits up to control_timeout_ms for the board's answer.
- **Synchronization:** Device domain. Wakes a target that suspended the board's USB device; the target must have enabled remote wakeup (usb_config.bm_attributes bit 5 and `MIMHID_EVENT_SUSPEND` with value 1).
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_BUSY` when the device is not suspended; a link error.

#### `mimhid_device_board_info`

```c
mimhid_result mimhid_device_board_info(const mimhid_device* device, mimhid_device_info* out_info);
```

- **Ownership:** Fills the caller's out_info.
- **Blocking:** None; a copy of what the board reported at open.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device or a null out_info.

#### `mimhid_device_limits`

```c
mimhid_result mimhid_device_limits(const mimhid_device* device, mimhid_board_limits* out_limits);
```

- **Ownership:** Fills the caller's out_limits (set its struct_size; `mimhid_board_limits_init` does).
- **Blocking:** None; a copy of what the board reported at open.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device, a null out_limits or a wrong struct_size.

#### `mimhid_device_link_counters`

```c
mimhid_result mimhid_device_link_counters(mimhid_device* device, uint32_t reset, uint32_t control_timeout_ms, mimhid_link_counters* out_counters);
```

- **Ownership:** Fills the caller's out_counters (set its struct_size; `mimhid_link_counters_init` does).
- **Blocking:** One command to the board, waiting up to control_timeout_ms for its answer.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device, a null out_counters or a wrong struct_size; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_TIMEOUT` or `MIMHID_ERR_IO` when the board did not answer. With reset nonzero the board's counters are zeroed after being read: a program that wants to see a link degrade reads them now and then. Any lost byte shows there, not in the report stream.

### Control transfers of the target (endpoint 0)

#### `mimhid_device_set_control_routing`

```c
mimhid_result mimhid_device_set_control_routing(mimhid_device* device, uint32_t report_all, const mimhid_control_rule* rules, size_t rule_count, uint32_t control_timeout_ms);
```

- **Ownership:** Copies rules.
- **Blocking:** Waits up to control_timeout_ms for the board's answer (a rule's timeout_ms is the board's wait for the program, a different one).
- **Synchronization:** Device domain. Replaces the whole set. report_all 1 reports every control transfer, held or not; with no rules and report_all 0 the board handles everything itself, as it does at first. A held request delays only the target's control transfer, never the reports, but while the board waits for the verdict of a host-to-device transfer its USB task does nothing else. SET_ADDRESS and SET_CONFIGURATION are never held, nor a host-to-device data stage longer than max_control_data_bytes. The rules stay until replaced or `mimhid_device_close`.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device, report_all, count or rule (a value outside its mask, a timeout over `MIMHID_CONTROL_MAX_TIMEOUT_MS`, an unknown action); `MIMHID_ERR_UNSET_FIELD` for a zero control_timeout_ms, rule timeout_ms or when_late; `MIMHID_ERR_OVERFLOW` for more rules than the board holds; a link error.

#### `mimhid_device_control_reply`

```c
mimhid_result mimhid_device_control_reply(mimhid_device* device, uint32_t token, mimhid_control_action action, const uint8_t* data, size_t length, uint32_t send_timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** Writes one frame, for up to send_timeout_ms without progress; it does not wait for the board.
- **Synchronization:** Device domain, and also from the event sink, which is the way to answer in time. Answers the held transfer of token (from `MIMHID_EVENT_CONTROL_SETUP`, or from `MIMHID_EVENT_CONTROL_DATA` for the verdict of a host-to-device transfer): - ANSWER to a device-to-host request: data is the data stage, sent whole whatever wLength says. More than wLength makes the host report an overrun; data with wLength 0 still starts a data stage, which is how a device that does so is copied. Empty data is an empty data stage. - ANSWER to any other request: data must be empty. With a host-to-device data stage the board takes the data, reports it as `MIMHID_EVENT_CONTROL_DATA` and waits for the verdict: ANSWER (acknowledge), STALL, or the rule's when_late. - STALL, DROP: data must be empty. - LEAVE: the board's own handling; as a verdict it acknowledges. A reply the board no longer waits for (too late, or the target gave up) is dropped.
- **Returns:** `MIMHID_OK` once the frame is written; `MIMHID_ERR_PARAM` for an invalid Device, a token of 0 or above 255, an unknown action, or data with an action that takes none; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_OVERFLOW` for data longer than max_control_data_bytes; a link error.

#### `mimhid_device_set_loss_reporting`

```c
mimhid_result mimhid_device_set_loss_reporting(mimhid_device* device, uint32_t enabled, uint32_t interval_ms, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Waits up to control_timeout_ms for the board's answer.
- **Synchronization:** Device domain. enabled 1 makes the board report the reports it drops because its queue is full (`MIMHID_EVENT_REPORT_LOST`), at most one event per interval_ms (0 to `MIMHID_LOSS_MAX_INTERVAL_MS`; 0 reports each time at once). enabled 0, which is how the board starts, needs interval_ms 0 and puts nothing extra on the link. The setting stays until replaced or `mimhid_device_close`.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Device, enabled or interval_ms; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; a link error.

### Spec factories and inspection

#### `mimhid_spec_create_keyboard`

```c
mimhid_result mimhid_spec_create_keyboard(const mimhid_keyboard_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`; `MIMHID_ERR_PARAM` also for a range that includes 0x00 to 0x03 or the modifiers.

#### `mimhid_spec_create_mouse`

```c
mimhid_result mimhid_spec_create_mouse(const mimhid_mouse_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_toggle`

```c
mimhid_result mimhid_spec_create_toggle(const mimhid_toggle_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`; `MIMHID_ERR_UNSUPPORTED` for a semantic that press and release cannot express, or a Camera Control Usage that is not `MIMHID_USAGE_ONE_SHOT`.

#### `mimhid_spec_create_gamepad`

```c
mimhid_result mimhid_spec_create_gamepad(const mimhid_gamepad_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_touchscreen`

```c
mimhid_result mimhid_spec_create_touchscreen(const mimhid_touchscreen_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_touchpad`

```c
mimhid_result mimhid_spec_create_touchpad(const mimhid_touchpad_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_pen`

```c
mimhid_result mimhid_spec_create_pen(const mimhid_pen_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_digitizer`

```c
mimhid_result mimhid_spec_create_digitizer(const mimhid_digitizer_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_spec_create_raw`

```c
mimhid_result mimhid_spec_create_raw(const mimhid_raw_options* options, mimhid_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`; `MIMHID_ERR_PARAM` also for a descriptor that does not parse or does not match the report table; `MIMHID_ERR_UNSUPPORTED` for Output or Feature items without the matching requires_* flag, a long item, or a construct the parser does not handle.

#### `mimhid_spec_retain`

```c
void mimhid_spec_retain(mimhid_spec* spec);
```

- **Ownership:** Adds a reference for the caller, who must already hold one.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** Nothing. `MIMHID_OK` is recorded; `MIMHID_ERR_PARAM` for null, or `MIMHID_ERR_OVERFLOW` when the count is full (no reference is added).

#### `mimhid_spec_release`

```c
void mimhid_spec_release(mimhid_spec* spec);
```

- **Ownership:** Gives up one of the caller's references; the last one frees the Spec. A Node keeps its own reference until it closes.
- **Blocking:** None.
- **Synchronization:** Any thread, as long as the last release does not race any other use.
- **Returns:** Nothing. `MIMHID_OK` is recorded, or `MIMHID_ERR_PARAM` for null (nothing is done).

#### `mimhid_spec_descriptor`

```c
mimhid_result mimhid_spec_descriptor(const mimhid_spec* spec, const uint8_t** bytes, size_t* length);
```

- **Ownership:** The bytes belong to the Spec.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null pointer.

#### `mimhid_spec_manifest`

```c
mimhid_result mimhid_spec_manifest(const mimhid_spec* spec, mimhid_capability_manifest* manifest);
```

- **Ownership:** Fills the caller's manifest; manifest->reports belongs to the Spec.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null pointer, a wrong struct_size or a nonzero reserved.

### Node lifecycle, configuration, and submission

#### `mimhid_node_open`

```c
mimhid_result mimhid_node_open(mimhid_device* device, mimhid_spec* spec, const mimhid_node_config* config, const mimhid_commit_timeouts* timeouts, mimhid_node** out_node);
```

- **Ownership:** Copies config; the Node keeps a reference to spec until it closes.
- **Blocking:** Sends the configuration of every Node and commits (see [Lifecycle and USB bus behavior](#lifecycle-and-usb-bus-behavior)), waiting as timeouts says, so the target sees the board's USB device unplugged and plugged in again and every other Node is briefly away. Inside a batch it only stages the Node; timeouts is still required. It returns once the board has answered the attach `COMMIT`, which it does right after turning its D+ pull-up on; `mimhid_device_wait_ready` waits for the target to mount the device.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null argument, a wrong struct_size, a bad value, or Nodes sharing an interface that do not follow the rules of `mimhid_usb_interface`; `MIMHID_ERR_UNSET_FIELD` for a required field or timeout left zero; `MIMHID_ERR_OVERFLOW` when the board has no room for the Node, its interface or its descriptor, or the Spec exceeds maximum_report_bytes or a linux_hid limit; the results of `mimhid_device_commit`.

#### `mimhid_node_open_raw`

```c
mimhid_result mimhid_node_open_raw(mimhid_device* device, const uint8_t* descriptor, size_t descriptor_length, const mimhid_node_config* config, const mimhid_commit_timeouts* timeouts, mimhid_node** out_node);
```

- **Ownership:** Copies descriptor and config.
- **Blocking:** As `mimhid_node_open`. The descriptor is sent as it is, neither parsed nor checked, so the Node has no input calls: send its reports with `mimhid_report_send`.
- **Synchronization:** Device domain.
- **Returns:** As `mimhid_node_open`, `MIMHID_OK` through the results of `mimhid_device_commit`; `MIMHID_ERR_PARAM` also for a null or empty descriptor.

#### `mimhid_node_close`

```c
mimhid_result mimhid_node_close(mimhid_node* node, const mimhid_commit_timeouts* timeouts, uint32_t drain_timeout_ms);
```

- **Ownership:** Follows the "Closing" rule.
- **Blocking:** Finishes a touch frame that has started and sends the neutral report (keys and buttons up, contacts lifted, axes at rest) within drain_timeout_ms, then commits the remaining Nodes as `mimhid_node_open` does; closing the last Node takes the USB device off the target. Inside a batch it only stages the removal. A failed step does not stop the next one; after a failed commit the target may still see the Node until a commit succeeds (`mimhid_device_commit`).
- **Synchronization:** Device domain; not from the event sink.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null or closed Node, a closing Device, or a wrong timeouts struct_size; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; otherwise the first error on the way out.

#### `mimhid_node_id`

```c
uint16_t mimhid_node_id(const mimhid_node* node);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain.
- **Returns:** The number the board knows the Node by, plus one (so it is nonzero), with `MIMHID_OK` recorded; or 0 with `MIMHID_ERR_PARAM` recorded for a null or closed Node.

#### `mimhid_node_set_feature_response`

```c
mimhid_result mimhid_node_set_feature_response(mimhid_node* node, uint8_t report_id, const uint8_t* data, size_t length, uint32_t control_timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** Waits up to control_timeout_ms for the board's answer; inside a batch, or for a Node not yet committed, it is sent with the commit instead.
- **Synchronization:** Device domain. Sets what the board answers (the bytes after the Report ID, at most the board's max_feature_bytes; `mimhid_device_limits`) when the target reads Feature report report_id of this Node. A touch Spec with enable_contact_count_maximum_feature_declaration has its answer set already.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Node, null data with a nonzero length, or more than max_feature_bytes; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_OVERFLOW` when the board has no room; a link error.

#### `mimhid_node_set_queue_depth`

```c
mimhid_result mimhid_node_set_queue_depth(mimhid_node* node, uint32_t depth, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** As `mimhid_node_set_feature_response`.
- **Synchronization:** Device domain. How many of this Node's reports may wait on the board for the target to read them; a report that finds the queue full is dropped (`MIMHID_EVENT_REPORT_LOST`). With depth 1 a report is at most one poll interval late. The board keeps one queue per interface, and Nodes that share an interface share the least depth any of them set. At first the queue holds all it can: 15 reports when the board's max_report_bytes is at most 64, else 7. The depth stays with the Node across commits.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Node or a depth of 0 or above 255; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_OVERFLOW` for a depth beyond the board's queue; a link error.

#### `mimhid_node_manifest`

```c
mimhid_result mimhid_node_manifest(const mimhid_node* node, mimhid_capability_manifest* manifest);
```

- **Ownership:** As `mimhid_spec_manifest`; manifest->reports is valid while the Node is open.
- **Blocking:** None.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a null pointer, a wrong struct_size or a nonzero reserved.

#### `mimhid_node_submit`

```c
mimhid_result mimhid_node_submit(mimhid_node* node, uint32_t send_timeout_ms);
```

- **Ownership:** The report is built in the library's own buffer.
- **Blocking:** Writes one report of the Node's current state, if it changed, to the serial port and returns once the driver has it (microseconds on an idle port). It never waits for the target; send_timeout_ms only bounds a write that makes no progress.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`, also when there was nothing to send; `MIMHID_ERR_PARAM` for an invalid Node; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_UNSUPPORTED` for a Raw Node (use `mimhid_raw_submit`); `MIMHID_ERR_BUSY` for a Node opened in a batch that has not ended, while the board enumerates, or while a report of the Node is being sent; a link error.

#### `mimhid_node_submit_blocking`

```c
mimhid_result mimhid_node_submit_blocking(mimhid_node* node, uint32_t timeout_ms);
```

- **Ownership:** As `mimhid_node_submit`.
- **Blocking:** Writes reports until the Node's state has been sent, a touch frame of several reports and a mouse's pending movement included, or timeout_ms has passed; each write is bounded by what is left of it.
- **Synchronization:** Device domain.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_TIMEOUT` when timeout_ms passed first; the results of `mimhid_node_submit`.

### Profile input calls

#### `mimhid_keyboard_key`

```c
mimhid_result mimhid_keyboard_key(mimhid_node* node, uint16_t usage, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) one Keyboard page Usage, a modifier or a key of the Spec's range. Closing the Node releases every key still down.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_mouse_move`

```c
mimhid_result mimhid_mouse_move(mimhid_node* node, int32_t dx, int32_t dy);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Adds to the pending X and Y movement. A report carries as much as the fields hold and keeps the rest for the next one.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`; `MIMHID_ERR_OVERFLOW` when the 64-bit pending total overflows.

#### `mimhid_mouse_scroll`

```c
mimhid_result mimhid_mouse_scroll(mimhid_node* node, int32_t wheel, int32_t pan);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Adds to the pending wheel and pan movement, as `mimhid_mouse_move` does.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`; `MIMHID_ERR_UNSUPPORTED` for a nonzero value on a field the Spec lacks; `MIMHID_ERR_OVERFLOW` when the 64-bit pending total overflows.

#### `mimhid_mouse_button`

```c
mimhid_result mimhid_mouse_button(mimhid_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) button 1 to button_count.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_toggle_usage`

```c
mimhid_result mimhid_toggle_usage(mimhid_node* node, uint16_t usage, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) one Usage of the Spec's allow-list; any number may be pressed at once. Closing the Node releases every Usage still down.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_gamepad_button`

```c
mimhid_result mimhid_gamepad_button(mimhid_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) button 1 to button_count.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_gamepad_axis`

```c
mimhid_result mimhid_gamepad_axis(mimhid_node* node, size_t axis_index, int32_t value);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets axes[axis_index] of the Spec to value, which must be in its logical range.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_gamepad_dpad`

```c
mimhid_result mimhid_gamepad_dpad(mimhid_node* node, uint32_t up, uint32_t down, uint32_t right, uint32_t left);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the D-pad from four 0/1 directions. With a Hat, up and down (or left and right) together are refused and no direction sends the Null state; with four buttons the bits go out as given.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`; `MIMHID_ERR_PARAM` also when the Spec has no D-pad.

#### `mimhid_touchscreen_contact`

```c
mimhid_result mimhid_touchscreen_contact(mimhid_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y, const mimhid_touchscreen_extra* extra);
```

- **Ownership:** Copies extra, which may be null (every optional field 0).
- **Blocking:** None.
- **Synchronization:** Device domain. down 1 places contact_id in a free slot, or moves it if it is already down; down 0 lifts it at x, y. A lifted contact_id can be placed again once its lift has been sent. While a frame split over several reports is being sent, every call is refused with `MIMHID_ERR_BUSY`.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`; `MIMHID_ERR_PARAM` also for lifting a contact_id that is not down; `MIMHID_ERR_BUSY` also for placing one whose lift is unsent; `MIMHID_ERR_OVERFLOW` when every slot is taken.

#### `mimhid_touchpad_contact`

```c
mimhid_result mimhid_touchpad_contact(mimhid_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y, const mimhid_touchpad_extra* extra);
```

- **Ownership:** Copies extra, which may be null.
- **Blocking:** None.
- **Synchronization:** Device domain. As `mimhid_touchscreen_contact`, for a Touchpad Node.
- **Returns:** As `mimhid_touchscreen_contact`, `MIMHID_OK` through `MIMHID_ERR_OVERFLOW`.

#### `mimhid_touchpad_button`

```c
mimhid_result mimhid_touchpad_button(mimhid_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) click button 1 to button_count.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_pen_update`

```c
mimhid_result mimhid_pen_update(mimhid_node* node, const mimhid_pen_sample* sample);
```

- **Ownership:** Copies sample.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the whole pen state. in_range 0 takes the pen away: In Range, Tip, pressure, Invert and the barrel bits go out as 0 with the sample's own X, Y, tilt and twist, so repeat the last position to leave the pointer where it was. Switching between tip and eraser while in range first sends an out-of-range report.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_digitizer_update`

```c
mimhid_result mimhid_digitizer_update(mimhid_node* node, const mimhid_digitizer_sample* sample);
```

- **Ownership:** Copies sample.
- **Blocking:** None.
- **Synchronization:** Device domain. As `mimhid_pen_update`, for a Digitizer Node.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`.

#### `mimhid_battery_update`

```c
mimhid_result mimhid_battery_update(mimhid_node* node, int32_t strength);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the battery level of a Node whose Spec has enable_battery. It goes out with the next `mimhid_node_submit` that has no input report to send, and only when it changed. A level not yet sent is dropped on close.
- **Returns:** As every input call, `MIMHID_OK` through `MIMHID_ERR_NO_DEVICE`; `MIMHID_ERR_PARAM` also for a Node without a battery report.

### Raw reports

#### `mimhid_raw_submit`

```c
mimhid_result mimhid_raw_submit(mimhid_node* node, const uint8_t* report, size_t length, uint32_t send_timeout_ms);
```

- **Ownership:** Copies report.
- **Blocking:** As `mimhid_node_submit`.
- **Synchronization:** Device domain. Sends one report of a Raw Spec's Node after checking it against the Spec's report table.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for a Node that is not Raw, a null report, a Report ID or length the Spec does not accept, or nonzero padding bits; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_BUSY` as for `mimhid_node_submit`; `MIMHID_ERR_OVERFLOW` for a report longer than maximum_report_bytes; a link error.

#### `mimhid_report_send`

```c
mimhid_result mimhid_report_send(mimhid_node* node, const uint8_t* report_bytes, size_t report_length, uint32_t send_timeout_ms);
```

- **Ownership:** Copies report_bytes.
- **Blocking:** As `mimhid_node_submit`.
- **Synchronization:** Device domain. Sends report_bytes as one report of the Node, Report ID included, without any check and without touching the Node's state: the way to send on a Node of `mimhid_node_open_raw`.
- **Returns:** `MIMHID_OK`; `MIMHID_ERR_PARAM` for an invalid Node or a null report_bytes with a nonzero length; `MIMHID_ERR_UNSET_FIELD` for a zero timeout; `MIMHID_ERR_BUSY` for a Node opened in a batch that has not ended or while the board enumerates; `MIMHID_ERR_OVERFLOW` for a report longer than maximum_report_bytes; a link error.

---

## C++20 header (`include/mimhid.hpp`)

[include/mimhid.hpp](../include/mimhid.hpp) wraps every function of `mimhid.h`
under `namespace mimhid` with the `mimhid_` prefix removed, adds factories that
return a structure after its `*_init` (`mimhid::device_config()`, `mimhid::commit_timeouts()`,
`mimhid::node_config()`, `mimhid::keyboard_options()`, etc.), and provides RAII
owning handles for the two types whose release cannot fail:

- `mimhid::SpecPtr` (`std::unique_ptr<mimhid_spec, mimhid::SpecRelease>`)
- `mimhid::DiscoveryPtr` (`std::unique_ptr<mimhid_discovery, mimhid::DiscoveryDestroy>`)

`mimhid::device_link_counters` takes `bool reset` in place of `uint32_t reset`.

```cpp
#include "mimhid.hpp"

mimhid::SpecPtr make_keyboard_spec() {
    auto kb = mimhid::keyboard_options();
    kb.usage_minimum = 0x04;
    kb.usage_maximum = 0xDD;
    mimhid_spec* raw_spec = nullptr;
    if (mimhid::spec_create_keyboard(&kb, &raw_spec) == MIMHID_OK) {
        return mimhid::SpecPtr{raw_spec};
    }
    return nullptr;
}
```

---

## Result codes (`mimhid_result`)

`libmimhid` and `libaoahid` give a code with the same meaning the same number.
The table lists both libraries; a dash means the library has no such code.

| Value | `libmimhid` | `libaoahid` | Meaning in `libmimhid` |
|---:|---|---|---|
| 0 | `MIMHID_OK` | `AOAHID_OK` | The call succeeded |
| 1 | `MIMHID_ERR_PARAM` | `AOAHID_ERR_PARAM` | A null pointer, a wrong `struct_size`, a nonzero reserved field, a flag other than 0 or 1, a value out of range, the wrong kind of Node; or the board's `MIMHID_NAK_PARAM` |
| 2 | `MIMHID_ERR_UNSET_FIELD` | `AOAHID_ERR_UNSET_FIELD` | A required field or timeout argument was zero |
| 3 | `MIMHID_ERR_UNSUPPORTED` | `AOAHID_ERR_UNSUPPORTED` | Not possible with this Spec, board, or link: a rate the board or adapter cannot carry, a `configuration_value` other than 1, an interface class other than HID, `mimhid_node_submit` on a Raw Node; or the board's `MIMHID_NAK_UNSUPPORTED` |
| 4 | `MIMHID_ERR_PROTOCOL` | `AOAHID_ERR_NOT_AOA` | The board's firmware has another major or minor version than the library |
| 5 | — | — | Not used by either library |
| 6 | `MIMHID_ERR_ACCESS` | `AOAHID_ERR_ACCESS` | The serial port could not be opened |
| 7 | `MIMHID_ERR_BUSY` | `AOAHID_ERR_BUSY` | The call does not fit the current state: a batch already open or not open, a Node opened in a batch that has not ended, the board re-enumerating, a report of the Node being sent; or the board's `MIMHID_NAK_STATE` or `MIMHID_NAK_BUSY` (for example `mimhid_device_remote_wakeup` when the target has not suspended the device) |
| 8 | `MIMHID_ERR_NO_DEVICE` | `AOAHID_ERR_NO_DEVICE` | The serial port failed or was unplugged; every later call on the Device returns it until `mimhid_device_close` |
| 9 | — | `AOAHID_ERR_STALL` | Not used by `libmimhid` |
| 10 | `MIMHID_ERR_TIMEOUT` | `AOAHID_ERR_TIMEOUT` | No board answered the handshake, a frame was not answered in time, a serial write made no progress, or the target did not mount the device within `mimhid_device_wait_ready`'s timeout |
| 11 | `MIMHID_ERR_SHORT_TRANSFER` | `AOAHID_ERR_SHORT_TRANSFER` | A serial write stopped part-way through a frame |
| 12 | `MIMHID_ERR_DESCRIPTOR_REJECTED` | `AOAHID_ERR_DESCRIPTOR_REJECTED` | The board refused the configuration (`MIMHID_NAK_DESCRIPTOR`). `libaoahid` defines the code and returns it from no call |
| 13 | `MIMHID_ERR_IO` | `AOAHID_ERR_IO` | Any other operating-system error on the serial port |
| 14 | `MIMHID_ERR_OVERFLOW` | `AOAHID_ERR_OVERFLOW` | A size, count, or limit was exceeded: of the protocol, USB, HID, `linux_hid`, or the board; or the board's `MIMHID_NAK_OVERFLOW` |
| 15 | — | `AOAHID_CLOSE_PENDING` | Not used by `libmimhid`: its writes finish within the call |
| 16 | `MIMHID_ERR_INTERNAL` | `AOAHID_ERR_INTERNAL` | An allocation failed, an internal invariant broke, or the board sent a `NAK` code the library does not know |

---

## Differences from `libaoahid`

| Area | `libaoahid` (Android Open Accessory 2.0) | `libmimhid` (MCU USB-HID bridge) |
|---|---|---|
| **Target and transport** | An Android phone or tablet directly over USB (`libusb`, AOA control requests 51 to 57 on endpoint 0) | Any USB host through an MCU board on a host serial port (`termios` or Win32) |
| **Top-level handles** | `aoahid_context*`, `aoahid_discovery*`, `aoahid_device*`, `aoahid_spec*`, `aoahid_node*`, `aoahid_channel*` | No Context: `mimhid_discovery*`, `mimhid_device*`, `mimhid_spec*`, `mimhid_node*` |
| **Initializers (`*_init`)** | Fill in values that work with a current Android phone; the header has `AOAHID_CONTROL_TIMEOUT_MS`, `AOAHID_SEND_TIMEOUT_MS`, and `AOAHID_DRAIN_TIMEOUT_MS` | Only zero the structure and set `struct_size`; there are no timeout constants, and every required field is set by the caller |
| **USB descriptors** | The phone's own; `aoahid_accessory_start` sends only the AOA identification strings | The caller gives the Device, Configuration, Interface, and Endpoint descriptors (`mimhid_device_config`, `mimhid_node_config`) |
| **Opening and closing a Node** | `ACCESSORY_REGISTER_HID` (request 54) and `ACCESSORY_SET_HID_REPORT_DESC` (56) on open, `ACCESSORY_UNREGISTER_HID` (55) on close, each waiting up to `control_timeout_ms` | `const mimhid_commit_timeouts*` (`control_timeout_ms` and `reconnect_timeout_ms`); the board re-enumerates, or the change is staged inside `mimhid_device_batch_begin` and `mimhid_device_batch_end` |
| **Submit calls** | `aoahid_node_submit` and `aoahid_node_submit_blocking` | `mimhid_node_submit` (one report) and `mimhid_node_submit_blocking` (until multi-packet frames and relative remainders are sent) |
| **Two-way HID and bus control** | Input only; bulk `aoahid_channel` for side interfaces such as ADB | Output reports, Feature answers, and bus events through `transport.event_sink`, `mimhid_node_set_feature_response`, `mimhid_device_disconnect`, `mimhid_device_connect`, `mimhid_device_remote_wakeup`, and endpoint 0 control routing (`mimhid_device_set_control_routing`, `mimhid_device_control_reply`) |
| **Result codes** | Also `AOAHID_ERR_NOT_AOA` (4), `AOAHID_ERR_STALL` (9), and `AOAHID_CLOSE_PENDING` (15) | `MIMHID_ERR_PROTOCOL` (4); 5, 9, and 15 are not used |
