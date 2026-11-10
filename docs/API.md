# API Reference

`libmimhid` exposes a stable C11 ABI in [include/mimhid.h](../include/mimhid.h)
and an optional header-only C++20 facade in
[include/mimhid.hpp](../include/mimhid.hpp). Every exported symbol begins with
`mimhid_`.

## Objects and ownership

The API has four opaque handle types:

| Handle | Created by | Destroyed by | Ownership & lifetime |
|---|---|---|---|
| `mimhid_discovery*` | `mimhid_discover` | `mimhid_discovery_destroy` | Owns the snapshot of `mimhid_device_info` records found during a port scan. Independent of any `Device`. |
| `mimhid_spec*` | `mimhid_spec_create_*` | `mimhid_spec_release` | Immutable, reference-counted HID descriptor, wire layout, and capability manifest. Independent of any `Device`; may be shared across threads and across multiple `Node` handles. |
| `mimhid_device*` | `mimhid_device_open` | `mimhid_device_close` | Owns the serial port, reader thread, command locks, and the board's active USB configuration. Closing a `Device` automatically closes any `Node` still attached to it. |
| `mimhid_node*` | `mimhid_node_open`, `mimhid_node_open_raw` | `mimhid_node_close` (or `mimhid_device_close`) | One HID collection/endpoint on an open `Device`. Holds one reference to its `mimhid_spec` (when opened with `mimhid_node_open`). Do not pass a `mimhid_node*` to `mimhid_node_close` after `mimhid_device_close` has consumed its parent `Device`. |

## Rules every function follows

### Return codes and `mimhid_last_error()`

Every fallible function returns a `mimhid_result` (`MIMHID_OK == 0` on success),
and every entry point records the calling thread's diagnostic in thread-local
storage before returning. Call `mimhid_last_error()` on the same thread to read
`const mimhid_error_detail*`:

- `code`: the `mimhid_result` code (`MIMHID_OK` when the last call succeeded).
- `field`: static string naming the argument, struct field, or step that failed
  (for example, `"usb_device.vendor_id"` or `"keyboard.usage_range"`), or `NULL`.
- `reason`: one English sentence explaining the failure.
- `transport_status`: `errno` or Win32 error of a failed serial call, or the
  board's `MIMHID_NAK_*` code (`1` bad value, `2` no room, `3` wrong state, `4`
  descriptor refused, `5` unsupported, `6` busy).
- `frame_type`: wire frame type involved (`mimhid_frame_type_name(detail->frame_type)`).
- `node_id`, `report_id`, `offset`, `length`: context populated for Node, report,
  or descriptor validation errors.

### Structures and initial values (`*_init`)

Every public options or output structure with `struct_size` has a static inline
`mimhid_<type>_init(s)` helper in `mimhid.h` (`mimhid_device_config_init`,
`mimhid_commit_timeouts_init`, `mimhid_node_config_init`,
`mimhid_board_limits_init`, `mimhid_link_counters_init`,
`mimhid_keyboard_options_init`, `mimhid_mouse_options_init`,
`mimhid_toggle_options_init`, `mimhid_gamepad_options_init`,
`mimhid_touchscreen_options_init`, `mimhid_touchpad_options_init`,
`mimhid_pen_options_init`, `mimhid_digitizer_options_init`,
`mimhid_raw_options_init`, and `mimhid_capability_manifest_init`).

> [!IMPORTANT]
> Every `*_init` function **only zeroes the structure and sets `struct_size`**.
> It fills in no default vendor ID, baud rate, timeout, endpoint address, or
> profile range. Leaving a required field zero returns `MIMHID_ERR_UNSET_FIELD`
> (or `MIMHID_ERR_PARAM`). Every enumeration starts at `1` so a zero-initialized
> field never selects a mode by accident. Reserved fields (`reserved`,
> `reserved8`, `reserved16`) must remain zero, and boolean flags must be `0` or `1`.

| Structure | Required non-zero fields | Optional (zero means off / absent) |
|---|---|---|
| `mimhid_transport_options` | `baud_rate` (or `MIMHID_BAUD_FASTEST`), `handshake_timeout_ms`, `maximum_report_bytes`, all 6 fields of `linux_hid` (`descriptor_bytes`, `fields_per_report`, `global_stack_depth`, `report_size_bits`, `usages`, `report_data_bits`), `log_level` (`MIMHID_LOG_DISABLED..MIMHID_LOG_TRACE`, plus `log_sink` when not `MIMHID_LOG_DISABLED`) | `validate_reports`, `keep_attached`, `event_sink`, `event_user`, `log_user` |
| `mimhid_usb_device` | `bcd_usb`, `ep0_max_packet_size` (`8`, `16`, `32`, or `64`; `64` at High-Speed), `vendor_id` (`1..0xFFFF`), `language_id` (`0x0409`, etc.) | `device_class`/`subclass`/`protocol`, `product_id` (`0..0xFFFF`), `bcd_device` (`0..0xFFFF`), `manufacturer`, `product`, `serial`, `serial_from_chip_id` |
| `mimhid_usb_config` | `configuration_value` (`1`), `bm_attributes` (bit 7 set, bits 4..0 clear) | `bmax_power` (`0..250`, in 2 mA units), `string` |
| `mimhid_device_config` | `struct_size`, `transport`, `usb_device`, `usb_config` | — |
| `mimhid_commit_timeouts` | `struct_size`, `control_timeout_ms`, `reconnect_timeout_ms` | — |
| `mimhid_node_config` | `struct_size`, `iface.class_code` (`0x03`), `iface.bcd_hid`, `ep_in.address` (`0x81..0x8F`), `ep_in.poll_interval_ms`, `ep_in.max_packet_size` | `iface.number`, `iface.subclass`/`protocol`/`country_code`/`name`, `ep_out` (all zero when unused) |
| `mimhid_control_rule` | `timeout_ms` (`1..MIMHID_CONTROL_MAX_TIMEOUT_MS`), `when_late` (`MIMHID_CONTROL_ACTION_*`) | `request_type_mask`/`value`, `request_mask`/`value`, `value_mask`/`value`, `index_mask`/`value` |

### The close rule

`mimhid_device_close`, `mimhid_node_close`, `mimhid_discovery_destroy`, and
`mimhid_spec_release` follow one rule:

- If the call returns `MIMHID_ERR_PARAM` or `MIMHID_ERR_UNSET_FIELD` (for
  example, a null handle, an already-closed Node, or a zero timeout), **nothing
  was done** and the handle remains the caller's.
- For **any other result**, the object is **consumed and freed** and its handle
  must not be used again; an error return reports what failed on the way out.

### Threading and domains

- **Device domain:** A `mimhid_device*` and all of its `mimhid_node*` handles
  form one synchronization domain. The caller ensures no two calls in the same
  Device domain run concurrently, except where explicitly noted
  (`mimhid_device_is_ready`, and `mimhid_device_control_reply` called from
  `transport.event_sink`).
- **Outside any domain:** `mimhid_spec*` and `mimhid_discovery*` handles are
  independent of any Device; `mimhid_spec_retain`, `mimhid_spec_release`,
  `mimhid_spec_descriptor`, `mimhid_spec_manifest`, and passing the same
  `mimhid_spec*` to multiple Devices or Nodes are safe from any thread.
- **Callbacks (`event_sink`, `log_sink`):** `transport.event_sink` runs on the
  Device's reader thread. `event->data` is valid only for the duration of the
  callback. `event_sink` must not call back into `libmimhid` except to call
  `mimhid_device_control_reply`.

---

## Lifecycle and USB bus behavior

1. **Before the first commit (`0 V` stealth state):** when the board boots and
   when `mimhid_device_open` completes the UART handshake, the board's USB `D+`
   pull-up remains disabled (`0 V`). The target PC sees no USB device at all.
2. **Opening Nodes and committing:** each `mimhid_node_open`,
   `mimhid_node_open_raw`, `mimhid_node_close`, `mimhid_device_set_usb_identity`,
   `mimhid_device_batch_end`, or `mimhid_device_commit` streams `DEVICE_CONFIG`,
   `NODE_BEGIN`, `NODE_DATA`, `FEATURE`, `QUEUE_DEPTH`, and `COMMIT` frames to
   the board:
   - On the **first** commit, the board builds the USB descriptors, pulls `D+`
     high (`tud_connect()`), and the host waits up to `reconnect_timeout_ms` for
     the board to report `MOUNTED` (`SET_CONFIGURATION` from the target OS).
   - On a **subsequent** commit while the board is already attached, the host
     first sends a detach `COMMIT` (`Link::disconnect`), sleeps
     `reconnect_delay_ms` on the host when `mimhid_device_commit` is given a
     nonzero `reconnect_delay_ms`, and then sends an attach `COMMIT`
     (`Link::connect`) and waits up to `reconnect_timeout_ms` for `MOUNTED`.
   - When the **last** live Node is closed (`mimhid_node_close`), the library
     sends a detach `COMMIT` so the board drops off the target USB bus.
3. **Sharing vs separating USB interfaces:**
   - Up to `MIMHID_MAX_NODES` (`16`) Nodes can be open on a `Device`, packed
     across the board's `max_interfaces` (`15` on RP2040/RP2350, `7` on
     nRF52840, `4` on ESP32-S3, `3` on STM32F411).
   - Nodes given the **same** `iface.number` share one USB HID interface: their
     `iface`, `ep_in`, and `ep_out` settings must match, and every Node on that
     shared interface must declare a distinct nonzero `Report ID` (`1..255`).
   - Nodes on **different** `iface.number` values must use distinct endpoint
     numbers (`1..max_endpoint_number`).

---

## Function reference

### 1. Diagnostics and version

#### `mimhid_last_error`

```c
const mimhid_error_detail* mimhid_last_error(void);
```

- **Purpose:** Returns the calling thread's `mimhid_error_detail` record for the
  most recent `libmimhid` call on that thread.
- **Ownership & threading:** Thread-local pointer valid until the calling
  thread's next `libmimhid` call; never `NULL`.

#### `mimhid_result_name`

```c
const char* mimhid_result_name(mimhid_result result);
```

- **Purpose:** Returns the constant name of `result` (for example,
  `"MIMHID_ERR_TIMEOUT"`), or `"MIMHID_RESULT_UNKNOWN"` (recording
  `MIMHID_ERR_PARAM`) for an unknown integer.
- **Ownership & threading:** Static string; safe from any thread.

#### `mimhid_frame_type_name`

```c
const char* mimhid_frame_type_name(int32_t frame_type);
```

- **Purpose:** Names the wire frame type in `mimhid_error_detail.frame_type`
  (`"PING"`, `"DEVICE_CONFIG"`, `"NODE_BEGIN"`, `"NODE_DATA"`, `"COMMIT"`,
  `"FEATURE"`, `"RESET"`, `"REMOTE_WAKEUP"`, `"SET_BAUD"`, `"CONTROL_CONFIG"`,
  `"CONTROL_REPLY"`, `"LOSS_CONFIG"`, `"QUEUE_DEPTH"`, `"PROBE"`, `"COUNTERS"`,
  `"REPORT"`, `"PONG"`, `"ACK"`, `"NAK"`, `"EVENT"`), or `"UNKNOWN"` (recording
  `MIMHID_ERR_PARAM`).
- **Ownership & threading:** Static string; safe from any thread.

#### `mimhid_version`

```c
uint32_t mimhid_version(void);
```

- **Purpose:** Returns `(MIMHID_VERSION_MAJOR << 24) | (MIMHID_VERSION_MINOR << 16) | MIMHID_VERSION_PATCH`
  of the running library binary.
- **Threading:** Safe from any thread.

---

### 2. Discovery

#### `mimhid_discover`

```c
mimhid_result mimhid_discover(uint32_t timeout_ms, mimhid_discovery** out_discovery);
```

- **Purpose:** Scans the host's candidate USB serial ports in parallel (skipping
  ports held open by another process), pings each at `MIMHID_BOOT_BAUD` (`115200`)
  waiting up to `timeout_ms`, sends a UART `BREAK` and pings again if silent,
  measures the fastest clean baud rate (`info->baud_rate`) on responsive boards
  using `PROBE` bursts and `COUNTERS`, returns the board to `MIMHID_BOOT_BAUD`,
  and stores the snapshot in `*out_discovery`.
- **Returns:** `MIMHID_OK` (including when 0 boards are found),
  `MIMHID_ERR_PARAM` (`out_discovery == NULL`), `MIMHID_ERR_UNSET_FIELD`
  (`timeout_ms == 0`).
- **Ownership:** Caller destroys `*out_discovery` with `mimhid_discovery_destroy`.

#### `mimhid_discovery_count`, `mimhid_discovery_get`, and `mimhid_discovery_destroy`

```c
size_t mimhid_discovery_count(const mimhid_discovery* discovery);
const mimhid_device_info* mimhid_discovery_get(const mimhid_discovery* discovery, size_t index);
void mimhid_discovery_destroy(mimhid_discovery* discovery);
```

- **Purpose:** Inspect and free a discovery snapshot. `mimhid_discovery_get`
  returns `NULL` (recording `MIMHID_ERR_PARAM`) when `discovery == NULL` or
  `index >= count`. The returned `mimhid_device_info*` belongs to `discovery`
  until `mimhid_discovery_destroy(discovery)`.

---

### 3. Device lifecycle, commit, bus control, and queries

#### `mimhid_device_open`

```c
mimhid_result mimhid_device_open(const char* port_path,
                                 const mimhid_device_config* config,
                                 mimhid_device** out_device);
```

- **Purpose:** Validates `config`, opens `port_path` exclusively, finds the board
  at `MIMHID_BOOT_BAUD` (or after a UART `BREAK`), checks `firmware_compatible`,
  switches the link to `config->transport.baud_rate` (or measures the fastest
  clean rate when `MIMHID_BAUD_FASTEST`) and verifies it with a `PROBE` burst and
  `COUNTERS`, and starts the reader thread. The board's USB device stays off the
  target bus (`D+` at `0 V`) until a Node is opened and committed.
- **Returns:** `MIMHID_OK`, `MIMHID_ERR_PARAM`, `MIMHID_ERR_UNSET_FIELD`,
  `MIMHID_ERR_UNSUPPORTED` (unsupported baud rate or High-Speed `ep0_max_packet_size != 64`),
  `MIMHID_ERR_OVERFLOW` (`maximum_report_bytes > board.max_report_bytes`),
  `MIMHID_ERR_TIMEOUT` (no board answered), `MIMHID_ERR_PROTOCOL` (incompatible
  firmware major/minor version), `MIMHID_ERR_ACCESS`, `MIMHID_ERR_BUSY`,
  `MIMHID_ERR_IO`.

#### `mimhid_device_close`

```c
mimhid_result mimhid_device_close(mimhid_device* device,
                                  uint32_t control_timeout_ms,
                                  uint32_t drain_timeout_ms);
```

- **Purpose:** Sends neutral release reports for all open Nodes within
  `drain_timeout_ms`, detaches the board from the target USB bus unless
  `transport.keep_attached == 1`, clears control routing and loss reporting,
  returns the board to `MIMHID_BOOT_BAUD`, stops the reader thread, closes the
  serial port, and frees `device` and its Nodes.
- **Returns:** `MIMHID_OK`, `MIMHID_ERR_PARAM`, `MIMHID_ERR_UNSET_FIELD`,
  `MIMHID_ERR_NO_DEVICE`, or the first link error encountered on the way out.

#### `mimhid_device_set_usb_identity`

```c
mimhid_result mimhid_device_set_usb_identity(mimhid_device* device,
                                             const mimhid_usb_device* usb_device,
                                             const mimhid_usb_config* usb_config,
                                             const mimhid_commit_timeouts* timeouts);
```

- **Purpose:** Replaces the USB Device and Configuration descriptor settings of
  `device` and commits (or stages the change when inside a batch).

#### `mimhid_device_batch_begin`, `mimhid_device_batch_end`, and `mimhid_device_commit`

```c
mimhid_result mimhid_device_batch_begin(mimhid_device* device);
mimhid_result mimhid_device_batch_end(mimhid_device* device, const mimhid_commit_timeouts* timeouts);
mimhid_result mimhid_device_commit(mimhid_device* device,
                                   uint32_t reconnect_delay_ms,
                                   const mimhid_commit_timeouts* timeouts);
```

- **Purpose:**
  - `mimhid_device_batch_begin`: begins a batch so subsequent `mimhid_node_open`,
    `mimhid_node_open_raw`, `mimhid_node_close`, and
    `mimhid_device_set_usb_identity` calls only stage their changes (`MIMHID_ERR_BUSY`
    if already in a batch).
  - `mimhid_device_batch_end`: commits all staged changes in a single USB
    re-enumeration (`MIMHID_ERR_BUSY` if not in a batch).
  - `mimhid_device_commit`: uploads the configuration of every open Node and
    commits (also ending any open batch). When the board was already attached and
    `reconnect_delay_ms > 0`, the host detaches the board, keeps `D+` at `0 V`
    for `reconnect_delay_ms` milliseconds, and then reconnects.

#### `mimhid_device_connect`, `mimhid_device_disconnect`, and `mimhid_device_remote_wakeup`

```c
mimhid_result mimhid_device_connect(mimhid_device* device, const mimhid_commit_timeouts* timeouts);
mimhid_result mimhid_device_disconnect(mimhid_device* device, uint32_t control_timeout_ms);
mimhid_result mimhid_device_remote_wakeup(mimhid_device* device, uint32_t control_timeout_ms);
```

- **Purpose:**
  - `mimhid_device_disconnect`: pulls `D+` to `0 V` on the target bus (`COMMIT`
    with `detach = 1`) while keeping all Nodes open on `device`.
  - `mimhid_device_connect`: re-attaches the board (`D+` pull-up on) or runs a
    full commit if not yet configured.
  - `mimhid_device_remote_wakeup`: sends a USB remote wakeup signaling pulse to a
    suspended target host (`MIMHID_ERR_BUSY` if not suspended).

#### `mimhid_device_is_ready`, `mimhid_device_wait_ready`, and `mimhid_device_latched_error`

```c
int32_t mimhid_device_is_ready(const mimhid_device* device);
mimhid_result mimhid_device_wait_ready(mimhid_device* device, uint32_t timeout_ms);
mimhid_result mimhid_device_latched_error(mimhid_device* device);
```

- **Purpose:**
  - `mimhid_device_is_ready`: returns `1` while the target has mounted the board's
    USB device, else `0` (safe from any thread while `device` is open).
  - `mimhid_device_wait_ready`: blocks up to `timeout_ms` (`> 0`) for the target
    to mount the board's USB device (`MIMHID_OK`, `MIMHID_ERR_TIMEOUT`,
    `MIMHID_ERR_NO_DEVICE`, `MIMHID_ERR_UNSET_FIELD`).
  - `mimhid_device_latched_error`: returns and clears any asynchronous link error
    recorded by the reader thread (`MIMHID_ERR_NO_DEVICE` stays latched until
    close).

#### `mimhid_device_board_info`, `mimhid_device_limits`, and `mimhid_device_link_counters`

```c
mimhid_result mimhid_device_board_info(const mimhid_device* device, mimhid_device_info* out_info);
mimhid_result mimhid_device_limits(const mimhid_device* device, mimhid_board_limits* out_limits);
mimhid_result mimhid_device_link_counters(mimhid_device* device,
                                          uint32_t reset,
                                          uint32_t control_timeout_ms,
                                          mimhid_link_counters* out_counters);
```

- **Purpose:**
  - `mimhid_device_board_info`: copies the `mimhid_device_info` recorded at open
    into `*out_info` without serial I/O.
  - `mimhid_device_limits`: fills `*out_limits` (initialize with
    `mimhid_board_limits_init` first) without serial I/O.
  - `mimhid_device_link_counters`: sends `COUNTERS` to the board (resetting the
    board's counters after reading when `reset != 0`) and fills `*out_counters`
    (initialize with `mimhid_link_counters_init` first) with `bad_crc`,
    `framing`, `uart_overrun`, `ring_overrun`, and `probes_good`.

#### `mimhid_device_set_loss_reporting`

```c
mimhid_result mimhid_device_set_loss_reporting(mimhid_device* device,
                                               uint32_t enabled,
                                               uint32_t interval_ms,
                                               uint32_t control_timeout_ms);
```

- **Purpose:** Enables (`enabled = 1`, `interval_ms` in `0..MIMHID_LOSS_MAX_INTERVAL_MS`
  where `0` reports immediately) or disables (`enabled = 0, interval_ms = 0`)
  `MIMHID_EVENT_REPORT_LOST` events when a board interface queue overflows.

---

### 4. Control transfers of the target (endpoint 0)

#### `mimhid_device_set_control_routing`

```c
mimhid_result mimhid_device_set_control_routing(mimhid_device* device,
                                                uint32_t report_all,
                                                const mimhid_control_rule* rules,
                                                size_t rule_count,
                                                uint32_t control_timeout_ms);
```

- **Purpose:** Configures endpoint 0 setup-packet reporting (`report_all` `0` or
  `1`) and up to `MIMHID_CONTROL_MAX_RULES` (`8`) hold rules (`rules[0..rule_count-1]`).
  Matching requests are reported to `transport.event_sink` as
  `MIMHID_EVENT_CONTROL_SETUP` with `MIMHID_CONTROL_FLAG_HELD` and held for up to
  `rule.timeout_ms` (`1..5000` ms), after which the board applies `rule.when_late`
  if no reply arrived.
- **Unheld requests (`MIMHID_CONTROL_FLAG_FIXED`):** Standard `OUT` `SET_ADDRESS`
  and `SET_CONFIGURATION(1)` (`bmRequestType == 0x00, wValue == 1, wIndex == 0, wLength == 0`)
  are reported with `MIMHID_CONTROL_FLAG_FIXED` and **never held**, because the
  board's own USB state depends on them. Host-to-device transfers with
  `wLength > max_control_data_bytes` are reported with
  `MIMHID_CONTROL_FLAG_TOO_LONG` and never held.

#### `mimhid_device_control_reply`

```c
mimhid_result mimhid_device_control_reply(mimhid_device* device,
                                          uint32_t token,
                                          mimhid_control_action action,
                                          const uint8_t* data,
                                          size_t length,
                                          uint32_t send_timeout_ms);
```

- **Purpose:** Answers the held transfer identified by `token` (`1..255`, from
  `event->control_token`):
  - `MIMHID_CONTROL_ACTION_LEAVE` (`1`): let the board's own TinyUSB handler
    process or acknowledge the request (`data == NULL, length == 0`).
  - `MIMHID_CONTROL_ACTION_ANSWER` (`2`): supply the `IN` data stage
    (`0..max_control_data_bytes`; sent whole even when `wLength == 0` so devices
    that ignore a zero `wLength` can be emulated), or accept an `OUT` setup/data
    stage (`length == 0`).
  - `MIMHID_CONTROL_ACTION_STALL` (`3`): stall endpoint 0 (`length == 0`).
  - `MIMHID_CONTROL_ACTION_DROP` (`4`): disarm endpoint 0 `IN` and `OUT` without
    stalling (`hal_control_silent_drop`), leaving endpoint 0 to `NAK` until the
    target host times out (`length == 0`).
- **Threading:** May be called from the Device domain **or** directly inside
  `transport.event_sink`.

---

### 5. Spec factories and inspection

```c
mimhid_result mimhid_spec_create_keyboard(const mimhid_keyboard_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_mouse(const mimhid_mouse_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_toggle(const mimhid_toggle_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_gamepad(const mimhid_gamepad_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_touchscreen(const mimhid_touchscreen_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_touchpad(const mimhid_touchpad_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_pen(const mimhid_pen_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_digitizer(const mimhid_digitizer_options* options, mimhid_spec** out_spec);
mimhid_result mimhid_spec_create_raw(const mimhid_raw_options* options, mimhid_spec** out_spec);

void mimhid_spec_retain(mimhid_spec* spec);
void mimhid_spec_release(mimhid_spec* spec);
mimhid_result mimhid_spec_descriptor(const mimhid_spec* spec, const uint8_t** bytes, size_t* length);
mimhid_result mimhid_spec_manifest(const mimhid_spec* spec, mimhid_capability_manifest* manifest);
```

- **Purpose:** Build, reference-count, and inspect immutable `mimhid_spec`
  objects. All validation rules for each profile factory are documented in
  [PROFILES.md](PROFILES.md).
- **Ownership:** `mimhid_spec_create_*` returns a `mimhid_spec*` with reference
  count `1`. `mimhid_node_open` retains its own reference until
  `mimhid_node_close`, so the caller may call `mimhid_spec_release(spec)`
  immediately after `mimhid_node_open`.

---

### 6. Node lifecycle, configuration, and submission

```c
mimhid_result mimhid_node_open(mimhid_device* device, mimhid_spec* spec,
                               const mimhid_node_config* config,
                               const mimhid_commit_timeouts* timeouts,
                               mimhid_node** out_node);
mimhid_result mimhid_node_open_raw(mimhid_device* device,
                                   const uint8_t* descriptor, size_t descriptor_length,
                                   const mimhid_node_config* config,
                                   const mimhid_commit_timeouts* timeouts,
                                   mimhid_node** out_node);
mimhid_result mimhid_node_close(mimhid_node* node,
                                const mimhid_commit_timeouts* timeouts,
                                uint32_t drain_timeout_ms);
uint16_t mimhid_node_id(const mimhid_node* node);
mimhid_result mimhid_node_set_feature_response(mimhid_node* node, uint8_t report_id,
                                               const uint8_t* data, size_t length,
                                               uint32_t control_timeout_ms);
mimhid_result mimhid_node_set_queue_depth(mimhid_node* node, uint32_t depth,
                                          uint32_t control_timeout_ms);
mimhid_result mimhid_node_manifest(const mimhid_node* node,
                                   mimhid_capability_manifest* manifest);
mimhid_result mimhid_node_submit(mimhid_node* node, uint32_t send_timeout_ms);
mimhid_result mimhid_node_submit_blocking(mimhid_node* node, uint32_t timeout_ms);
```

- **`mimhid_node_id`:** returns the 1-based identifier (`1..16`) that matches
  `event->node_id` in `mimhid_event` and `mimhid_error_detail.node_id`, or `0`
  (recording `MIMHID_ERR_PARAM`) for a null or closed Node.
- **`mimhid_node_set_feature_response`:** sets the payload (`0..max_feature_bytes`,
  without the Report ID byte) returned when the target issues
  `GET_REPORT(Feature, report_id)` on `node`.
- **`mimhid_node_set_queue_depth`:** sets how many reports (`1..15` when
  `max_report_bytes <= 64`, else `1..7`) may wait in the board's SPSC queue for
  `node`'s interface before new reports are dropped.
- **`mimhid_node_submit` vs `mimhid_node_submit_blocking`:** `mimhid_node_submit`
  writes one report (primary Input first, or a dirty battery report when no Input
  report is dirty); `mimhid_node_submit_blocking` loops `mimhid_node_submit`
  until all pending multi-packet touch frames, clamped mouse deltas, and battery
  updates have been written or `timeout_ms` expires.

---

### 7. Profile input mutations and raw/report send

```c
/* Keyboard */
mimhid_result mimhid_keyboard_key(mimhid_node* node, uint16_t usage, uint32_t down);

/* Mouse */
mimhid_result mimhid_mouse_move(mimhid_node* node, int32_t dx, int32_t dy);
mimhid_result mimhid_mouse_scroll(mimhid_node* node, int32_t wheel, int32_t pan);
mimhid_result mimhid_mouse_button(mimhid_node* node, uint32_t button, uint32_t down);

/* Toggle */
mimhid_result mimhid_toggle_usage(mimhid_node* node, uint16_t usage, uint32_t down);

/* Gamepad */
mimhid_result mimhid_gamepad_button(mimhid_node* node, uint32_t button, uint32_t down);
mimhid_result mimhid_gamepad_axis(mimhid_node* node, size_t axis_index, int32_t value);
mimhid_result mimhid_gamepad_dpad(mimhid_node* node, uint32_t up, uint32_t down, uint32_t right, uint32_t left);

/* Touchscreen & Touchpad */
mimhid_result mimhid_touchscreen_contact(mimhid_node* node, uint32_t contact_id, uint32_t down,
                                         int32_t x, int32_t y, const mimhid_touchscreen_extra* extra);
mimhid_result mimhid_touchpad_contact(mimhid_node* node, uint32_t contact_id, uint32_t down,
                                      int32_t x, int32_t y, const mimhid_touchpad_extra* extra);
mimhid_result mimhid_touchpad_button(mimhid_node* node, uint32_t button, uint32_t down);

/* Pen & Digitizer */
mimhid_result mimhid_pen_update(mimhid_node* node, const mimhid_pen_sample* sample);
mimhid_result mimhid_digitizer_update(mimhid_node* node, const mimhid_digitizer_sample* sample);

/* Battery */
mimhid_result mimhid_battery_update(mimhid_node* node, int32_t strength);

/* Raw and direct report send */
mimhid_result mimhid_raw_submit(mimhid_node* node, const uint8_t* report, size_t length, uint32_t send_timeout_ms);
mimhid_result mimhid_report_send(mimhid_node* node, const uint8_t* report_bytes, size_t report_length, uint32_t send_timeout_ms);
```

Full per-profile contracts for these calls are in [PROFILES.md](PROFILES.md).

---

## C++20 header (`include/mimhid.hpp`)

[include/mimhid.hpp](../include/mimhid.hpp) wraps every function of `mimhid.h`
under `namespace mimhid` with the `mimhid_` prefix removed, adds zero-initializing
option factories (`mimhid::device_config()`, `mimhid::commit_timeouts()`,
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

Every result code shared with `libaoahid` uses the same integer value (numbers
`5`, `9`, and `15` are reserved for `libaoahid`-only codes):

| Code | Value | Meaning |
|---|---:|---|
| `MIMHID_OK` | `0` | Operation succeeded |
| `MIMHID_ERR_PARAM` | `1` | Null pointer, wrong `struct_size`, non-zero reserved field, invalid flag (`!= 0 && != 1`), wrong profile kind, or value outside declared range |
| `MIMHID_ERR_UNSET_FIELD` | `2` | Required option, configuration field, or timeout argument was left zero |
| `MIMHID_ERR_UNSUPPORTED` | `3` | Feature not enabled in the `Spec`, unsupported HUT semantic, or baud rate not supported by the board/link |
| `MIMHID_ERR_PROTOCOL` | `4` | Board firmware major or minor version does not match the library (`0.2`) |
| `MIMHID_ERR_ACCESS` | `6` | Insufficient permission to open the serial port (`EACCES` / `EPERM` / `ERROR_ACCESS_DENIED`) |
| `MIMHID_ERR_BUSY` | `7` | Serial port locked by another process, batch open/closed in the wrong state, device not suspended on `remote_wakeup`, report already in flight, or opposite edge not yet submitted |
| `MIMHID_ERR_NO_DEVICE` | `8` | Serial port disappeared or failed; stays latched on the `Device` until `mimhid_device_close` |
| `MIMHID_ERR_TIMEOUT` | `10` | Serial handshake, command `ACK`, serial write, or target USB enumeration (`MOUNTED`) timed out |
| `MIMHID_ERR_SHORT_TRANSFER` | `11` | Serial write stopped part-way through a frame |
| `MIMHID_ERR_DESCRIPTOR_REJECTED` | `12` | Board refused the staged USB or HID configuration (`MIMHID_NAK_DESCRIPTOR` or `MIMHID_NAK_PARAM`) |
| `MIMHID_ERR_IO` | `13` | Operating-system serial I/O error or unexpected `NAK` frame |
| `MIMHID_ERR_OVERFLOW` | `14` | Value, count, report size, or descriptor size exceeds a protocol, USB, HID, `linux_hid`, or board limit |
| `MIMHID_ERR_INTERNAL` | `16` | Out-of-memory (`std::bad_alloc`) or internal invariant failure contained at the C ABI boundary |

---

## Differences from `libaoahid`

| Area | `libaoahid` (Android Open Accessory 2.0) | `libmimhid` (MCU USB-HID bridge) |
|---|---|---|
| **Target & transport** | Android phone/tablet directly over USB (`libusb`, AOA 2.0 control requests `54..57` on endpoint 0) | Any USB host (PC, console, phone) through an MCU board over a host serial port (`termios`/`Win32`) |
| **Top-level handles** | `aoahid_context*`, `aoahid_discovery*`, `aoahid_device*`, `aoahid_spec*`, `aoahid_node*` | No `Context` handle: `mimhid_discovery*`, `mimhid_device*`, `mimhid_spec*`, `mimhid_node*` |
| **Initializers (`*_init`)** | `aoahid_*_init` fills in working defaults for a current Android phone | `mimhid_*_init` only zeroes the struct and sets `struct_size`; every required field is set by the caller |
| **USB descriptors** | Fixed by the Android phone (`ACCESSORY_START` strings only) | Caller supplies full USB Device, Configuration, Interface, and Endpoint descriptors (`mimhid_device_config`, `mimhid_node_config`) |
| **Opening & closing a Node** | Sends `REGISTER_HID` + `SET_HID_REPORT_DESCRIPTOR` with a single `control_timeout_ms` | Takes `const mimhid_commit_timeouts*` (`control_timeout_ms` + `reconnect_timeout_ms`); can be batched with `mimhid_device_batch_begin` / `end` |
| **Submit calls** | `aoahid_node_submit_blocking` and `aoahid_node_submit` | `mimhid_node_submit` (one report) and `mimhid_node_submit_blocking` (drains multi-packet frames and remainders) |
| **Two-way HID & bus control** | Input-only (AOA 2.0 defines no Output or Feature requests) | Full two-way HID via `transport.event_sink` (`OUTPUT_REPORT`, `FEATURE_REPORT`, `SET_PROTOCOL`, `SET_IDLE`, `REPORT_LOST`, bus state), `mimhid_node_set_feature_response`, `mimhid_device_disconnect`/`connect`/`remote_wakeup`, and endpoint 0 control transfer routing (`mimhid_device_set_control_routing`, `mimhid_device_control_reply`) |
| **Result codes** | Shares the common integer values plus `5` (`AOAHID_ERR_NOT_AOA`), `9` (`AOAHID_ERR_STALL`), and `15` (`AOAHID_ERR_CLOSE_PENDING`) | Shares the common integer values plus `4` (`MIMHID_ERR_PROTOCOL`) |
