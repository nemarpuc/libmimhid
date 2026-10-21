# API

The authoritative reference is the comments in [`include/mimhid.h`](../include/mimhid.h): every call states ownership, blocking, synchronization, and every result code. `mimhid.hpp` forwards each call into `namespace mimhid` without the prefix. This page explains the model.

The structure follows `libaoahid`; the differences are listed at the end.

Two layers are visible in the API. The **low level** is `mimhid_discover`, `mimhid_device_open`, `mimhid_node_open_raw`, `mimhid_raw_submit`, `mimhid_report_send`, and `event_sink`; it is enough to drive a hand-built descriptor and needs none of the profile factories or mutation helpers. The **profile layer** (`mimhid_spec_create_*` and the `mimhid_keyboard_key` family) generates descriptors and maintains report state on top of a Node. Opening a non-raw Node already runs through the low-level `mimhid_node_open`; the profile calls only mutate state, they send nothing until `mimhid_node_submit`.

## Objects

| Object | Meaning | Created by | Destroyed by |
|---|---|---|---|
| `mimhid_discovery` | snapshot of the boards found on the serial ports | `mimhid_discover` | `mimhid_discovery_destroy` |
| `mimhid_device` | one open serial link to a board, and the USB device the board presents | `mimhid_device_open` | `mimhid_device_close` |
| `mimhid_spec` | immutable description of one HID device: report descriptor, report layout, manifest | `mimhid_spec_create_*` | `mimhid_spec_release` (reference counted) |
| `mimhid_node` | a Spec or raw descriptor running on a Device, with mutable input state | `mimhid_node_open`, `mimhid_node_open_raw` | `mimhid_node_close` |

A Device owns its Nodes: closing the Device closes every Node first. A Spec can be shared by any number of Nodes and Devices.

## Lifecycle

```
discover (optional)
device_open                 opens serial port (e.g. "/dev/ttyACM0") and starts reader thread; USB stays disconnected
  spec_create_* / raw descriptor
  node_open / node_open_raw adds the node on an explicit interface_number; the board re-enumerates (attaches on the first node)
  device_connect / disconnect command physical connection (D+=3.3V) or stealth disconnection (D+=0V)
  device_wait_ready         until the target mounts the USB device
  <mutate>  node_submit     repeat
  node_close                neutral report first, then the board re-enumerates without the node
device_close                detaches the USB device (unless keep_attached)
spec_release
```

`mimhid_node_open`, `mimhid_node_open_raw`, and `mimhid_node_close` send the configuration to the board and re-enumerate the USB device at once; the target sees an unplug and replug. To change several nodes with one re-enumeration, wrap the calls in `mimhid_device_batch_begin` / `mimhid_device_batch_end`. Nodes opened inside a batch return `MIMHID_ERR_BUSY` from input calls until `mimhid_device_batch_end` succeeds. `mimhid_device_commit(device, reconnect_delay_ms)` is not needed after either; it re-sends the configuration and re-enumerates once more, and is the call that holds D+=0V for `reconnect_delay_ms` before reconnecting.

Physical connection state can also be controlled manually using `mimhid_device_connect()` (assert D+=3.3V) and `mimhid_device_disconnect()` (stealth/silent D+=0V).

## Input

Input calls change the state of a node and send nothing. `mimhid_node_submit` encodes the state as a report and writes it to the serial port without waiting for the target. `mimhid_node_submit_blocking` also finishes the further reports of a touch frame. Mutating calls return `MIMHID_ERR_BUSY` while a report is in flight.

| Profile | Calls |
|---|---|
| Keyboard | `mimhid_keyboard_key` |
| Mouse | `mimhid_mouse_move`, `mimhid_mouse_scroll`, `mimhid_mouse_button` |
| Toggle | `mimhid_toggle_usage` |
| Gamepad | `mimhid_gamepad_button`, `mimhid_gamepad_axis`, `mimhid_gamepad_dpad` |
| Touchscreen | `mimhid_touchscreen_contact` |
| Touchpad | `mimhid_touchpad_contact`, `mimhid_touchpad_button` |
| Pen, Digitizer | `mimhid_pen_update`, `mimhid_digitizer_update` |
| Battery (profiles with `enable_battery`) | `mimhid_battery_update` |
| Raw | `mimhid_raw_submit`, `mimhid_report_send` (complete report, Report ID included) |

Every call is serialized by the caller per Device. Calls on different Devices are independent.

All 23 profile-layer entries (9 Spec factories and 14 mutation calls) are retained: each one matches a distinct HID report shape or semantic (`keyboard_key` for NKRO keyboard arrays, `mouse_move/scroll/button`, `touchscreen_contact` with explicit slots, `pen_update` and `digitizer_update` as separately shaped update families, `battery_update`, and so on). Collapsing them into one generic call would remove explicit per-field control the library intentionally leaves to the caller. `mimhid_raw_submit` plus `mimhid_report_send` provide the escape hatch for any profile not covered by a factory.

## Settings of the USB device

`mimhid_device_options`: every value is used as given and none has a default. A required field that is zero fails with `MIMHID_ERR_UNSET_FIELD`.

- Identity: `vendor_id` (required), `product_id`, `bcd_device`, `language_id` (required), `manufacturer`, `product`, `serial`, `configuration` (strings of at most 31 printable ASCII characters; NULL or empty leaves the string out), `serial_from_chip_id` (use the MCU's unique id when `serial` is NULL).
- Descriptor: `device_class`, `device_subclass`, `device_protocol`.
- Power: `self_powered`, `remote_wakeup`, `max_power_ma` (0 to 500).
- Link, all required except `maximum_report_bytes`: `baud_rate` (one of `MIMHID_BAUD_RATES`; `mimhid_discover` reports the fastest one the link carries), `control_timeout_ms`, `send_timeout_ms`, `reconnect_timeout_ms`, `close_drain_timeout_ms`, and `maximum_report_bytes` (0: the limit the board reports).
- Behavior: `validate_reports` (decode each generated report again), `keep_attached` (leave the USB device on the bus after `device_close`).
- Target parser limits `linux_*_policy`: nonzero values chosen by the caller for the HID parser of the target. A descriptor that exceeds them is rejected when the Spec is opened on the Device.
- Callbacks: `event_sink` (everything the target sends: Output and Feature reports, `SET_PROTOCOL`, `SET_IDLE`, mount, suspend, resume), `log_sink`.

`mimhid_node_options` configures the USB interface that carries a node:
- `interface_number`: explicit USB interface number (`0`, `1`, `2`...; required, no automatic allocation).
- `poll_interval_ms`: polling interval in ms (1..255 ms, typical `1`).
- `ep_in_size`: IN endpoint packet size (`1` to `64`; typical `64`).
- `ep_out_size`: OUT endpoint packet size (`0`: OUT disabled, `1` to `64`: OUT enabled).
- `boot_protocol`: `0`: None, `1`: Keyboard, `2`: Mouse.
- `bcd_hid`: HID version (default `0x0111`).
- `country_code`: country code (default `0`).
- `interface_name`: interface string descriptor (`NULL` for none).

A USB device has one VID/PID, so those belong to the Device, not to a profile.

## Events

`event_sink` receives, on the library's reader thread: `MOUNTED`, `UNMOUNTED`, `SUSPEND`, `RESUME`, `OUTPUT_REPORT` and `FEATURE_REPORT` (the target wrote a report; Report ID separate from the data), `SET_PROTOCOL`, and `SET_IDLE`. The callback must be short, must not call libmimhid, and must copy data it keeps.

`CONTROL_SETUP` and `CONTROL_DATA` events (with `control_token` and `control_flags`) come from [control transfers](PROTOCOL.md#control-transfers), which are off until `mimhid_device_set_control_routing` turns them on. `mimhid_device_control_reply` may be called from the event sink (it only writes a frame), which is the way to answer inside a short rule time.

`mimhid_node_set_feature_response` sets the answer to a Feature read of a Report ID. A touch Spec with `enable_contact_count_maximum_feature_declaration` is answered automatically. `mimhid_device_remote_wakeup` wakes a suspended target when `remote_wakeup` was enabled.

## Control transfers of the target

Two calls, both on the Device and both off until used (see [PROTOCOL.md](PROTOCOL.md#control-transfers) for the wire side):

| Call | Does |
|---|---|
| `mimhid_device_set_control_routing(device, report_all, rules, count)` | Replaces the set: `report_all` 1 reports every request on endpoint 0 as `MIMHID_EVENT_CONTROL_SETUP`; each `mimhid_control_rule` (up to `MIMHID_CONTROL_MAX_RULES`, the board's `max_control_rules`) holds the requests it matches. `timeout_ms` and `when_late` have no default and zero is refused. |
| `mimhid_device_control_reply(device, token, action, data, length)` | Answers a held request: `ANSWER` (data for a device-to-host request, an acknowledgement otherwise), `STALL`, or `LEAVE` (the board's own handling). Writes one frame and returns; callable from the event sink. |

A held request that nobody answers within its rule's time gets the rule's `when_late` action. `MIMHID_EVENT_CONTROL_DATA` carries the data stage of a host-to-device request; the second reply is the verdict. Standard OUT SET_ADDRESS and SET_CONFIGURATION are reported and never held; a host-to-device data stage longer than `max_control_data_bytes` is reported with `MIMHID_CONTROL_FLAG_TOO_LONG` and never held. `mimhid_device_close` removes the rules. A complete program: [`examples/c/control_transfers.c`](../examples/c/control_transfers.c).

## Errors

Calls return `mimhid_result`. `mimhid_last_error()` returns a thread-local `mimhid_error_detail` (field, reason, node id, Report ID, offsets) valid until the next call on the thread. A serial failure latches `MIMHID_ERR_NO_DEVICE`: afterwards every Device and Node call fails with it, and `mimhid_device_latched_error` reports it. Recovery is to close the Device and open it again.

## Differences from libaoahid

- No context and no event mode: a Device owns its port and one reader thread.
- `mimhid_device_open` takes a direct serial port path string (`const char* port_path`, e.g. `"/dev/ttyACM0"` or `"COM3"`) rather than a libusb device.
- `mimhid_node_open` and `mimhid_node_open_raw` take an explicit `interface_number` and re-enumerate the board's USB device; `mimhid_device_batch_begin` / `mimhid_device_batch_end` combine several changes into one re-enumeration, and `mimhid_device_commit` controls the reconnect delay.
- Direct physical USB bus control with `mimhid_device_connect()` and `mimhid_device_disconnect()`.
- Extra Device and Node settings for the USB device, listed above.
- No accessory or channel calls.
- Per-profile VID/PID does not exist (one USB device has one).
