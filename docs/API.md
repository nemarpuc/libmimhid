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
  node_open / node_open_raw adds the node on an explicit iface.number; the board re-enumerates (attaches on the first node)
  device_connect / disconnect command physical connection (D+=3.3V) or stealth disconnection (D+=0V)
  device_wait_ready         until the target mounts the USB device
  <mutate>  node_submit     repeat
  node_close                neutral report first, then the board re-enumerates without the node
device_close                detaches the USB device (unless keep_attached)
spec_release
```

`mimhid_node_open`, `mimhid_node_open_raw`, and `mimhid_node_close` send the configuration to the board and re-enumerate the USB device at once; the target sees an unplug and replug. To change several nodes with one re-enumeration, wrap the calls in `mimhid_device_batch_begin` / `mimhid_device_batch_end`. Nodes opened inside a batch return `MIMHID_ERR_BUSY` from input calls until `mimhid_device_batch_end` succeeds. `mimhid_device_commit(device, reconnect_delay_ms, timeouts)` is not needed after either; it re-sends the configuration and re-enumerates once more, and is the call that holds D+=0V for `reconnect_delay_ms` before reconnecting.

Physical connection state can also be controlled manually using `mimhid_device_connect()` (assert D+=3.3V) and `mimhid_device_disconnect()` (stealth/silent D+=0V).

## Timeouts

No wait has a default and the Device keeps none: every call that can wait takes its time in milliseconds as an argument, and zero fails with `MIMHID_ERR_UNSET_FIELD` before anything is sent. There are three kinds:

| Wait | Meaning | Taken by |
|---|---|---|
| control | each configuration frame waits this long for the board's ACK or NAK (a frame is sent up to three times) | `mimhid_device_disconnect`, `mimhid_device_remote_wakeup`, `mimhid_device_set_control_routing`, `mimhid_node_set_feature_response`, and `control_timeout_ms` of `mimhid_commit_timeouts` and `mimhid_device_close` |
| reconnect | the board's re-enumeration after a commit | `reconnect_timeout_ms` of `mimhid_commit_timeouts` |
| send | a serial write may make no progress this long before `MIMHID_ERR_TIMEOUT`, with nothing sent | `mimhid_node_submit`, `mimhid_raw_submit`, `mimhid_report_send`, `mimhid_device_control_reply` |

`mimhid_commit_timeouts` (`struct_size`, `control_timeout_ms`, `reconnect_timeout_ms`; `mimhid_commit_timeouts_init`) is passed to every call that re-enumerates the board: `mimhid_node_open`, `mimhid_node_open_raw`, `mimhid_node_close`, `mimhid_device_batch_end`, `mimhid_device_commit`, `mimhid_device_connect` and `mimhid_device_set_usb_identity`. It is required also where the call only stages (inside a batch). The drain of the neutral reports on close is `drain_timeout_ms` of `mimhid_node_close` and `mimhid_device_close`. A close with a zero timeout consumes nothing.

Already per call: `mimhid_discover` and `mimhid_device_wait_ready` (`timeout_ms`), `mimhid_node_submit_blocking` (`timeout_ms`, which also bounds each write by what is left of it), and each control rule (`timeout_ms`, how long the board holds a request; at most `MIMHID_CONTROL_MAX_TIMEOUT_MS`). The wire protocol's fixed times (`MIMHID_BAUD_CONFIRM_MS`, the board's 10 ms receive resync) are the protocol's and no call changes them.

## Input

Input calls change the state of a node and send nothing. `mimhid_node_submit(node, send_timeout_ms)` encodes the state as a report and writes it to the serial port without waiting for the target. `mimhid_node_submit_blocking` also finishes the further reports of a touch frame. Mutating calls return `MIMHID_ERR_BUSY` while a report is in flight.

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

`mimhid_device_config` (`mimhid_device_config_init` sets `struct_size`) holds `transport`, `usb_device` and `usb_config`. Every value is used as given and none has a default. A required field that is zero fails with `MIMHID_ERR_UNSET_FIELD`.

- `usb_device`, identity: `vendor_id` (required), `product_id`, `bcd_device`, `language_id` (required), `manufacturer`, `product`, `serial` (printable ASCII, at most 126 characters; NULL leaves the string out), `serial_from_chip_id` (use the MCU's unique id when `serial` is NULL).
- `usb_device`, descriptor: `bcd_usb` (required), `ep0_max_packet_size` (required: 8, 16, 32 or 64), `device_class`, `device_subclass`, `device_protocol`.
- `usb_config`: `configuration_value` (required, nonzero), `bm_attributes` (bit 7 set), `bmax_power` (at most 250, in 2 mA units), `string`.
- `transport`, link: `baud_rate` (required, one of `MIMHID_BAUD_RATES`; `mimhid_discover` reports the fastest one the link carries), `handshake_timeout_ms` (required: the wait for each answer while the board is searched for on the port), `maximum_report_bytes` (required: the largest report the Device sends, at most the board's `max_report_bytes` and `MIMHID_HOST_MAX_REPORT_BYTES`) and `log_level` (required: a `MIMHID_LOG_*` value; any level but `MIMHID_LOG_DISABLED` needs `log_sink`). The other waits are not here: see [Timeouts](#timeouts).
- `transport`, behavior: `validate_reports` (decode each generated report again), `keep_attached` (leave the USB device on the bus after `device_close`).
- Target parser limits `linux_*_policy`: nonzero values chosen by the caller for the HID parser of the target. A descriptor that exceeds them is rejected when the Spec is opened on the Device.
- Callbacks: `event_sink` (everything the target sends: Output and Feature reports, `SET_PROTOCOL`, `SET_IDLE`, mount, suspend, resume), `log_sink`.

`mimhid_node_config` configures the USB interface and endpoints that carry a node:
- `iface.number`: explicit USB interface number (`0`, `1`, `2`...; required, no automatic allocation); `ep_in` and `ep_out` carry the endpoint addresses, sizes and polling intervals.
- `ep_in.poll_interval_ms`: polling interval in ms (required, 1..255).
- `ep_in.max_packet_size`: IN endpoint packet size (required, 1 to the board's `max_endpoint_bytes`).
- `ep_out.address`: 0 disables the OUT endpoint; otherwise `ep_out.max_packet_size` is its packet size (1 to the board's `max_endpoint_bytes`).
- `iface.subclass`, `iface.protocol`: boot interface (`protocol` 0: none, 1: keyboard, 2: mouse).
- `iface.bcd_hid`: HID version (required, e.g. `0x0111`).
- `iface.country_code`: country code (0: not supported).

Every top-level structure (`mimhid_device_config`, `mimhid_node_config`, `mimhid_commit_timeouts`, `mimhid_board_limits`) starts with `struct_size` and `reserved`; `reserved` must be zero.
- `interface_name`: interface string descriptor (`NULL` for none).

A USB device has one VID/PID, so those belong to the Device, not to a profile.

## Events

`event_sink` receives, on the library's reader thread: `MOUNTED`, `UNMOUNTED`, `SUSPEND`, `RESUME`, `OUTPUT_REPORT` and `FEATURE_REPORT` (the target wrote a report; Report ID separate from the data), `SET_PROTOCOL`, and `SET_IDLE`. The callback must be short, must not call libmimhid, and must copy data it keeps.

`CONTROL_SETUP` and `CONTROL_DATA` events (with `control_token` and `control_flags`) come from [control transfers](PROTOCOL.md#control-transfers), which are off until `mimhid_device_set_control_routing` turns them on. `mimhid_device_control_reply` may be called from the event sink (it only writes a frame), which is the way to answer inside a short rule time.

`mimhid_node_set_feature_response` sets the answer to a Feature read of a Report ID. A touch Spec with `enable_contact_count_maximum_feature_declaration` is answered automatically. `mimhid_device_remote_wakeup(device, control_timeout_ms)` wakes a suspended target when `remote_wakeup` was enabled.

## Control transfers of the target

Two calls, both on the Device and both off until used (see [PROTOCOL.md](PROTOCOL.md#control-transfers) for the wire side):

| Call | Does |
|---|---|
| `mimhid_device_set_control_routing(device, report_all, rules, count, control_timeout_ms)` | Replaces the set: `report_all` 1 reports every request on endpoint 0 as `MIMHID_EVENT_CONTROL_SETUP`; each `mimhid_control_rule` (up to `MIMHID_CONTROL_MAX_RULES`, the board's `max_control_rules`) holds the requests it matches. `timeout_ms` and `when_late` have no default and zero is refused. |
| `mimhid_device_control_reply(device, token, action, data, length, send_timeout_ms)` | Answers a held request: `ANSWER` (data for a device-to-host request, sent whole whatever `wLength` says, also when it is 0; an acknowledgement otherwise), `STALL`, or `LEAVE` (the board's own handling). Writes one frame and returns; callable from the event sink. |

A held request that nobody answers within its rule's time gets the rule's `when_late` action. `MIMHID_EVENT_CONTROL_DATA` carries the data stage of a host-to-device request; the second reply is the verdict. Standard OUT SET_ADDRESS is reported and never held; SET_CONFIGURATION can be held and answered by rules; a host-to-device data stage longer than `max_control_data_bytes` is reported with `MIMHID_CONTROL_FLAG_TOO_LONG` and never held. `mimhid_device_close` removes the rules. A complete program: [`examples/c/control_transfers.c`](../examples/c/control_transfers.c).

## Errors

Calls return `mimhid_result`. `mimhid_last_error()` returns a thread-local `mimhid_error_detail` (field, reason, node id, Report ID, offsets) valid until the next call on the thread. A serial failure latches `MIMHID_ERR_NO_DEVICE`: afterwards every Device and Node call fails with it, and `mimhid_device_latched_error` reports it. Recovery is to close the Device and open it again.

Result codes with the same number mean the same in `libaoahid` and `libmimhid`: `OK` 0, `PARAM` 1, `UNSET_FIELD` 2, `UNSUPPORTED` 3, `ACCESS` 6, `BUSY` 7, `NO_DEVICE` 8, `TIMEOUT` 10, `SHORT_TRANSFER` 11, `DESCRIPTOR_REJECTED` 12, `IO` 13, `OVERFLOW` 14, `INTERNAL` 16. Number 4 is the transport's own: `MIMHID_ERR_PROTOCOL` here, `AOAHID_ERR_NOT_AOA` there; `AOAHID_ERR_STALL` (9) and `AOAHID_CLOSE_PENDING` (15) exist only in `libaoahid`, whose USB transfers can outlive a call.

## Differences from libaoahid

- Waits follow the same rule as `libaoahid` 2.0 and later (see Timeouts above): every call that can wait takes its timeout as an argument, with the same argument names. The only extra is the wait for the board's re-enumeration, in `mimhid_commit_timeouts`. `libaoahid`'s `aoahid_node_submit` queues a USB transfer and returns, so its `send_timeout_ms` is that transfer's deadline; here it bounds a serial write that makes no progress.
- No context and no event mode: a Device owns its port and one reader thread.
- `mimhid_device_open` takes a direct serial port path string (`const char* port_path`, e.g. `"/dev/ttyACM0"` or `"COM3"`) rather than a libusb device.
- `mimhid_node_open` and `mimhid_node_open_raw` take an explicit `iface.number` and re-enumerate the board's USB device; `mimhid_device_batch_begin` / `mimhid_device_batch_end` combine several changes into one re-enumeration, and `mimhid_device_commit` controls the reconnect delay.
- Direct physical USB bus control with `mimhid_device_connect()` and `mimhid_device_disconnect()`.
- Extra Device and Node settings for the USB device, listed above.
- No accessory or channel calls.
- Per-profile VID/PID does not exist (one USB device has one).
