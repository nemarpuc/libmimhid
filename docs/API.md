# API

[`include/mimhid.h`](../include/mimhid.h) is the stable C ABI and its reference: every function has an Ownership / Blocking / Synchronization / Returns contract there. This page explains how the pieces fit together. [`include/mimhid.hpp`](../include/mimhid.hpp) forwards every function into `namespace mimhid` without the prefix and adds functions that return initialized structures, and owning handles.

The structure follows `libaoahid`; the differences are listed [at the end](#differences-from-libaoahid).

Two layers are visible. The **low level** is `mimhid_discover`, `mimhid_device_open`, `mimhid_node_open_raw`, `mimhid_raw_submit`, `mimhid_report_send` and `event_sink`; it drives a hand-built descriptor and needs no profile. The **profile layer** (`mimhid_spec_create_*` and the `mimhid_keyboard_key` family) builds descriptors and keeps report state on top of a Node. The profile calls only change state; they send nothing until `mimhid_node_submit`.

## Objects

| Object | Meaning | Created by | Destroyed by |
|---|---|---|---|
| `mimhid_discovery` | a snapshot of the boards found on the serial ports | `mimhid_discover` | `mimhid_discovery_destroy` |
| `mimhid_device` | one open serial link to a board, and the USB device the board presents | `mimhid_device_open` | `mimhid_device_close` |
| `mimhid_spec` | an immutable HID report descriptor, report layout and manifest | `mimhid_spec_create_*` | `mimhid_spec_release` (reference counted) |
| `mimhid_node` | a Spec or raw descriptor running on a Device, with the state its input calls change | `mimhid_node_open`, `mimhid_node_open_raw` | `mimhid_node_close` |

A Device owns its Nodes: closing the Device closes every Node first. A Node keeps its own reference to its Spec, so the Spec may be released as soon as the Node is open. A Spec can be shared by any number of Nodes and Devices.

## Rules every function follows

- **Results.** Every call records the details of its result for the calling thread; `mimhid_last_error()` reads them (field, reason, node id, Report ID, offsets, the frame type and the board's or the operating system's code) until the next call on that thread. A serial failure latches `MIMHID_ERR_NO_DEVICE`: afterwards every call on the Device and its Nodes fails with it, and `mimhid_device_latched_error` reports it. Recovery is to close the Device and open it again.
- **Outputs.** A function that creates an object returns it through an `out_*` pointer, which is set to null first and stays null when the call fails.
- **Closing.** `mimhid_device_close`, `mimhid_node_close`, `mimhid_discovery_destroy` and `mimhid_spec_release` follow one rule. `MIMHID_ERR_PARAM` or `MIMHID_ERR_UNSET_FIELD`: nothing was done, and the handle is still the caller's. Any other result: the object is gone and its handle must not be used again; an error says what failed on the way out (a Node whose neutral report could not be sent, a board whose link was already lost). Writes to the serial port finish within the call, so nothing is ever left pending.
- **Threads.** A Device and its Nodes form one domain: the caller makes sure no two calls in a domain run at the same time. Specs and Discovery snapshots are outside any domain. Calls on different Devices are independent.
- **Timeouts.** See [Timeouts](#timeouts).
- **Structures.** See [Structures and initial values](#structures-and-initial-values).

## Structures and initial values

Every enumeration is an `int32_t` typedef with integral constants, so structure layout does not change when a compiler uses short enums; `tests/abi/test_c_abi.c` asserts the layouts.

- Every top-level options structure starts with `struct_size` (its `sizeof`) and `reserved` (zero). A wrong size fails with `MIMHID_ERR_PARAM`, so an old binary and a new header never read a structure differently.
- A required field left zero is refused with `MIMHID_ERR_UNSET_FIELD`; the library never puts a value of its own in its place. Every enumeration starts at 1, so a zeroed enumeration field is refused rather than read as a choice. Where zero is a real value (a logical minimum, interface number 0, country code 0) the field has no "unset" meaning, and what is optional has its own `enable_*` flag or count.
- `enable_*` and the other flags are exactly 0 or 1.

Each structure has an initializer, `mimhid_<structure>_init`, that zeroes it and sets `struct_size`, and **nothing else**: it fills in no value. Every field the library requires is yours to set, and one left at zero where zero is not valid is refused with `MIMHID_ERR_UNSET_FIELD` (or `MIMHID_ERR_PARAM` where the profile's validation names it); a value you set is used as given. The initializers are `static inline` functions in the header, so they are not part of the binary ABI. `mimhid.hpp` has the same as functions that return the structure (`mimhid::device_config()`, `mimhid::node_config()`, `mimhid::keyboard_options()`, ...).

What a program must set, by structure:

| Structure | Required (zero is refused) | Optional |
|---|---|---|
| `mimhid_device_config` | `transport.baud_rate` (`MIMHID_BOOT_BAUD` is 115,200, `MIMHID_BAUD_FASTEST` asks for the fastest rate the link carries; `mimhid_discover` reports the fastest one a port carries), `transport.handshake_timeout_ms`, `transport.maximum_report_bytes`, `transport.log_level` (an enumeration: `MIMHID_LOG_DISABLED` is 1), the six `transport.linux_hid` limits ([LIMITS.md](LIMITS.md#linux-hid-parser)), `usb_device.vendor_id`, `usb_device.bcd_usb`, `usb_device.ep0_max_packet_size`, `usb_device.language_id`, `usb_config.configuration_value`, `usb_config.bm_attributes` (bit 7 set) | `usb_device.product_id` (zero is a valid ID), `bcd_device`, the device class triple, the strings (null leaves one out), `serial_from_chip_id`, `usb_config.bmax_power` |
| `mimhid_commit_timeouts` | `control_timeout_ms`, `reconnect_timeout_ms` | |
| `mimhid_node_config` | `iface.class_code` (`0x03`), `iface.bcd_hid`, `ep_in.address`, `ep_in.poll_interval_ms`, `ep_in.max_packet_size`; with an OUT endpoint also its address, interval and size | `iface.number` (a label; Nodes with the same one share an interface: give each Node its own unless you mean that), `subclass`, `protocol`, `country_code`, `name` |
| `mimhid_keyboard_options` | `usage_minimum`, `usage_maximum` | |
| `mimhid_mouse_options` | `button_count`, `x`, `y` | wheel and pan (`enable_*` with their fields) |
| `mimhid_toggle_options` | `application_page`, `application_usage`, `field_page`, the Usages and their three parallel arrays | |
| `mimhid_gamepad_options` | the axes (`axes`, `axis_count`), `button_count` and `button_usage_minimum`, the D-pad (`dpad_representation` and, for a Hat, its range and width) | |
| `mimhid_touchscreen_options`, `mimhid_touchpad_options` | `maximum_contacts`, `contacts_per_report`, `contact_id`, `x`, `y`, `contact_count` | optional fields (`enable_*`), `button_count` on a touchpad |
| `mimhid_pen_options`, `mimhid_digitizer_options` | `x`, `y` | pressure, tilt, twist, hover, barrel Usages (`enable_*` with their fields) |
| `mimhid_raw_options`, `mimhid_board_limits`, `mimhid_link_counters`, `mimhid_capability_manifest` | the descriptor and its report table (raw); nothing for the others | |

`docs/EXAMPLES.md` and `examples/` show a full set of values for each; they are examples, not defaults.

`mimhid_integer_field_make(minimum, maximum, bits)` builds one field without physical units.

> [!IMPORTANT]
> The examples and tests set `0x1209`/`0x0001`, an ID [pid.codes](https://pid.codes/1209/0001/) sets aside for testing in-house only. The library fills in no identity: `usb_device.vendor_id` zero is refused, so a device you hand out has the vendor and product ID of its own that you set.

## Lifecycle

```
discover (optional)
device_open                 opens the serial port (e.g. "/dev/ttyACM0" or "COM3") and starts the reader thread;
                            the board's USB device stays detached
  spec_create_* / raw descriptor
  node_open / node_open_raw adds the node on iface.number; the board enumerates again (attaches on the first node)
  device_wait_ready         until the target configures the USB device
  <change state> node_submit   repeat
  node_close                neutral report first, then the board enumerates again without the node
device_close                closes the Nodes left, then detaches the USB device (unless keep_attached)
spec_release
```

`mimhid_node_open`, `mimhid_node_open_raw` and `mimhid_node_close` send the configuration to the board and enumerate the USB device again at once (a *commit*); the target sees an unplug and a replug. To change several Nodes with one commit, put the calls between `mimhid_device_batch_begin` and `mimhid_device_batch_end`; a Node opened inside a batch returns `MIMHID_ERR_BUSY` from input calls until the batch ends. `mimhid_device_commit(device, reconnect_delay_ms, timeouts)` is not needed after either: it sends the configuration and enumerates once more, holding D+ at 0 V for `reconnect_delay_ms` first. `mimhid_device_set_usb_identity` changes the Device descriptor and its strings the same way.

`mimhid_device_disconnect` takes the USB device off the bus (D+ at 0 V) and `mimhid_device_connect` puts it back (D+ pulled up to 3.3 V).

## Interfaces and endpoints

`mimhid_node_config` sets the USB interface and endpoints that carry a Node; nothing is numbered for you.

- `iface.number` labels the interface; the board numbers its interfaces 0, 1, 2, ... in the order of these labels. Nodes with the same number share the interface, and must then all have Report IDs, all different, and the same interface and endpoint settings. Nodes on different interfaces need different endpoint addresses. Anything else is `MIMHID_ERR_PARAM`.
- `iface.class_code` (`0x03`, HID: any other class is `MIMHID_ERR_UNSUPPORTED`, because the board opens HID interfaces only), `iface.subclass` (0, or 1 for a boot interface) and `iface.protocol` (0, 1 keyboard or 2 mouse; HID 1.11 4.2 and 4.3), `iface.bcd_hid` (nonzero), `iface.country_code` (0 when not localized, up to 35), `iface.name` (a string, or null).
- `ep_in`: `address` (`0x81` to `0x80 | max_endpoint_number`, the board's highest endpoint number in `mimhid_board_limits`), `poll_interval_ms` (`bInterval`: 1 to 255 at full speed, 1 to 16 at high speed), `max_packet_size` (1 to the board's `max_endpoint_bytes`). All required.
- `ep_out`: all zero for none; otherwise an address `0x01` to `0x0F`, a poll interval and a packet size, and the target's Output reports reach `event_sink`.

A USB device has one VID and PID, so those belong to the Device, not to a Node.

## Settings of the USB device

`mimhid_device_config` holds `transport`, `usb_device` and `usb_config`. The fields of `usb_device` and `usb_config` are those of the USB 2.0 standard descriptors (section 9.6); a string is UTF-8 text without control characters, at most 126 UTF-16 code units, or null to leave it out.

- `usb_device`: `bcd_usb`, `ep0_max_packet_size` (8, 16, 32 or 64; 64 on a high-speed board) and `language_id` are required (zero is refused) (`bcd_usb` of 0x0210 or more makes the target ask for a BOS descriptor, which the board does not have: answer it with a control-routing rule, see [LIMITS.md](LIMITS.md#who-answers-a-control-request)); `vendor_id` and `product_id` (any value, zero included: USB 2.0 9.6.1 reserves none), `bcd_device`, `device_class`, `device_subclass`, `device_protocol`, `manufacturer`, `product`, `serial`, `serial_from_chip_id` (with a null `serial`, the chip's unique id).
- `usb_config`: `configuration_value` (1: the board has one configuration, and any other value is `MIMHID_ERR_UNSUPPORTED`), `bm_attributes` (bit 7 set; bit 6 self-powered; bit 5 remote wakeup), `bmax_power` (0 to 250, in 2 mA units), `string`.
- `transport`, link: `baud_rate` (`MIMHID_BOOT_BAUD`, any rate up to the board's `max_baud`, or `MIMHID_BAUD_FASTEST`, which measures the fastest the link carries without loss when the Device opens; `mimhid_discover` reports the same figure beforehand, and `mimhid_device_link_counters` shows afterwards that the line keeps losing nothing), `handshake_timeout_ms` (each wait for an answer while the board is looked for on the port), `maximum_report_bytes` (the largest report this Device sends, at most the board's `max_report_bytes` and `MIMHID_HOST_MAX_REPORT_BYTES`), `log_level` (any level but `MIMHID_LOG_DISABLED` needs `log_sink`). All required.
- `transport.linux_hid`: the limits of the target's Linux HID parser ([LIMITS.md](LIMITS.md#linux-hid-parser)). All required; `mimhid_node_open` refuses a Spec that exceeds one with `MIMHID_ERR_OVERFLOW`.
- `transport`, behavior: `validate_reports` (check every built report against its Spec before sending), `keep_attached` (leave the USB device on the target after `mimhid_device_close`).
- Callbacks: `event_sink`, `log_sink`, each with its user pointer.

## Timeouts

The library keeps no timeout: every call that can wait takes its time in milliseconds as an argument, and zero fails with `MIMHID_ERR_UNSET_FIELD` before anything is sent. There are no constants for the waits: the examples use 500 ms for a configuration frame and a serial write, 1000 ms for a close, and 3000 ms for the board's new enumeration, which work for a board on a USB serial adapter, and the library never uses them by itself.

| Wait | Meaning | Taken by |
|---|---|---|
| control | each configuration frame waits this long for the board's answer, and is sent up to three times | `mimhid_device_disconnect`, `mimhid_device_remote_wakeup`, `mimhid_device_set_control_routing`, `mimhid_device_set_loss_reporting`, `mimhid_node_set_feature_response`, `mimhid_node_set_queue_depth`, `mimhid_device_close`, and `mimhid_commit_timeouts` |
| reconnect | the board's new enumeration after a commit | `mimhid_commit_timeouts` |
| send | a serial write may make no progress this long before `MIMHID_ERR_TIMEOUT`, with nothing sent | `mimhid_node_submit`, `mimhid_raw_submit`, `mimhid_report_send`, `mimhid_device_control_reply` |
| drain | the neutral reports of a close | `mimhid_node_close`, `mimhid_device_close` |

`mimhid_commit_timeouts` goes to every call that commits: `mimhid_node_open`, `mimhid_node_open_raw`, `mimhid_node_close`, `mimhid_device_batch_end`, `mimhid_device_commit`, `mimhid_device_connect` and `mimhid_device_set_usb_identity`. It is required also where the call only stages a change inside a batch.

Other calls take their own: `mimhid_discover` and `mimhid_device_wait_ready` (`timeout_ms`), `mimhid_node_submit_blocking` (`timeout_ms`, which also bounds each write), and each control rule (`timeout_ms`, how long the board holds a request; at most `MIMHID_CONTROL_MAX_TIMEOUT_MS`). The wire protocol's fixed times (`MIMHID_BAUD_CONFIRM_MS`, the board's 10 ms receive resync) are the protocol's.

## Input

Input calls change the state of a Node and send nothing. `mimhid_node_submit(node, send_timeout_ms)` encodes the state as a report and writes it to the serial port without waiting for the target. `mimhid_node_submit_blocking` also sends the further reports of a touch frame that does not fit one report. A Node has at most one report in flight; a call that changes its state meanwhile returns `MIMHID_ERR_BUSY`.

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
| Raw | `mimhid_raw_submit`, `mimhid_report_send` (a complete report, Report ID included) |

Each profile call matches one HID report shape. `mimhid_raw_submit` and `mimhid_report_send` cover any device no factory builds.

## Functions

| Group | Functions |
|---|---|
| Diagnostics | `mimhid_last_error`, `mimhid_result_name`, `mimhid_frame_type_name`, `mimhid_version` |
| Discovery | `mimhid_discover`, `mimhid_discovery_count`, `mimhid_discovery_get`, `mimhid_discovery_destroy` |
| Device | `mimhid_device_open`, `mimhid_device_close`, `mimhid_device_latched_error`, `mimhid_device_is_ready`, `mimhid_device_wait_ready`, `mimhid_device_board_info`, `mimhid_device_limits` |
| Commit and bus | `mimhid_device_batch_begin`, `mimhid_device_batch_end`, `mimhid_device_commit`, `mimhid_device_set_usb_identity`, `mimhid_device_connect`, `mimhid_device_disconnect`, `mimhid_device_remote_wakeup` |
| Target requests | `mimhid_device_set_control_routing`, `mimhid_device_control_reply`, `mimhid_device_set_loss_reporting` |
| Spec factories | `mimhid_spec_create_keyboard`, `_mouse`, `_toggle`, `_gamepad`, `_touchscreen`, `_touchpad`, `_pen`, `_digitizer`, `_raw` |
| Spec access | `mimhid_spec_retain`, `mimhid_spec_release`, `mimhid_spec_descriptor`, `mimhid_spec_manifest` |
| Node | `mimhid_node_open`, `mimhid_node_open_raw`, `mimhid_node_close`, `mimhid_node_id`, `mimhid_node_manifest`, `mimhid_node_set_feature_response`, `mimhid_node_set_queue_depth`, `mimhid_node_submit`, `mimhid_node_submit_blocking` |
| Profile state | the calls under [Input](#input) |

`mimhid_version()` returns `(major << 24) | (minor << 16) | patch`; the header also defines `MIMHID_VERSION_MAJOR`, `_MINOR` and `_PATCH`. `mimhid_result_name()` returns the constant's name, or `"MIMHID_RESULT_UNKNOWN"` for an unknown value. `mimhid_frame_type_name()` names a serial frame type, such as the one in `mimhid_error_detail.frame_type`.

## Events

`event_sink` receives, on the library's reader thread: `MOUNTED`, `UNMOUNTED`, `SUSPEND`, `RESUME`, `OUTPUT_REPORT` and `FEATURE_REPORT` (the target wrote a report; the Report ID is separate from the data), `SET_PROTOCOL`, `SET_IDLE`, and the events of the two sections below. The callback must be short, must copy the data it keeps, and must not call the library, except `mimhid_device_control_reply`, which only writes a frame and is the way to answer within a short rule time.

`mimhid_node_set_feature_response` sets the answer to a Feature read of a Report ID. A touch Spec with `enable_contact_count_maximum_feature_declaration` is answered without it. `mimhid_device_remote_wakeup(device, control_timeout_ms)` wakes a suspended target when the configuration allows remote wakeup.

## Report queue and lost reports

Reports wait on the board, in one queue per USB interface, until the target polls them; a report that finds the queue full is dropped. Neither call below changes anything until it is made, and without them the board sends nothing extra.

| Call | What it does |
|---|---|
| `mimhid_node_set_queue_depth(node, depth, control_timeout_ms)` | How many reports may wait (1 up to what the board's queue holds, `MIMHID_ERR_OVERFLOW` beyond). A short queue bounds how late a report can be: with depth 1 a report waits at most one poll interval behind the one being sent. The depth stays with the Node across commits; Nodes sharing an interface take the least depth any of them set. |
| `mimhid_device_set_loss_reporting(device, enabled, interval_ms, control_timeout_ms)` | With `enabled` 1 the board counts the reports it drops and sends `MIMHID_EVENT_REPORT_LOST` (`value`: the count, `node_id`: a Node of the interface) at most once per `interval_ms` (0 to `MIMHID_LOSS_MAX_INTERVAL_MS`; 0 reports as soon as it can). With `enabled` 0 (and `interval_ms` 0) it counts nothing and sends nothing. `mimhid_device_close` turns it off. |

A dropped report may be a key-up or a button release; a program that enabled the event sends its current state again when it comes.

## Control transfers of the target

Two calls, both off until used (see [PROTOCOL.md](PROTOCOL.md#control-transfers) for the wire side):

| Call | Does |
|---|---|
| `mimhid_device_set_control_routing(device, report_all, rules, count, control_timeout_ms)` | Replaces the set: `report_all` 1 reports every request on endpoint 0 as `MIMHID_EVENT_CONTROL_SETUP`; each `mimhid_control_rule` (up to `MIMHID_CONTROL_MAX_RULES`, the board's `max_control_rules`) holds the requests it matches. `timeout_ms` and `when_late` have no default and zero is refused. |
| `mimhid_device_control_reply(device, token, action, data, length, send_timeout_ms)` | Answers a held request: `ANSWER` (data for a device-to-host request, sent whole whatever `wLength` says, also when it is 0; an acknowledgement otherwise), `STALL`, or `LEAVE` (the board's own handling). Writes one frame and returns. |

A held request that nobody answers within its rule's time gets the rule's `when_late` action. `MIMHID_EVENT_CONTROL_DATA` carries the data stage of a host-to-device request; the second reply is the verdict. Standard OUT SET_ADDRESS is reported and never held; SET_CONFIGURATION can be held and answered by rules; a host-to-device data stage longer than `max_control_data_bytes` is reported with `MIMHID_CONTROL_FLAG_TOO_LONG` and never held. `mimhid_device_close` removes the rules. A complete program: [`examples/c/control_transfers.c`](../examples/c/control_transfers.c).

## Result codes

Codes with the same number mean the same in `libaoahid` and `libmimhid`: `OK` 0, `PARAM` 1, `UNSET_FIELD` 2, `UNSUPPORTED` 3, `ACCESS` 6, `BUSY` 7, `NO_DEVICE` 8, `TIMEOUT` 10, `SHORT_TRANSFER` 11, `DESCRIPTOR_REJECTED` 12, `IO` 13, `OVERFLOW` 14, `INTERNAL` 16. Number 4 is the transport's own: `MIMHID_ERR_PROTOCOL` here, `AOAHID_ERR_NOT_AOA` there. `AOAHID_ERR_STALL` (9) and `AOAHID_CLOSE_PENDING` (15) exist only in `libaoahid`, whose USB transfers can outlive a call.

## Differences from libaoahid

- No Context and no event mode: a Device owns its port and one reader thread.
- `mimhid_device_open` takes a serial port path (`"/dev/ttyACM0"`, `"COM3"`) instead of a discovered USB device.
- A Node names its USB interface and endpoints (`mimhid_node_config`), and opening or closing one enumerates the board's USB device again; `mimhid_device_batch_begin` / `_end` gather several changes into one, and `mimhid_device_commit` sets the reconnect delay. `mimhid_commit_timeouts` holds the wait for that enumeration, the one wait libaoahid does not have.
- `mimhid_node_submit` returns once the report is written to the serial port, so its `send_timeout_ms` bounds a write that makes no progress; `aoahid_node_submit` queues a USB transfer, and its `send_timeout_ms` is that transfer's deadline.
- The USB identity is the Device's (`mimhid_device_config`), and the bus can be controlled directly (`mimhid_device_connect`, `mimhid_device_disconnect`).
- The target's requests reach the program: `event_sink`, control routing, lost reports.
- No accessory or Channel calls, and no `CLOSE_PENDING`.
