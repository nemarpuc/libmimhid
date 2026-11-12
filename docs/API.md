# API Reference

`libmimusb` exposes a stable C11 ABI in [include/mimusb.h](../include/mimusb.h)
and an optional header-only C++20 facade in
[include/mimusb.hpp](../include/mimusb.hpp). Every exported symbol begins with
`mimusb_`. The function reference below follows the comment above each
declaration in `mimusb.h`; where this page and the header differ, the code
decides, and the difference is a bug in one of them.

## Objects and ownership

The API has four opaque handle types:

| Handle | Created by | Released by | Ownership and lifetime |
|---|---|---|---|
| `mimusb_discovery*` | `mimusb_discover` | `mimusb_discovery_destroy` | A snapshot of the `mimusb_device_info` records found by one search. Independent of any Device. |
| `mimusb_spec*` | `mimusb_spec_create_*` | `mimusb_spec_release` | An immutable, reference-counted HID report descriptor and report layout. Independent of any Device; one Spec may be used by several Nodes and threads. |
| `mimusb_device*` | `mimusb_device_open` | `mimusb_device_close` | One board on one serial port: the port, the reader thread, and the USB device the board presents. Closing it closes its Nodes. |
| `mimusb_node*` | `mimusb_node_open`, `mimusb_node_open_profile` | `mimusb_node_close` or `mimusb_device_close` | The interfaces one function presents to the target: any class from its interfaces, endpoints, class descriptors and control rules (`mimusb_node_open`; a HID interface of the caller's own is class 0x03 with its HID descriptor and rules), or one HID collection from a Spec (`mimusb_node_open_profile`, for which the library writes the control rules HID needs). A Node opened from a Spec holds a reference to it. After `mimusb_device_close` its Nodes are gone and their handles must not be used. |

## Rules every function follows

### Return codes and `mimusb_last_error()`

Every function that can fail returns a `mimusb_result` (`MIMUSB_OK`, 0, on
success) or, for the few that return a value, says in its Returns line what it
returns on failure. Every call except `mimusb_last_error` replaces the calling
thread's record, `mimusb_result_name` and `mimusb_frame_type_name` included, so
read the record before making another call. `mimusb_last_error()` returns
`const mimusb_error_detail*`:

- `code`: the `mimusb_result` of the last call (`MIMUSB_OK` when it succeeded).
- `field`: a static string naming the argument, field, or step concerned (for
  example `"usb_device.vendor_id"` or `"keyboard.usage_range"`), or `NULL`.
- `reason`: one English sentence.
- `transport_status`: the `errno` or Win32 error of a failed serial call, or the
  board's `MIMUSB_NAK_*` code (`proto/mimusb_proto.h`: 1 `PARAM`, 2 `OVERFLOW`,
  3 `STATE`, 4 `DESCRIPTOR`, 5 `UNSUPPORTED`, 6 `BUSY`).
- `frame_type`: the serial frame concerned (`mimusb_frame_type_name`).
- `node_id`, `report_id`, `offset`, `length`: set for Node, report, and
  descriptor errors.

A `NAK` from the board becomes a result like this (`command_error` in
`src/api/device_ops.cpp`):

| Board answer | Result |
|---|---|
| `MIMUSB_NAK_PARAM` | `MIMUSB_ERR_PARAM` |
| `MIMUSB_NAK_OVERFLOW` | `MIMUSB_ERR_OVERFLOW` |
| `MIMUSB_NAK_STATE`, `MIMUSB_NAK_BUSY` | `MIMUSB_ERR_BUSY` |
| `MIMUSB_NAK_DESCRIPTOR` | `MIMUSB_ERR_DESCRIPTOR_REJECTED` |
| `MIMUSB_NAK_UNSUPPORTED` | `MIMUSB_ERR_UNSUPPORTED` |
| any other code | `MIMUSB_ERR_INTERNAL` |
| no answer within the timeout | `MIMUSB_ERR_TIMEOUT` |
| port closed or unplugged | `MIMUSB_ERR_NO_DEVICE` |
| write cut short | `MIMUSB_ERR_SHORT_TRANSFER` |
| other serial error | `MIMUSB_ERR_IO` |

### Structures and initial values (`*_init`)

Every options or output structure with `struct_size` has a static inline
`mimusb_<type>_init` in `mimusb.h` (`mimusb_device_config_init`,
`mimusb_commit_timeouts_init`, `mimusb_node_config_init`, `mimusb_hid_node_config_init`,
`mimusb_board_limits_init`, `mimusb_link_counters_init`,
`mimusb_keyboard_options_init`, `mimusb_mouse_options_init`,
`mimusb_toggle_options_init`, `mimusb_gamepad_options_init`,
`mimusb_touchscreen_options_init`, `mimusb_touchpad_options_init`,
`mimusb_pen_options_init`, `mimusb_digitizer_options_init`,
`mimusb_raw_options_init`, and `mimusb_capability_manifest_init`).

> [!IMPORTANT]
> Every `*_init` function **only zeroes the structure and sets `struct_size`**.
> It fills in no vendor ID, baud rate, timeout, endpoint address, or profile
> range. A required field left zero is refused with `MIMUSB_ERR_UNSET_FIELD`.
> Every enumeration starts at 1, so a zeroed field never selects a mode by
> accident. `reserved` fields must stay zero, and a flag must be 0 or 1.

| Structure | Required (zero is refused) | Optional |
|---|---|---|
| `mimusb_transport_options` | `baud_rate` (or `MIMUSB_BAUD_FASTEST`), `handshake_timeout_ms`, `maximum_report_bytes`, the six `linux_hid` fields, `log_level` (`MIMUSB_LOG_DISABLED` to `MIMUSB_LOG_TRACE`; any level but `MIMUSB_LOG_DISABLED` also needs `log_sink`) | `validate_reports`, `keep_attached`, `event_sink`, `event_user`, `log_user` |
| `mimusb_usb_device` | `bcd_usb`, `ep0_max_packet_size` (8, 16, 32 or 64; 64 on a high-speed board), `vendor_id`, `language_id` | `device_class`, `device_subclass`, `device_protocol`, `product_id`, `bcd_device`, `manufacturer`, `product`, `serial`, `serial_from_chip_id` |
| `mimusb_usb_config` | `configuration_value` (must be 1), `bm_attributes` (bit 7 set, bits 4..0 clear) | `bmax_power` (0 to 250, in 2 mA units), `string` |
| `mimusb_commit_timeouts` | `control_timeout_ms`, `reconnect_timeout_ms` | — |
| `mimusb_node_config` | `interfaces`, `interface_count` (at least 1); per interface: `class_code` (never 0; 0x03 is accepted), per endpoint `address`, `transfer_type`, `max_packet_size`, `interval` (interrupt only), and on an IN endpoint `policy.on_full` and `policy.depth` | `number` (0 is the same as unset: the numbers in use must be 0, 1, 2, ... anyway), `subclass`, `protocol`, `name`, `class_descriptors`, `association`, `rules` and `rule_count` (this Node's control rules) |
| `mimusb_hid_node_config` | `bcd_hid`, `ep_in.address` (`0x81` to `0x8F`), `ep_in.transfer_type` (`MIMUSB_TRANSFER_INTERRUPT`), `ep_in.interval`, `ep_in.max_packet_size`, `ep_in.policy` (`on_full`, any of the three, and a depth) | `interface_number`, `subclass`, `protocol`, `country_code`, `name`, `ep_out` (all zero when there is no OUT endpoint) |
| `mimusb_control_rule` | `action` (`MIMUSB_CONTROL_ANSWER`, `_ANSWER_LAST_SENT`, `_ACCEPT`, `_ASK` or `_STALL`); for `ANSWER` `data` and `length`; for `ANSWER_LAST_SENT` `endpoint_address` (an IN address); for `ASK` `timeout_ms` (1 to `MIMUSB_CONTROL_MAX_TIMEOUT_MS`) and `when_late` (`MIMUSB_CONTROL_ACTION_STALL` or `_DROP`) | the masks and values of `bmRequestType`, `bRequest`, `wValue`, and `wIndex`; `first_byte_filter` and `first_byte` of `ANSWER_LAST_SENT` |

### The close rule

`mimusb_device_close` and `mimusb_node_close` follow one rule:

- `MIMUSB_ERR_PARAM` or `MIMUSB_ERR_UNSET_FIELD` (a null or closed handle, a
  zero timeout): **nothing was done** and the handle is still the caller's.
- **Any other result:** the object is gone and its handle must not be used
  again; an error says what failed on the way out.

`mimusb_discovery_destroy` and `mimusb_spec_release` return nothing: for a
non-null handle they always free the snapshot or drop the reference.

Writes to the serial port finish within the call, so a close never leaves
anything pending.

### Threading and domains

- **Device domain:** a `mimusb_device*` and its `mimusb_node*` handles form one
  domain. No two calls in one domain may run at the same time, except where a
  function says so (`mimusb_device_is_ready` from any thread while the Device is
  open, and `mimusb_device_control_reply` from inside `transport.event_sink`).
- **Outside any domain:** Specs and Discovery snapshots. `mimusb_spec_retain`,
  `mimusb_spec_release`, `mimusb_spec_descriptor`, and `mimusb_spec_manifest` may
  be called from any thread.
- **Callbacks (`event_sink`, `log_sink`):** they run on the Device's reader
  thread or on the thread making the call. `event->data` is valid only during
  the callback. A sink must not call back into `libmimusb`, except
  `mimusb_device_control_reply` from the event sink.

---

## Lifecycle and USB bus behavior

### 1. Before the first commit (`0 V` stealth state)

From power-up, and after `mimusb_device_open` has found the board, the board's
USB `D+` pull-up stays off (`0 V`): the target sees no USB device.
`mimusb_device_open` sends no configuration.

### 2. Opening Nodes and committing

`mimusb_node_open`, `mimusb_node_open_profile`, `mimusb_node_close`,
`mimusb_device_set_usb_identity`, `mimusb_device_batch_end`, and
`mimusb_device_commit` commit (`commit_device` in `src/api/device_ops.cpp`):

1. When descriptors are set as bytes (`mimusb_device_set_descriptors`), the
   library sends them as they are, with the BOS descriptor and the raw strings
   (`mimusb_device_set_string`) they go with. When Nodes are left instead, the
   library composes the USB descriptors (and sends the BOS descriptor of
   `mimusb_device_set_bos` if there is one) and the control rules (those of each Node, the ones it writes for each profile
   interface, then the Device's own) and sends `DESC_RESET`, then the device
   descriptor, the configuration descriptor and the strings (`DESC_BEGIN` and
   `DESC_DATA`), every control rule (`RULE`, with the bytes of an answer as
   `DESC_BEGIN` and `DESC_DATA`), and an `EP_CONFIG` for every IN endpoint (its
   policy and depth). Each frame waits
   up to `control_timeout_ms` for the board's answer and is sent up to three
   times. The board stores these frames; nothing changes on the target yet.
2. When the board's USB device is already on the target, the library sends a
   detach `COMMIT` (D+ off), waiting up to `control_timeout_ms`, and then waits
   `reconnect_delay_ms` on the host when `mimusb_device_commit` was given one
   (the other calls use 0).
3. The library sends an attach `COMMIT` and waits up to `reconnect_timeout_ms`
   for its answer. The board checks what it stored (`MIMUSB_ERR_DESCRIPTOR_REJECTED`
   when it does not hold together), makes it and the rules its configuration, turns its D+
   pull-up on, and answers. The call returns then: it does not
   wait for the target to enumerate the device. `mimusb_device_wait_ready`
   waits for that (the board's `MOUNTED` event, sent when the target selects
   the configuration).

When the last Node is closed (or `mimusb_device_clear_descriptors` is called),
the library sends only a detach `COMMIT`, and the board's USB device leaves the
target.

### 3. Sharing and separating USB interfaces

- As many Nodes as you like (up to 65534) can be open on a Device, on at most
  the board's `max_interfaces_any` interfaces of all classes together (HID ones
  included: the board reads no class) and within its endpoint numbers, since each
  HID interface needs an IN endpoint of its own (15 numbers on the RP2040 and
  RP2350, 7 on the nRF52840, 6 on the ESP32-S3, 3 on the STM32F4; see
  [LIMITS.md](LIMITS.md)); the control rules of all Nodes and of the Device
  together are at most `max_control_rules`, their answer bytes at most
  `max_answer_bytes`.
- The interface number (`mimusb_usb_interface.number`,
  `mimusb_hid_node_config.interface_number`) is the `bInterfaceNumber` the
  target sees. The numbers in use, over all Nodes, must be 0, 1, 2, ... without
  a gap, or the commit fails with `MIMUSB_ERR_PARAM` and the missing number in
  `mimusb_error_detail.offset`; nothing is renumbered, so open the Nodes in
  number order (or in one batch) and close the highest number first.
- Profile Nodes (`mimusb_node_open_profile`) with the
  **same** interface number share one USB HID interface: their interface and
  endpoint settings must match (the queue depth excepted: the board gets the
  least depth any of them set), and each must have a Report ID, all different.
  The rules for sharing live in the HID layer (`src/usb/hid_layer.cpp`), not in
  the composer and not in `mimusb_node_open`.
- Every endpoint address is used by one endpoint on the whole device (USB 2.0
  9.6.6; IN and OUT of one number are two addresses), with a number from 1 to
  the board's `max_endpoint_number`.
- The caller's strings (`mimusb_device_add_string`) keep their indices from 5
  up; an interface's `name` takes the lowest index above 4 that nothing else
  has, at each commit. (Step 1 gave an interface `5 + number`; that is gone.)

---

## Function reference

Each entry gives the declaration and the four lines of the comment above it in
`mimusb.h`: what the call owns or copies, what it waits for, which threads may
call it, and what it returns. Any call can also return `MIMUSB_ERR_INTERNAL`,
and the link errors (`MIMUSB_ERR_TIMEOUT`, `MIMUSB_ERR_NO_DEVICE`,
`MIMUSB_ERR_SHORT_TRANSFER`, `MIMUSB_ERR_IO`) come from any call that talks to
the board. Per-profile rules for the Spec factories and input calls are in
[PROFILES.md](PROFILES.md).

### Diagnostics and version

#### `mimusb_last_error`

```c
const mimusb_error_detail* mimusb_last_error(void);
```

- **Ownership:** Returns the calling thread's record, valid until that thread's next call.
- **Blocking:** None.
- **Synchronization:** The record belongs to the calling thread; do not pass it to another.
- **Returns:** Never null.

#### `mimusb_result_name`

```c
const char* mimusb_result_name(mimusb_result result);
```

- **Ownership:** The string is static.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** The constant's name, such as "`MIMUSB_ERR_TIMEOUT`", with `MIMUSB_OK` recorded; for a value that is not a result, "`MIMUSB_RESULT_UNKNOWN`" with `MIMUSB_ERR_PARAM` recorded.

#### `mimusb_frame_type_name`

```c
const char* mimusb_frame_type_name(int32_t frame_type);
```

- **Ownership:** The string is static.
- **Blocking:** None.
- **Synchronization:** Any thread. Names the serial frame type of `mimusb_error_detail`.frame_type (docs/PROTOCOL.md).
- **Returns:** The frame's name, such as "COMMIT" or "EP_IN", with `MIMUSB_OK` recorded; for a value that is not a frame type, "UNKNOWN" with `MIMUSB_ERR_PARAM` recorded.

#### `mimusb_version`

```c
uint32_t mimusb_version(void);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** (major << 24) | (minor << 16) | patch of the library that is running.

### Discovery

#### `mimusb_discover`

```c
mimusb_result mimusb_discover(uint32_t timeout_ms, mimusb_discovery** out_discovery);
```

- **Ownership:** The caller destroys the snapshot with `mimusb_discovery_destroy`.
- **Blocking:** Searches every USB serial port at once, one thread each, waiting up to timeout_ms for each answer; a port another program has open is skipped. On each port it pings at the boot rate and, when nothing answers, sends a BREAK (which returns a board a program left on a fast rate to the boot rate) and pings again, so a serial device that is not a board receives a few pings and a BREAK. On a board it measures the fastest rate the link carries: from the board's max_baud downward, it sends bursts of test frames and trusts a rate only when the board's own error counters stay at zero (baud_rate), and it leaves the board on the boot rate. The search takes a few seconds at most. A board whose firmware has another major or minor version is listed with only its version and firmware_compatible 0, so that it can be flashed again.
- **Synchronization:** Any thread.
- **Returns:** `MIMUSB_OK`, also when no board was found; `MIMUSB_ERR_PARAM` for a null out_discovery; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout.

#### `mimusb_discovery_count`

```c
size_t mimusb_discovery_count(const mimusb_discovery* discovery);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread, but not during `mimusb_discovery_destroy`.
- **Returns:** The number of boards with `MIMUSB_OK` recorded, or 0 with `MIMUSB_ERR_PARAM` recorded for a null snapshot.

#### `mimusb_discovery_get`

```c
const mimusb_device_info* mimusb_discovery_get(const mimusb_discovery* discovery, size_t index);
```

- **Ownership:** The entry belongs to the snapshot.
- **Blocking:** None.
- **Synchronization:** As `mimusb_discovery_count`.
- **Returns:** The entry with `MIMUSB_OK` recorded, or null with `MIMUSB_ERR_PARAM` recorded for a null snapshot or an index out of range.

#### `mimusb_discovery_destroy`

```c
void mimusb_discovery_destroy(mimusb_discovery* discovery);
```

- **Ownership:** Frees the snapshot and every entry of it.
- **Blocking:** None.
- **Synchronization:** No other call may use the snapshot meanwhile.
- **Returns:** Nothing. `MIMUSB_OK` is recorded, or `MIMUSB_ERR_PARAM` for null (nothing is done).

### Device lifecycle, commit, bus control, and queries

#### `mimusb_device_open`

```c
mimusb_result mimusb_device_open(const char* port_path, const mimusb_device_config* config, mimusb_device** out_device);
```

- **Ownership:** Copies config; the caller closes the Device with `mimusb_device_close`.
- **Blocking:** Opens port_path (such as "/dev/ttyACM0" or "COM3") for this process alone, looks for the board at the boot rate (and after a BREAK, for one left on a fast rate; waiting transport.handshake_timeout_ms for each answer), reads its limits, moves the link to transport.baud_rate and checks it with a short burst of test frames (`MIMUSB_BAUD_FASTEST` measures instead, which takes up to a few seconds), and starts the reader thread. It sends no configuration: the board's USB device stays off the target until a Node is opened.
- **Synchronization:** Any thread, one call per port.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer, a wrong struct_size or a bad value; `MIMUSB_ERR_UNSET_FIELD` for a required field left zero; `MIMUSB_ERR_UNSUPPORTED` for a rate the board cannot make, or one the adapter or wiring did not carry; `MIMUSB_ERR_OVERFLOW` for a maximum_report_bytes the board does not take; `MIMUSB_ERR_TIMEOUT` when no board answered; `MIMUSB_ERR_PROTOCOL` for firmware of another major or minor version; `MIMUSB_ERR_ACCESS` for a port that cannot be opened (it does not exist, the permission is missing, or another program has it); `MIMUSB_ERR_IO` for a port that cannot be set up.

#### `mimusb_device_close`

```c
mimusb_result mimusb_device_close(mimusb_device* device, uint32_t control_timeout_ms, uint32_t drain_timeout_ms);
```

- **Ownership:** Follows the "Closing" rule; the Device's Nodes go with it.
- **Blocking:** Sends the neutral report of every open Node (keys and buttons up, contacts lifted, axes at rest) within drain_timeout_ms, takes the board's USB device off the target unless transport.keep_attached is 1, removes the control routing and loss reporting, puts the board back on the boot rate and closes the port. Each configuration frame waits up to control_timeout_ms. A failed step does not stop the next one.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null or closing Device; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_NO_DEVICE` when the link was already gone, so the board was not reset; otherwise the first link error on the way out.

#### `mimusb_device_set_usb_identity`

```c
mimusb_result mimusb_device_set_usb_identity(mimusb_device* device, const mimusb_usb_device* usb_device, const mimusb_usb_config* usb_config, const mimusb_commit_timeouts* timeouts);
```

- **Ownership:** Copies usb_device and usb_config.
- **Blocking:** Commits (see [Lifecycle and USB bus behavior](#lifecycle-and-usb-bus-behavior)), waiting as timeouts says; inside a batch it only stages the change.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer, a wrong struct_size or a bad value; `MIMUSB_ERR_UNSET_FIELD` for a required field or timeout left zero; the results of `mimusb_device_commit`.

#### `mimusb_device_latched_error`

```c
mimusb_result mimusb_device_latched_error(mimusb_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Reads the error the reader thread recorded for the link, and forgets it unless it is `MIMUSB_ERR_NO_DEVICE`, which stays.
- **Returns:** `MIMUSB_OK` when there is none; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_NO_DEVICE` after the port failed or was unplugged; `MIMUSB_ERR_IO` for another link error.

#### `mimusb_device_is_ready`

```c
int32_t mimusb_device_is_ready(const mimusb_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Any thread while the Device is open.
- **Returns:** 1 while the target has the board's USB device mounted and takes reports, else 0.

#### `mimusb_device_wait_ready`

```c
mimusb_result mimusb_device_wait_ready(mimusb_device* device, uint32_t timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Until the target mounts the board's USB device, or timeout_ms.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK` once mounted; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_TIMEOUT`; `MIMUSB_ERR_NO_DEVICE` when the link fails meanwhile.

#### `mimusb_device_batch_begin`

```c
mimusb_result mimusb_device_batch_begin(mimusb_device* device);
```

- **Ownership:** Nothing.
- **Blocking:** None; nothing is sent. Until `mimusb_device_batch_end`, `mimusb_node_open`, `mimusb_node_close` and `mimusb_device_set_usb_identity` only stage their change, and a Node opened in the batch refuses reports with `MIMUSB_ERR_BUSY`.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_BUSY` inside a batch.

#### `mimusb_device_batch_end`

```c
mimusb_result mimusb_device_batch_end(mimusb_device* device, const mimusb_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** Commits every change staged since `mimusb_device_batch_begin` at once, waiting as timeouts says.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, a null timeouts or a wrong struct_size; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_BUSY` outside a batch; the results of `mimusb_device_commit`.

#### `mimusb_device_commit`

```c
mimusb_result mimusb_device_commit(mimusb_device* device, uint32_t reconnect_delay_ms, const mimusb_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** Sends the configuration of every open Node and commits, waiting as timeouts says. `mimusb_node_open`, `mimusb_node_close` and `mimusb_device_batch_end` do this already; call it to retry after a failed commit, or for reconnect_delay_ms: with a nonzero delay on an attached device the library takes the board's USB device off the target (D+ at 0 V), waits that many milliseconds on the host, and then puts it back. Inside a batch it commits the staged changes and ends the batch.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device or timeouts; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_OVERFLOW` when the Nodes do not fit the board; `MIMUSB_ERR_DESCRIPTOR_REJECTED` when the board refuses the configuration; a link error.

#### `mimusb_device_connect`

```c
mimusb_result mimusb_device_connect(mimusb_device* device, const mimusb_commit_timeouts* timeouts);
```

- **Ownership:** Nothing.
- **Blocking:** A Device that is not on the target (not configured yet, or after `mimusb_device_disconnect`) commits instead, which puts the board's USB device on the target (D+ pull-up on) and waits for the board's answer. A Device that is on the target already sends nothing and returns `MIMUSB_OK`: the board refuses a commit that attaches while it is attached.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device or timeouts; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; a link error; the results of `mimusb_device_commit` when it commits.

#### `mimusb_device_disconnect`

```c
mimusb_result mimusb_device_disconnect(mimusb_device* device, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Takes the board's USB device off the target (D+ at 0 V, as if unplugged), waiting up to control_timeout_ms for the board's answer. The next connect or commit configures it again.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; a link error.

#### `mimusb_device_remote_wakeup`

```c
mimusb_result mimusb_device_remote_wakeup(mimusb_device* device, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Waits up to control_timeout_ms for the board's answer.
- **Synchronization:** Device domain. Wakes a target that suspended the board's USB device; the target must have enabled remote wakeup (usb_config.bm_attributes bit 5 and `MIMUSB_EVENT_SUSPEND` with value 1).
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_BUSY` when the device is not suspended; a link error.

#### `mimusb_device_enter_bootloader`

```c
mimusb_result mimusb_device_enter_bootloader(mimusb_device* device, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Waits up to control_timeout_ms for the board's answer.
- **Synchronization:** Device domain. Restarts the board into its chip's own bootloader, where new firmware is written (the RPI-RP2 drive on the target-facing USB port of an RP2040 or RP2350, USB DFU or a USART on an STM32F4, the ROM's download mode on an ESP32-S3, the bootloader in flash on an nRF52840 board that has one). The board first takes its USB device off the target. After `MIMUSB_OK` the board no longer answers: the Device is good only for `mimusb_device_close`. Close does not know the board restarted: it reports the board as gone (`MIMUSB_ERR_NO_DEVICE`) only when the serial link itself failed. A Device that was never committed has nothing to detach, so close returns `MIMUSB_OK`. For a Device that was committed, the detach `COMMIT` that close sends gets no answer from the restarted board and close returns the link error (`MIMUSB_ERR_TIMEOUT` after three tries).
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_UNSUPPORTED` on a board whose chip has no bootloader the firmware can enter; a link error.

#### `mimusb_device_board_info`

```c
mimusb_result mimusb_device_board_info(const mimusb_device* device, mimusb_device_info* out_info);
```

- **Ownership:** Fills the caller's out_info.
- **Blocking:** None; a copy of what the board reported at open.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device or a null out_info.

#### `mimusb_device_limits`

```c
mimusb_result mimusb_device_limits(const mimusb_device* device, mimusb_board_limits* out_limits);
```

- **Ownership:** Fills the caller's out_limits (set its struct_size; `mimusb_board_limits_init` does).
- **Blocking:** None; a copy of what the board reported at open.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, a null out_limits or a wrong struct_size.

#### `mimusb_device_link_counters`

```c
mimusb_result mimusb_device_link_counters(mimusb_device* device, uint32_t reset, uint32_t control_timeout_ms, mimusb_link_counters* out_counters);
```

- **Ownership:** Fills the caller's out_counters (set its struct_size; `mimusb_link_counters_init` does).
- **Blocking:** One command to the board, waiting up to control_timeout_ms for its answer.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, a null out_counters or a wrong struct_size; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_TIMEOUT` or `MIMUSB_ERR_IO` when the board did not answer. With reset nonzero the board's counters are zeroed after being read: a program that wants to see a link degrade reads them now and then. Any lost byte shows there, not in the report stream.

### Control transfers of the target (endpoint 0)

#### `mimusb_device_set_control_rules`

```c
mimusb_result mimusb_device_set_control_rules(mimusb_device* device, uint32_t report_all, const mimusb_control_rule* rules, size_t rule_count, uint32_t control_timeout_ms);
```

- **Ownership:** Copies rules and the bytes they point to.
- **Blocking:** Sends the whole table of the Device's Nodes and these rules again, with their answer bytes, and waits up to control_timeout_ms for the board's answer to each frame (a rule's timeout_ms is the board's wait for the program, a different one). Inside a batch, or before the first commit, nothing is sent: the rules go with the commit.
- **Synchronization:** Device domain. Replaces the Device's own rules, which come after those of the Nodes (the first rule that matches decides, so a rule here never overrides a Node's). report_all 1 reports every control transfer, whatever its rule; with no rules and report_all 0 the Device adds nothing. A request the board does not match is stalled (USB 2.0 9.2.7), apart from the standard ones its stack serves. A held request (`MIMUSB_CONTROL_ASK`) delays only the target's control transfer, never the reports, but while the board waits for the verdict of a host-to-device transfer its USB task does nothing else. SET_ADDRESS and SET_CONFIGURATION are never held, nor a host-to-device data stage longer than max_control_data_bytes. The rules stay until replaced or `mimusb_device_close`.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, report_all, count or rule (a value outside its mask, an unknown action, an ANSWER with null data and a length, an ANSWER or ANSWER_LAST_SENT of a host-to-device request, an endpoint that is not an IN address, a timeout over `MIMUSB_CONTROL_MAX_TIMEOUT_MS`, a when_late other than STALL or DROP); `MIMUSB_ERR_UNSET_FIELD` for a zero control_timeout_ms, an action left 0, or an ASK without timeout_ms or when_late; `MIMUSB_ERR_OVERFLOW` for more rules or answer bytes than the board holds with those of the Nodes; a link error. A rule naming an endpoint that is not an IN endpoint of the committed configuration is refused by the board.

The actions (`mimusb_control_rule_action`): `MIMUSB_CONTROL_ANSWER` returns `data` cut to wLength (device to host only); `MIMUSB_CONTROL_ANSWER_LAST_SENT` returns the last transfer the board started on IN endpoint `endpoint_address` (with `first_byte_filter` 1, the last one whose first byte is `first_byte`, which is how a Report ID tells reports apart; device to host only) and stalls when there is none since the commit; `MIMUSB_CONTROL_ACCEPT` takes the data stage and tells the program with `MIMUSB_EVENT_CONTROL_SETUP` (flag `MIMUSB_CONTROL_FLAG_ACCEPTED`) and `MIMUSB_EVENT_CONTROL_DATA`; `MIMUSB_CONTROL_ASK` holds the request and asks the program (`mimusb_device_control_reply`); `MIMUSB_CONTROL_STALL` stalls. The wire description is in [PROTOCOL.md](PROTOCOL.md#control-rules).

#### `mimusb_device_set_rule_data`, `mimusb_node_set_rule_data`

```c
mimusb_result mimusb_device_set_rule_data(mimusb_device* device, size_t rule_index, const uint8_t* data, size_t length, uint32_t control_timeout_ms);
mimusb_result mimusb_node_set_rule_data(mimusb_node* node, size_t rule_index, const uint8_t* data, size_t length, uint32_t control_timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** One command to the board, waiting up to control_timeout_ms; it is retried while the board is answering from that rule. Inside a batch, or before the first commit, the bytes are kept for the commit.
- **Synchronization:** Device domain. Changes the bytes an `ANSWER` rule returns without a new table: rule number rule_index of the array given to `mimusb_device_set_control_rules` (or of `mimusb_node_config.rules` for the Node). At most as many bytes as the rule was given (the rule then answers with that many; when the library next writes the whole table, which any Node open or close or `mimusb_device_set_control_rules` does, a rule keeps the length it had then as the most it can be given). A profile Node has no rules of its own to change: use `mimusb_node_set_feature_response`.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device or Node, a rule that is not an ANSWER rule, a null data with a length or more bytes than the rule's capacity; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_BUSY` when the board kept answering from the rule; a link error. A rule keeps its capacity, the length of its data when the rule was set, however its bytes change and however often the table is built again, so a shortened answer can be made longer again, up to the capacity.

#### `mimusb_hid_descriptor_bytes`, `mimusb_hid_descriptor_rule`

```c
mimusb_result mimusb_hid_descriptor_bytes(uint16_t bcd_hid, uint8_t country_code, size_t report_descriptor_length, uint8_t out[9]);
mimusb_result mimusb_hid_descriptor_rule(uint8_t interface_number, uint8_t descriptor_type, const uint8_t* bytes, size_t length, mimusb_control_rule* out_rule);
```

- **Ownership:** Nothing; `out_rule->data` points at `bytes`, which the caller keeps until the rule is given to the library.
- **Blocking:** None.
- **Synchronization:** None. Helpers for a HID interface of the caller's own (`examples/c/hid_interface.c`): the first writes the 9-byte HID descriptor with one report descriptor (HID 1.11 6.2.1) for `mimusb_usb_interface.class_descriptors`; the second fills an `ANSWER` rule for `GET_DESCRIPTOR` (USB 2.0 9.4.3, HID 1.11 7.1.1) of type 0x22 (report descriptor) or 0x21 (HID descriptor) on one interface.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer, a length of 0 or above 65535 or another descriptor type; `MIMUSB_ERR_UNSET_FIELD` for a zero bcd_hid.

#### `mimusb_device_control_reply`

```c
mimusb_result mimusb_device_control_reply(mimusb_device* device, uint32_t token, mimusb_control_action action, const uint8_t* data, size_t length, uint32_t send_timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** Writes one frame, for up to send_timeout_ms without progress; it does not wait for the board.
- **Synchronization:** Device domain, and also from the event sink, which is the way to answer in time. Answers the held transfer of token (from `MIMUSB_EVENT_CONTROL_SETUP`, or from `MIMUSB_EVENT_CONTROL_DATA` for the verdict of a host-to-device transfer): - ANSWER to a device-to-host request: data is the data stage, sent whole whatever wLength says. More than wLength makes the host report an overrun; data with wLength 0 still starts a data stage, which is how a device that does so is copied. Empty data is an empty data stage. - ANSWER to any other request: data must be empty. With a host-to-device data stage the board takes the data, reports it as `MIMUSB_EVENT_CONTROL_DATA` and waits for the verdict: ANSWER (acknowledge), STALL, or the rule's when_late. - STALL, DROP: data must be empty. - LEAVE: the board's own handling; as a verdict it acknowledges. A reply the board no longer waits for (too late, or the target gave up) is dropped.
- **Returns:** `MIMUSB_OK` once the frame is written; `MIMUSB_ERR_PARAM` for an invalid Device, a token of 0 or above 255, an unknown action, or data with an action that takes none; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_OVERFLOW` for data longer than max_control_data_bytes; a link error.

#### `mimusb_device_set_loss_reporting`

```c
mimusb_result mimusb_device_set_loss_reporting(mimusb_device* device, uint32_t enabled, uint32_t interval_ms, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** Waits up to control_timeout_ms for the board's answer.
- **Synchronization:** Device domain. enabled 1 makes the board report the reports it drops because its queue is full (`MIMUSB_EVENT_TRANSFER_LOST`, naming the endpoint), at most one event per interval_ms (0 to `MIMUSB_LOSS_MAX_INTERVAL_MS`; 0 reports each time at once). enabled 0, which is how the board starts, needs interval_ms 0 and puts nothing extra on the link. The setting stays until replaced or `mimusb_device_close`.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, enabled or interval_ms; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; a link error.

### Spec factories and inspection

#### `mimusb_spec_create_keyboard`

```c
mimusb_result mimusb_spec_create_keyboard(const mimusb_keyboard_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`; `MIMUSB_ERR_PARAM` also for a range that includes 0x00 to 0x03 or the modifiers.

#### `mimusb_spec_create_mouse`

```c
mimusb_result mimusb_spec_create_mouse(const mimusb_mouse_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_toggle`

```c
mimusb_result mimusb_spec_create_toggle(const mimusb_toggle_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`; `MIMUSB_ERR_UNSUPPORTED` for a semantic that press and release cannot express, or a Camera Control Usage that is not `MIMUSB_USAGE_ONE_SHOT`.

#### `mimusb_spec_create_gamepad`

```c
mimusb_result mimusb_spec_create_gamepad(const mimusb_gamepad_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_touchscreen`

```c
mimusb_result mimusb_spec_create_touchscreen(const mimusb_touchscreen_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_touchpad`

```c
mimusb_result mimusb_spec_create_touchpad(const mimusb_touchpad_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_pen`

```c
mimusb_result mimusb_spec_create_pen(const mimusb_pen_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_digitizer`

```c
mimusb_result mimusb_spec_create_digitizer(const mimusb_digitizer_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_spec_create_raw`

```c
mimusb_result mimusb_spec_create_raw(const mimusb_raw_options* options, mimusb_spec** out_spec);
```

- **Ownership:** As every factory.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** As every factory, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`; `MIMUSB_ERR_PARAM` also for a descriptor that does not parse or does not match the report table; `MIMUSB_ERR_UNSUPPORTED` for Output or Feature items without the matching requires_* flag, a long item, or a construct the parser does not handle.

#### `mimusb_spec_retain`

```c
void mimusb_spec_retain(mimusb_spec* spec);
```

- **Ownership:** Adds a reference for the caller, who must already hold one.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** Nothing. `MIMUSB_OK` is recorded; `MIMUSB_ERR_PARAM` for null, or `MIMUSB_ERR_OVERFLOW` when the count is full (no reference is added).

#### `mimusb_spec_release`

```c
void mimusb_spec_release(mimusb_spec* spec);
```

- **Ownership:** Gives up one of the caller's references; the last one frees the Spec. A Node keeps its own reference until it closes.
- **Blocking:** None.
- **Synchronization:** Any thread, as long as the last release does not race any other use.
- **Returns:** Nothing. `MIMUSB_OK` is recorded, or `MIMUSB_ERR_PARAM` for null (nothing is done).

#### `mimusb_spec_descriptor`

```c
mimusb_result mimusb_spec_descriptor(const mimusb_spec* spec, const uint8_t** bytes, size_t* length);
```

- **Ownership:** The bytes belong to the Spec.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer.

#### `mimusb_spec_manifest`

```c
mimusb_result mimusb_spec_manifest(const mimusb_spec* spec, mimusb_capability_manifest* manifest);
```

- **Ownership:** Fills the caller's manifest; manifest->reports belongs to the Spec.
- **Blocking:** None.
- **Synchronization:** Any thread.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer, a wrong struct_size or a nonzero reserved.

### Node lifecycle, configuration, and submission

#### `mimusb_node_open`

```c
mimusb_result mimusb_node_open(mimusb_device* device, const mimusb_node_config* config, const mimusb_commit_timeouts* timeouts, mimusb_node** out_node);
```

- **Ownership:** Copies config and everything it points to.
- **Blocking:** Sends the configuration of every Node and commits (see [Lifecycle and USB bus behavior](#lifecycle-and-usb-bus-behavior)), waiting as timeouts says, so the target sees the board's USB device unplugged and plugged in again and every other Node is briefly away. Inside a batch it only stages the Node; timeouts is still required.
- **Synchronization:** Device domain. Opens a Node of any class from its interfaces, endpoints, class descriptors and optional association. Nothing is read from `class_code` except that 0 is unset. What the class does on endpoint 0 (a HID interface's report descriptor, `GET_REPORT`, `SET_IDLE`, ...) is in `config.rules`: a request that no rule of any Node or of the Device matches is stalled; `mimusb_node_open_profile` writes those rules for a Spec. Checked without knowing the class: interface numbers (0, 1, 2, ... without a gap over all Nodes), every endpoint address (USB 2.0 9.6.6, unique on the device, a number the controller has), the transfer type (bulk or interrupt; the board has no others), the packet size (within `max_endpoint_bytes`; bulk 8, 16, 32 or 64 at full speed, 512 at high speed, USB 2.0 5.8.3), the interval of an interrupt endpoint (1 to 255, at high speed 1 to 16), a policy on every IN endpoint and none on an OUT endpoint, the `bLength` of each class descriptor, the association (8 bytes, type 0x0B, naming this Node's consecutive interfaces), the rules and the totals against `mimusb_board_limits`.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null argument, a wrong struct_size, a bad value, a gap in the interface numbers (`mimusb_error_detail.offset` is the missing number), a repeated endpoint address or interface number, or an association that does not match; `MIMUSB_ERR_UNSET_FIELD` for a required field left zero (`class_code`, `transfer_type`, `max_packet_size`, the interval of an interrupt endpoint, the policy of an IN endpoint, the action of a rule) or a zero timeout; `MIMUSB_ERR_UNSUPPORTED` for a transfer type the board does not have; `MIMUSB_ERR_OVERFLOW` when a size, count or limit of the board is exceeded; the results of `mimusb_device_commit`.

#### `mimusb_node_open_profile`

```c
mimusb_result mimusb_node_open_profile(mimusb_device* device, mimusb_spec* spec, const mimusb_hid_node_config* config, const mimusb_commit_timeouts* timeouts, mimusb_node** out_node);
```

- **Ownership:** Copies config; the Node keeps a reference to spec until it closes.
- **Blocking:** As `mimusb_node_open`.
- **Synchronization:** Device domain. Opens a HID Node whose report descriptor is the Spec's: interface class 0x03, the HID descriptor, one interrupt IN endpoint and at most one interrupt OUT endpoint, all on the board's generic driver. The library writes the control rules that make it behave as HID 1.11 asks (`GET_DESCRIPTOR` of the HID and report descriptors, `GET_REPORT` of the last Input report sent and of a Feature answer, `SET_REPORT`, `GET_IDLE`/`SET_IDLE`, `GET_PROTOCOL`/`SET_PROTOCOL`) and the events that follow from them (`MIMUSB_EVENT_OUTPUT_REPORT`, `_FEATURE_REPORT`, `_SET_IDLE`, `_SET_PROTOCOL`). The profile calls and `mimusb_node_submit` work on it, and so does `mimusb_node_endpoint_write` on its IN endpoint. The IN endpoint's policy is any of the three (`MIMUSB_FULL_DROP_NEWEST`, `_DROP_OLDEST`, `_WAIT`).
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null argument, a wrong struct_size, a bad value, Nodes sharing an interface that do not follow the rules, or interface numbers that leave a gap (`mimusb_error_detail.offset` is the missing number; nothing is sent); `MIMUSB_ERR_UNSET_FIELD` for a required field or timeout left zero; `MIMUSB_ERR_UNSUPPORTED` for a transfer type other than interrupt; `MIMUSB_ERR_OVERFLOW` when the board has no room for the Node, its interface or its descriptor, or the Spec exceeds maximum_report_bytes or a linux_hid limit; the results of `mimusb_device_commit`.

#### `mimusb_node_close`

```c
mimusb_result mimusb_node_close(mimusb_node* node, const mimusb_commit_timeouts* timeouts, uint32_t drain_timeout_ms);
```

- **Ownership:** Follows the "Closing" rule.
- **Blocking:** Finishes a touch frame that has started and sends the neutral report (keys and buttons up, contacts lifted, axes at rest) within drain_timeout_ms, then commits the remaining Nodes as `mimusb_node_open` does; closing the last Node takes the USB device off the target. Inside a batch it only stages the removal. A failed step does not stop the next one; after a failed commit the target may still see the Node until a commit succeeds (`mimusb_device_commit`). Closing a Node whose interface number is not the highest in use would leave a gap in the numbers: the Node is gone, the commit fails with `MIMUSB_ERR_PARAM`, and a later commit succeeds once the gap is closed (close the highest number first, or in a batch).
- **Synchronization:** Device domain; not from the event sink.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null or closed Node, a closing Device, or a wrong timeouts struct_size; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; otherwise the first error on the way out.

#### `mimusb_node_id`

```c
uint16_t mimusb_node_id(const mimusb_node* node);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain.
- **Returns:** The number the board knows the Node by, plus one (so it is nonzero), with `MIMUSB_OK` recorded; or 0 with `MIMUSB_ERR_PARAM` recorded for a null or closed Node.

#### `mimusb_node_set_feature_response`

```c
mimusb_result mimusb_node_set_feature_response(mimusb_node* node, uint8_t report_id, const uint8_t* data, size_t length, uint32_t control_timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** Waits up to control_timeout_ms for the board's answer; inside a batch, or for a Node not yet committed, it is sent with the commit instead.
- **Synchronization:** Device domain. Sets what the board answers (the bytes after the Report ID, at most the board's max_feature_bytes; `mimusb_device_limits`) when the target reads Feature report report_id of this Node (profile Nodes only): an `ANSWER` rule the library writes, whose bytes it changes in place when the length stays and otherwise sends the table again. A Feature read with no answer set is stalled. A touch Spec with enable_contact_count_maximum_feature_declaration has its answer set already.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Node, null data with a nonzero length, or more than max_feature_bytes, or a Node opened with `mimusb_node_open`; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_OVERFLOW` when the board has no room; a link error.

#### `mimusb_node_endpoint_set_policy`

```c
mimusb_result mimusb_node_endpoint_set_policy(mimusb_node* node, uint8_t address, const mimusb_endpoint_policy* policy, uint32_t control_timeout_ms);
```

- **Ownership:** Copies policy.
- **Blocking:** As `mimusb_node_set_feature_response`.
- **Synchronization:** Device domain. Changes what an IN endpoint does when its queue is full (`MIMUSB_FULL_DROP_NEWEST`, `_DROP_OLDEST`, `_WAIT`) and how many transfers may wait. A queue of depth d takes d + 1 slots of the board's pool (2 * d + 1 for drop-oldest); the board refuses a change its queue cannot hold (`MIMUSB_ERR_OVERFLOW`) and nothing changes; a commit cuts the queues anew. On a HID Node the policy is drop-newest only, and Nodes sharing an interface share the least depth any of them set. A change resets the wait count.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Node, a null policy, an address that is not an IN endpoint of the Node or a bad on_full; `MIMUSB_ERR_UNSET_FIELD` for on_full or depth 0 or a zero timeout; `MIMUSB_ERR_UNSUPPORTED` for another policy on a HID Node; `MIMUSB_ERR_OVERFLOW`; a link error.

#### `mimusb_node_endpoint_write`

```c
mimusb_result mimusb_node_endpoint_write(mimusb_node* node, uint8_t address, const uint8_t* data, size_t length, uint32_t timeout_ms);
```

- **Ownership:** Copies data.
- **Blocking:** With the wait policy, until the board has room or timeout_ms has passed, with no lock of the Device held, so the event sink and other calls run; otherwise only until the serial driver has the frames (timeout_ms then bounds a write that makes no progress, and 0 is accepted).
- **Synchronization:** Device domain; writes to different endpoints may run at the same time, writes to one endpoint must not.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Node, an address that is not an IN endpoint of the Node, or null data with a nonzero length; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout with the wait policy; `MIMUSB_ERR_TIMEOUT` when timeout_ms passed with nothing sent; `MIMUSB_ERR_SHORT_TRANSFER` when it passed after some frames of a split write; `MIMUSB_ERR_BUSY` when the endpoint was flushed, stalled, reconfigured or unmounted while the write waited (nothing more is sent), or the Node is staged or the board enumerates; `MIMUSB_ERR_OVERFLOW` for more than 65535 frames; a link error.

Sends data as one USB transfer on an IN endpoint of a Node, a profile Node's included (its own report sends go through the same path, so its wait policy applies to them too). A length of 0 sends a zero-length packet (USB 2.0 5.8.3). A length above the board's `max_transfer_bytes` is sent as several frames, each a multiple of the endpoint's packet size except the last; the library never adds a zero-length packet (a caller whose class ends a transfer of an exact multiple of the packet size with one sends it with a second call of length 0). The wait policy counts per endpoint the frames written minus those the board reported finished (`EP_DONE`) and blocks while that equals the depth; a flush, a stall or clear, a policy change, an unmount and a commit set the count to zero and wake writers. The count never goes below zero.

#### `mimusb_node_endpoint_stall`, `mimusb_node_endpoint_clear_stall`, `mimusb_node_endpoint_flush`

```c
mimusb_result mimusb_node_endpoint_stall(mimusb_node* node, uint8_t address, uint32_t control_timeout_ms);
mimusb_result mimusb_node_endpoint_clear_stall(mimusb_node* node, uint8_t address, uint32_t control_timeout_ms);
mimusb_result mimusb_node_endpoint_flush(mimusb_node* node, uint8_t address, uint32_t control_timeout_ms);
```

- **Ownership:** Nothing.
- **Blocking:** A command to the board, waiting up to control_timeout_ms.
- **Synchronization:** Device domain. Stall sets and clear removes the Halt feature of an endpoint (USB 2.0 9.4.5); stalling an IN endpoint drops what waits and what is on the bus. Flush drops what waits on an IN endpoint (a transfer the controller already has completes). Each resets the wait count of an IN endpoint.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Node or an address that is not an endpoint of the Node (flush: not an IN endpoint); `MIMUSB_ERR_UNSUPPORTED` for a profile Node; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_BUSY` when the board does not run the configuration; a link error.

#### `mimusb_device_add_string`, `mimusb_device_remove_string`

```c
mimusb_result mimusb_device_add_string(mimusb_device* device, const char* utf8, uint8_t* out_index);
mimusb_result mimusb_device_remove_string(mimusb_device* device, uint8_t index);
```

- **Ownership:** Copies utf8.
- **Blocking:** None; the next commit sends the change.
- **Synchronization:** Device domain. Stores a string descriptor in the Device's language and returns its index (the first is 5) for the caller's own class descriptors; it stays until removed.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, a null argument, text that is not UTF-8 without control characters of at most 126 UTF-16 code units, or (remove) an index that was not given; `MIMUSB_ERR_OVERFLOW` when the board's `max_strings` or the index space is used up.

#### `mimusb_device_set_string`, `mimusb_device_clear_string`

```c
mimusb_result mimusb_device_set_string(mimusb_device* device, uint8_t index, uint16_t language_id, const uint16_t* units, size_t unit_count);
mimusb_result mimusb_device_clear_string(mimusb_device* device, uint8_t index, uint16_t language_id);
```

- **Ownership:** Copies the units.
- **Blocking:** None; the next commit sends the change.
- **Synchronization:** Device domain. Stores a string descriptor as it goes on the bus (index 1 to 255, a nonzero `language_id`, 1 to 126 UTF-16 code units, whatever they are); the same index and language replaces it. These are the strings a Device given as bytes serves, and only those: the strings of `usb_device`, `usb_config` and `mimusb_device_add_string` are not sent then.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, null units, an index or `language_id` of 0, a `unit_count` of 0 or above 126, or (clear) a pair that was not given; `MIMUSB_ERR_OVERFLOW` when `max_strings` is used up.

#### `mimusb_device_set_bos`

```c
mimusb_result mimusb_device_set_bos(mimusb_device* device, const uint8_t* bytes, size_t length);
```

- **Ownership:** Copies the bytes.
- **Blocking:** None; the next commit sends it.
- **Synchronization:** Device domain. For a Device composed from Nodes: the Binary Object Store descriptor (USB 3.2 9.6.2) the target reads; length 0 removes it. Checked without reading any capability: a 5-byte header of type `0x0F` whose `wTotalLength` is `length` and whose `bNumDeviceCaps` is the number of capability descriptors that follow, each of at least 3 bytes, type `0x10`, inside the bytes; at most `max_bos_bytes`. At the commit `usb_device.bcd_usb` must be `0x0201` or more (a host asks for a BOS only then).
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, null bytes with a length, or a descriptor that does not hold together (`offset` is where); `MIMUSB_ERR_OVERFLOW` for more than `max_bos_bytes`; `MIMUSB_ERR_BUSY` while the Device is given as bytes. The commit returns `MIMUSB_ERR_PARAM` (field `device.bos`) for a `bcd_usb` below `0x0201`.

#### `mimusb_device_set_descriptors`, `mimusb_device_clear_descriptors`

```c
mimusb_result mimusb_device_set_descriptors(mimusb_device* device, const mimusb_device_descriptors* descriptors, const mimusb_commit_timeouts* timeouts);
mimusb_result mimusb_device_clear_descriptors(mimusb_device* device, const mimusb_commit_timeouts* timeouts);
```

- **Ownership:** Copies the bytes.
- **Blocking:** Commits like `mimusb_node_open`; inside a batch it only stages.
- **Synchronization:** Device domain. Presents a whole device given as bytes (`device`, `configuration`, optionally `bos`), changing nothing in them and filling nothing in; `usb_device`, `usb_config` and their strings are not used. Checked without reading any class: an 18-byte device descriptor of type 1 with one configuration; `wTotalLength` of the configuration; every `bLength` at least 2 and inside the bytes; interface numbers 0, 1, 2, ... in order, each with alternate settings 0, 1, 2, ... in order and nothing between them; interrupt, bulk and (on a board that has them) isochronous endpoints of 7 bytes, or 9 in the audio-class form, with valid addresses within the board's limits, unique within a setting (an address may come again in another setting of the same interface: a bulk or interrupt endpoint with the same descriptor, an isochronous one with any packet size the board takes), and packet sizes `mimusb_node_open` would accept (isochronous: up to `max_transfer_bytes`, at most 1023 at full speed and 1024 at high speed, `bInterval` 1 at full speed); totals within `mimusb_board_limits`; a BOS as in `mimusb_device_set_bos`, with `bcdUSB` of `0x0201` or more. Every IN endpoint needs a policy given first (`mimusb_device_endpoint_set_policy`). Exclusive with Nodes. `clear` forgets the bytes and the policies and takes the Device off the target; the strings stay.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for an invalid Device, a null argument, a wrong `struct_size`, a nonzero `reserved` or bytes that do not hold together (`field` is `descriptors.device`, `.configuration` or `.bos`, `offset` the byte); `MIMUSB_ERR_UNSET_FIELD` for a zero timeout, a `bcdUSB` of 0 or an IN endpoint with no policy (its address is in `offset`); `MIMUSB_ERR_UNSUPPORTED` for an isochronous endpoint on a board that has none (`offset` is its `bmAttributes`); `MIMUSB_ERR_OVERFLOW` for the board's limits (a configuration whose isochronous endpoints need more packet memory than the controller has is refused by the board at the commit); `MIMUSB_ERR_BUSY` while a Node is open; the results of `mimusb_device_commit`.

#### `mimusb_device_endpoint_set_policy`, `_write`, `_stall`, `_clear_stall`, `_flush`

```c
mimusb_result mimusb_device_endpoint_set_policy(mimusb_device* device, uint8_t address, const mimusb_endpoint_policy* policy, uint32_t control_timeout_ms);
mimusb_result mimusb_device_endpoint_write(mimusb_device* device, uint8_t address, const uint8_t* data, size_t length, uint32_t timeout_ms);
mimusb_result mimusb_device_endpoint_stall(mimusb_device* device, uint8_t address, uint32_t control_timeout_ms);
mimusb_result mimusb_device_endpoint_clear_stall(mimusb_device* device, uint8_t address, uint32_t control_timeout_ms);
mimusb_result mimusb_device_endpoint_flush(mimusb_device* device, uint8_t address, uint32_t control_timeout_ms);
```

- **Ownership:** Copies policy and data.
- **Blocking:** As the `mimusb_node_endpoint_*` calls; the policy is only kept (no command) while the descriptors are not committed.
- **Synchronization:** Device domain. The endpoints of a Device given as bytes, named by address, with everything the Node calls say of splitting, zero-length packets, the wait policy and the wait count. A Device given as bytes may have alternate settings and, on a board whose `transfer_types` has `MIMUSB_TRANSFER_ISOCHRONOUS`, isochronous endpoints (audio and video classes): the target's `SET_INTERFACE` starts and stops the isochronous endpoints of the interface and is reported as `MIMUSB_EVENT_INTERFACE_ALTERNATE`; the board and the library add nothing to a stream, so what to write, when, and what a missing frame means is the program's. A policy for an address the descriptors do not have is kept for the next `mimusb_device_set_descriptors`.
- **Returns:** As the Node calls; `MIMUSB_ERR_PARAM` for an address the descriptors do not have (none set included; a policy needs only an IN address) and, for a write, a transfer to an isochronous endpoint longer than its packet size (the largest of its settings): it is one packet and is not split; `MIMUSB_ERR_BUSY` also while the descriptors are staged in a batch that has not ended.

#### `mimusb_node_manifest`

```c
mimusb_result mimusb_node_manifest(const mimusb_node* node, mimusb_capability_manifest* manifest);
```

- **Ownership:** As `mimusb_spec_manifest`; manifest->reports is valid while the Node is open.
- **Blocking:** None.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a null pointer, a wrong struct_size or a nonzero reserved.

#### `mimusb_node_submit`

```c
mimusb_result mimusb_node_submit(mimusb_node* node, uint32_t send_timeout_ms);
```

- **Ownership:** The report is built in the library's own buffer.
- **Blocking:** Writes one report of the Node's current state, if it changed, to the serial port and returns once the driver has it (microseconds on an idle port). It never waits for the target; send_timeout_ms only bounds a write that makes no progress.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`, also when there was nothing to send; `MIMUSB_ERR_PARAM` for an invalid Node; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_UNSUPPORTED` for a Raw Node (use `mimusb_raw_submit`); `MIMUSB_ERR_BUSY` for a Node opened in a batch that has not ended, while the board enumerates, or while a report of the Node is being sent; a link error.

#### `mimusb_node_submit_blocking`

```c
mimusb_result mimusb_node_submit_blocking(mimusb_node* node, uint32_t timeout_ms);
```

- **Ownership:** As `mimusb_node_submit`.
- **Blocking:** Writes reports until the Node's state has been sent, a touch frame of several reports and a mouse's pending movement included, or timeout_ms has passed; each write is bounded by what is left of it.
- **Synchronization:** Device domain.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_TIMEOUT` when timeout_ms passed first; the results of `mimusb_node_submit`.

### Profile input calls

#### `mimusb_keyboard_key`

```c
mimusb_result mimusb_keyboard_key(mimusb_node* node, uint16_t usage, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) one Keyboard page Usage, a modifier or a key of the Spec's range. Closing the Node releases every key still down.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_mouse_move`

```c
mimusb_result mimusb_mouse_move(mimusb_node* node, int32_t dx, int32_t dy);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Adds to the pending X and Y movement. A report carries as much as the fields hold and keeps the rest for the next one.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`; `MIMUSB_ERR_OVERFLOW` when the 64-bit pending total overflows.

#### `mimusb_mouse_scroll`

```c
mimusb_result mimusb_mouse_scroll(mimusb_node* node, int32_t wheel, int32_t pan);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Adds to the pending wheel and pan movement, as `mimusb_mouse_move` does.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`; `MIMUSB_ERR_UNSUPPORTED` for a nonzero value on a field the Spec lacks; `MIMUSB_ERR_OVERFLOW` when the 64-bit pending total overflows.

#### `mimusb_mouse_button`

```c
mimusb_result mimusb_mouse_button(mimusb_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) button 1 to button_count.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_toggle_usage`

```c
mimusb_result mimusb_toggle_usage(mimusb_node* node, uint16_t usage, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) one Usage of the Spec's allow-list; any number may be pressed at once. Closing the Node releases every Usage still down.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_gamepad_button`

```c
mimusb_result mimusb_gamepad_button(mimusb_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) button 1 to button_count.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_gamepad_axis`

```c
mimusb_result mimusb_gamepad_axis(mimusb_node* node, size_t axis_index, int32_t value);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets axes[axis_index] of the Spec to value, which must be in its logical range.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_gamepad_dpad`

```c
mimusb_result mimusb_gamepad_dpad(mimusb_node* node, uint32_t up, uint32_t down, uint32_t right, uint32_t left);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the D-pad from four 0/1 directions. With a Hat, up and down (or left and right) together are refused and no direction sends the Null state; with four buttons the bits go out as given.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`; `MIMUSB_ERR_PARAM` also when the Spec has no D-pad.

#### `mimusb_touchscreen_contact`

```c
mimusb_result mimusb_touchscreen_contact(mimusb_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y, const mimusb_touchscreen_extra* extra);
```

- **Ownership:** Copies extra, which may be null (every optional field 0).
- **Blocking:** None.
- **Synchronization:** Device domain. down 1 places contact_id in a free slot, or moves it if it is already down; down 0 lifts it at x, y. A lifted contact_id can be placed again once its lift has been sent. While a frame split over several reports is being sent, every call is refused with `MIMUSB_ERR_BUSY`.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`; `MIMUSB_ERR_PARAM` also for lifting a contact_id that is not down; `MIMUSB_ERR_BUSY` also for placing one whose lift is unsent; `MIMUSB_ERR_OVERFLOW` when every slot is taken.

#### `mimusb_touchpad_contact`

```c
mimusb_result mimusb_touchpad_contact(mimusb_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y, const mimusb_touchpad_extra* extra);
```

- **Ownership:** Copies extra, which may be null.
- **Blocking:** None.
- **Synchronization:** Device domain. As `mimusb_touchscreen_contact`, for a Touchpad Node.
- **Returns:** As `mimusb_touchscreen_contact`, `MIMUSB_OK` through `MIMUSB_ERR_OVERFLOW`.

#### `mimusb_touchpad_button`

```c
mimusb_result mimusb_touchpad_button(mimusb_node* node, uint32_t button, uint32_t down);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Presses (down 1) or releases (down 0) click button 1 to button_count.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_pen_update`

```c
mimusb_result mimusb_pen_update(mimusb_node* node, const mimusb_pen_sample* sample);
```

- **Ownership:** Copies sample.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the whole pen state. in_range 0 takes the pen away: In Range, Tip, pressure, Invert and the barrel bits go out as 0 with the sample's own X, Y, tilt and twist, so repeat the last position to leave the pointer where it was. Switching between tip and eraser while in range first sends an out-of-range report.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_digitizer_update`

```c
mimusb_result mimusb_digitizer_update(mimusb_node* node, const mimusb_digitizer_sample* sample);
```

- **Ownership:** Copies sample.
- **Blocking:** None.
- **Synchronization:** Device domain. As `mimusb_pen_update`, for a Digitizer Node.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`.

#### `mimusb_battery_update`

```c
mimusb_result mimusb_battery_update(mimusb_node* node, int32_t strength);
```

- **Ownership:** Nothing.
- **Blocking:** None.
- **Synchronization:** Device domain. Sets the battery level of a Node whose Spec has enable_battery. It goes out with the next `mimusb_node_submit` that has no input report to send, and only when it changed. A level not yet sent is dropped on close.
- **Returns:** As every input call, `MIMUSB_OK` through `MIMUSB_ERR_NO_DEVICE`; `MIMUSB_ERR_PARAM` also for a Node without a battery report.

### Raw reports

#### `mimusb_raw_submit`

```c
mimusb_result mimusb_raw_submit(mimusb_node* node, const uint8_t* report, size_t length, uint32_t send_timeout_ms);
```

- **Ownership:** Copies report.
- **Blocking:** As `mimusb_node_submit`.
- **Synchronization:** Device domain. Sends one report of a Raw Spec's Node after checking it against the Spec's report table.
- **Returns:** `MIMUSB_OK`; `MIMUSB_ERR_PARAM` for a Node that is not Raw, a null report, a Report ID or length the Spec does not accept, or nonzero padding bits; `MIMUSB_ERR_UNSET_FIELD` for a zero timeout; `MIMUSB_ERR_BUSY` as for `mimusb_node_submit`; `MIMUSB_ERR_OVERFLOW` for a report longer than maximum_report_bytes; a link error.

---

## C++20 header (`include/mimusb.hpp`)

[include/mimusb.hpp](../include/mimusb.hpp) wraps every function of `mimusb.h`
under `namespace mimusb` with the `mimusb_` prefix removed, adds factories that
return a structure after its `*_init` (`mimusb::device_config()`, `mimusb::commit_timeouts()`,
`mimusb::node_config()`, `mimusb::keyboard_options()`, etc.), and provides RAII
owning handles for the two types whose release cannot fail:

- `mimusb::SpecPtr` (`std::unique_ptr<mimusb_spec, mimusb::SpecRelease>`)
- `mimusb::DiscoveryPtr` (`std::unique_ptr<mimusb_discovery, mimusb::DiscoveryDestroy>`)

`mimusb::device_link_counters` takes `bool reset` in place of `uint32_t reset`.

```cpp
#include "mimusb.hpp"

mimusb::SpecPtr make_keyboard_spec() {
    auto kb = mimusb::keyboard_options();
    kb.usage_minimum = 0x04;
    kb.usage_maximum = 0xDD;
    mimusb_spec* raw_spec = nullptr;
    if (mimusb::spec_create_keyboard(&kb, &raw_spec) == MIMUSB_OK) {
        return mimusb::SpecPtr{raw_spec};
    }
    return nullptr;
}
```

---

## Result codes (`mimusb_result`)

`libmimusb` and `libaoahid` give a code with the same meaning the same number.
The table lists both libraries; a dash means the library has no such code.

| Value | `libmimusb` | `libaoahid` | Meaning in `libmimusb` |
|---:|---|---|---|
| 0 | `MIMUSB_OK` | `AOAHID_OK` | The call succeeded |
| 1 | `MIMUSB_ERR_PARAM` | `AOAHID_ERR_PARAM` | A null pointer, a wrong `struct_size`, a nonzero reserved field, a flag other than 0 or 1, a value out of range, the wrong kind of Node; or the board's `MIMUSB_NAK_PARAM` |
| 2 | `MIMUSB_ERR_UNSET_FIELD` | `AOAHID_ERR_UNSET_FIELD` | A required field or timeout argument was zero |
| 3 | `MIMUSB_ERR_UNSUPPORTED` | `AOAHID_ERR_UNSUPPORTED` | Not possible with this Spec, board, or link: a rate the board or adapter cannot carry, a `configuration_value` other than 1, an interface class other than HID, `mimusb_node_submit` on a Raw Node; or the board's `MIMUSB_NAK_UNSUPPORTED` |
| 4 | `MIMUSB_ERR_PROTOCOL` | `AOAHID_ERR_NOT_AOA` | The board's firmware has another major or minor version than the library |
| 5 | — | — | Not used by either library |
| 6 | `MIMUSB_ERR_ACCESS` | `AOAHID_ERR_ACCESS` | The serial port could not be opened |
| 7 | `MIMUSB_ERR_BUSY` | `AOAHID_ERR_BUSY` | The call does not fit the current state: a batch already open or not open, a Node opened in a batch that has not ended, the board re-enumerating, a report of the Node being sent; or the board's `MIMUSB_NAK_STATE` or `MIMUSB_NAK_BUSY` (for example `mimusb_device_remote_wakeup` when the target has not suspended the device) |
| 8 | `MIMUSB_ERR_NO_DEVICE` | `AOAHID_ERR_NO_DEVICE` | The serial port failed or was unplugged; every later call on the Device returns it until `mimusb_device_close` |
| 9 | — | `AOAHID_ERR_STALL` | Not used by `libmimusb` |
| 10 | `MIMUSB_ERR_TIMEOUT` | `AOAHID_ERR_TIMEOUT` | No board answered the handshake, a frame was not answered in time, a serial write made no progress, or the target did not mount the device within `mimusb_device_wait_ready`'s timeout |
| 11 | `MIMUSB_ERR_SHORT_TRANSFER` | `AOAHID_ERR_SHORT_TRANSFER` | A serial write stopped part-way through a frame |
| 12 | `MIMUSB_ERR_DESCRIPTOR_REJECTED` | `AOAHID_ERR_DESCRIPTOR_REJECTED` | The board refused the configuration (`MIMUSB_NAK_DESCRIPTOR`). `libaoahid` defines the code and returns it from no call |
| 13 | `MIMUSB_ERR_IO` | `AOAHID_ERR_IO` | Any other operating-system error on the serial port |
| 14 | `MIMUSB_ERR_OVERFLOW` | `AOAHID_ERR_OVERFLOW` | A size, count, or limit was exceeded: of the protocol, USB, HID, `linux_hid`, or the board; or the board's `MIMUSB_NAK_OVERFLOW` |
| 15 | — | `AOAHID_CLOSE_PENDING` | Not used by `libmimusb`: its writes finish within the call |
| 16 | `MIMUSB_ERR_INTERNAL` | `AOAHID_ERR_INTERNAL` | An allocation failed, an internal invariant broke, or the board sent a `NAK` code the library does not know |

---

## Differences from `libaoahid`

| Area | `libaoahid` (Android Open Accessory 2.0) | `libmimusb` (MCU USB-HID bridge) |
|---|---|---|
| **Target and transport** | An Android phone or tablet directly over USB (`libusb`, AOA control requests 51 to 57 on endpoint 0) | Any USB host through an MCU board on a host serial port (`termios` or Win32) |
| **Top-level handles** | `aoahid_context*`, `aoahid_discovery*`, `aoahid_device*`, `aoahid_spec*`, `aoahid_node*`, `aoahid_channel*` | No Context: `mimusb_discovery*`, `mimusb_device*`, `mimusb_spec*`, `mimusb_node*` |
| **Initializers (`*_init`)** | Fill in values that work with a current Android phone; the header has `AOAHID_CONTROL_TIMEOUT_MS`, `AOAHID_SEND_TIMEOUT_MS`, and `AOAHID_DRAIN_TIMEOUT_MS` | Only zero the structure and set `struct_size`; there are no timeout constants, and every required field is set by the caller |
| **USB descriptors** | The phone's own; `aoahid_accessory_start` sends only the AOA identification strings | The caller gives the Device, Configuration, Interface, and Endpoint descriptors (`mimusb_device_config`, `mimusb_node_config`) |
| **Opening and closing a Node** | `ACCESSORY_REGISTER_HID` (request 54) and `ACCESSORY_SET_HID_REPORT_DESC` (56) on open, `ACCESSORY_UNREGISTER_HID` (55) on close, each waiting up to `control_timeout_ms` | `const mimusb_commit_timeouts*` (`control_timeout_ms` and `reconnect_timeout_ms`); the board re-enumerates, or the change is staged inside `mimusb_device_batch_begin` and `mimusb_device_batch_end` |
| **Submit calls** | `aoahid_node_submit` and `aoahid_node_submit_blocking` | `mimusb_node_submit` (one report) and `mimusb_node_submit_blocking` (until multi-packet frames and relative remainders are sent) |
| **Two-way HID and bus control** | Input only; bulk `aoahid_channel` for side interfaces such as ADB | Output reports, Feature answers, and bus events through `transport.event_sink`, `mimusb_node_set_feature_response`, `mimusb_device_disconnect`, `mimusb_device_connect`, `mimusb_device_remote_wakeup`, and endpoint 0 control rules (`mimusb_device_set_control_rules`, `mimusb_device_control_reply`) |
| **Result codes** | Also `AOAHID_ERR_NOT_AOA` (4), `AOAHID_ERR_STALL` (9), and `AOAHID_CLOSE_PENDING` (15) | `MIMUSB_ERR_PROTOCOL` (4); 5, 9, and 15 are not used |
