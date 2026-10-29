# Wire protocol (version 6)

The constants live in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h), which both the host library and the firmware include. This page describes how they are used. The link is UART, 8N1. A board starts at 115,200 baud; the host then agrees a faster rate with it (see [Line rate](#line-rate)).

## Frame

```
A5  TYPE  LEN  payload[LEN]  XOR
```

`LEN` is one byte for a payload shorter than 128 bytes. Longer, it is two: `0x80 | (LEN & 0x7F)`, then `LEN >> 7`. `XOR` is `TYPE ^` every `LEN` byte `^` every payload byte. A frame shorter than 128 bytes is byte for byte the frame of protocol 3, so a board still on protocol 3 answers a `PING` and the host can tell which protocol it speaks. A frame with a bad checksum or an unknown length is dropped and the parser looks for the next `A5`. A parser that stops mid-frame for 10 ms starts over, so a damaged length byte cannot stall the link.

| Range | Direction | Meaning |
|---|---|---|
| `0x01` | host to board | `PING`. Payload: the host's protocol version. No sequence number. |
| `0x10`..`0x1B` | host to board | Commands. The payload starts with a sequence number. |
| `0x40`..`0x4F` | host to board | Report for node `TYPE & 0x0F`. Payload: the report exactly as the target receives it, Report ID included. No acknowledgement. |
| `0x81` | board to host | `PONG` |
| `0x82` / `0x83` | board to host | `ACK` (`seq, type`) / `NAK` (`seq, type, code`) |
| `0x84` | board to host | `EVENT` (`kind, ...`) |

A report payload is at most the board's `max_report_bytes`. Other payloads are at most the board's `max_frame_payload`, never below 255 (so every configuration command fits) and never above `MIMHID_PROTO_MAX_PAYLOAD` (4096). The board says both in its PONG.

## Commands

| Type | Name | Payload after the sequence number |
|---|---|---|
| `0x10` | `DEVICE_CONFIG` | USB identity and power; starts a new staged configuration and clears every staged node |
| `0x11` | `NODE_BEGIN` | one node: id, interface index, descriptor length (two bytes), interface class, subclass and protocol, `bcdHID` (two bytes), country code, then for the IN and the OUT endpoint its address, packet size (two bytes) and poll interval, the idle rate, flags, and the interface string. A repeated begin for a node that has not changed is acknowledged again. |
| `0x12` | `NODE_DATA` | node id, offset, up to 240 descriptor bytes |
| `0x13` | `COMMIT` | apply the staged configuration. Reconnect delay in payload. With payload byte `1`: detach from the bus and forget the configuration |
| `0x14` | `FEATURE` | node id, Report ID, up to `max_feature_bytes` returned for a Feature `GET_REPORT`; takes effect at once, also on a running configuration |
| `0x15` | `RESET` | drop every queued report |
| `0x16` | `REMOTE_WAKEUP` | signal remote wakeup (NAK `STATE` when the target has not suspended the device) |
| `0x17` | `SET_BAUD` | index of a line rate (see [Line rate](#line-rate)); NAK `PARAM` for a rate the board cannot make. Handled on the receive side, like `PING` |

| `0x18` | `CONTROL_CONFIG` | which control transfers of the target are reported and which are held (see [Control transfers](#control-transfers)) |
| `0x19` | `CONTROL_REPLY` | the answer to a held control transfer. Handled on the receive side, like `PING` and `SET_BAUD`; the host does not wait for its acknowledgement |
| `0x1A` | `LOSS_CONFIG` | enabled (0 or 1), the least interval between two `REPORT_LOST` events in ms (two bytes; 0 while disabled). The board starts with it off, and then counts and sends nothing for it |
| `0x1B` | `QUEUE_DEPTH` | node id, how many reports may wait on the node's interface (1 up to what the queue holds: NAK `OVERFLOW` beyond). Kept with the staged node, so it survives a `COMMIT`, and takes effect at once on a running configuration. Nodes sharing an interface share its queue, which takes the least depth they set. Without it the queue holds all it can: 15 reports on a board whose `max_report_bytes` is at most 64, 7 otherwise |

Field layouts of `DEVICE_CONFIG`, `NODE_BEGIN`, and `NODE_DATA` are in the header comment.

The board acknowledges each command. The host waits the call's `control_timeout_ms` (no default; every call that sends a configuration frame takes one) and sends the same frame, with the same sequence number, up to three times. The board keeps the answer to the last command it ran: a command arriving again with that sequence number and type is answered again and not run twice (a `COMMIT` does not re-enumerate, a `RESET` drops nothing more), and an answer that was only late still answers the host. A `PING` makes the board forget it, since a new host starts its numbers anew. NAK codes: `PARAM`, `OVERFLOW`, `STATE`, `DESCRIPTOR`, `UNSUPPORTED`, `BUSY`; the host maps them to `MIMHID_ERR_*`.

### Applying a configuration

1. `DEVICE_CONFIG`, then `NODE_BEGIN` and `NODE_DATA` for every node, then optional `FEATURE` frames. The host resends the whole configuration every time a node opens or closes.
2. `COMMIT`. The board builds the device, configuration, string, and report descriptors from the staged data (the NAK is `DESCRIPTOR` when they do not fit), drops queued reports, disconnects from the bus, waits `reconnect_delay_ms` without blocking, reconnects, and only then sends the `ACK`.
3. When the target mounts the device the board sends `EVENT MOUNTED`.

Only the USB side of the firmware changes the active descriptors; the receive side only stages. On a two-core MCU this means no core ever patches a descriptor that the other is serving.

## Interfaces and nodes

The caller explicitly assigns each node to a USB interface index (`iface.number` in `mimhid_node_config`), told to the board in `NODE_BEGIN`. With enough IN endpoints every node gets its own interface. The firmware does not look inside descriptors beyond what TinyUSB needs; it forwards reports unchanged.

## PONG

```
0 fw_major  1 fw_minor  2 fw_patch  3 ready  4 protocol_version
5 max_nodes  6 max_interfaces  7 flags (bit 0: chip unique id available)
8 chip  9 usb speed (0 full, 1 high)  10 index of the line rate the board is on
11..12 rates the board can make (little endian; bit i: rate i)
13..14 max_descriptor_bytes  15..16 max_report_bytes  17..18 max_feature_bytes
19..20 max_endpoint_bytes  21..22 max_frame_payload      (all little endian)
23..24 max_control_data_bytes  25 max_control_rules
26 unique id length n (0..16)  27.. the chip's unique id, n bytes
```

The length is 27 + n. A reader ignores bytes past the ones it knows. `chip` is 0 (unknown), 1 RP2040, 2 RP2350, 3 ESP32-S3, 4 nRF52840, 5 STM32F4.

The first five bytes keep their place in every version. The host reads them from any board, lists a board on another `protocol_version` in `mimhid_discover` with only those values, and refuses to open it with `MIMHID_ERR_PROTOCOL`. Version 2 (libmimhid 0.1) answered at 3,000,000 baud (1,000,000 on nRF52840); the search finds it there.

## Line rate

| Index | Rate |
|---:|---|
| 0 | 115,200 (boot rate) |
| 1 | 230,400 |
| 2 | 460,800 |
| 3 | 921,600 |
| 4 | 1,000,000 |
| 5 | 1,500,000 |
| 6 | 2,000,000 |
| 7 | 3,000,000 |
| 8 | 4,000,000 |

A board starts at the boot rate and reports in its PONG which rates it can make within 2 % (`hal_uart_baud_ok`). On `SET_BAUD` it sends the `ACK` at the rate it is on, switches once the `ACK` has left, and keeps the new rate only when a valid frame arrives at it within 100 ms (`MIMHID_BAUD_CONFIRM_MS`). Otherwise it goes back to the boot rate. A switch to the boot rate needs no confirmation.

The host never trusts a rate it has not seen work, because a USB serial adapter may accept a rate from the operating system and send at another one:

1. Find the board: ping at the boot rate, then at each rate of the table (a board left on a fast rate by a program that went away). Bring it back to the boot rate.
2. Switch to a rate: set the port to it once to see that the driver takes it, send `SET_BAUD` at the boot rate, switch the port, and ping three times. Three PONGs that name the new rate settle it. `mimhid_device_open` does this for the caller's `baud_rate` and fails when it is not carried.
3. When the PONGs do not come, set the port back to the boot rate, wait until the board has gone back (100 ms after the switch, plus a margin) and check with a ping. If a ping did reach the board, it kept the new rate; the search of step 1 finds it.
4. `mimhid_discover` measures: it tries step 2 for each rate the board can make and the operating system can set, fastest first, reports the first one settled as `baud_rate`, and leaves the board on the boot rate.

`mimhid_device_close` sends `SET_BAUD` to the boot rate, so the next program finds the board at once. The host opens a port for its process alone (`TIOCEXCL` on Linux and macOS; Windows ports are exclusive), so a search does not ping a board another program is using.

## Events

| Kind | Data |
|---|---|
| `MOUNTED`, `UNMOUNTED`, `RESUME` | none |
| `SUSPEND` | remote wakeup enabled by the target (0 or 1) |
| `SET_REPORT` | interface, report type, Report ID, bytes |
| `SET_PROTOCOL` | interface, protocol (0 boot, 1 report) |
| `SET_IDLE` | interface, idle rate |
| `CONTROL_SETUP` | token, flags, the 8 setup bytes (see [Control transfers](#control-transfers)) |
| `CONTROL_DATA` | token, the data stage of a host-to-device transfer |
| `REPORT_LOST` | interface, count (u16, at most 65535): reports dropped because the interface's queue was full, since the last such event. Only after `LOSS_CONFIG` enabled it, at most once per its interval |

The host maps the interface to the node and delivers `mimhid_event` on its reader thread.

## Control transfers

Besides the reports, the target talks to the board on endpoint 0. Without configuration the board answers it all itself (TinyUSB does, and the HID class driver for the report requests). `CONTROL_CONFIG` changes that, for as long as the host wants:

```
flags (bit 0: report every control transfer)  rule count n (0..max_control_rules)
n rules of 15 bytes: bmRequestType mask, value  bRequest mask, value  wValue mask, value (2+2)
                     wIndex mask, value (2+2)  timeout in ms (2)  action after the timeout
```

A request matches a rule when each field ANDed with the mask equals the value; the first matching rule holds the request. The set replaces the previous one, the board starts with none, a timeout of 0 or above `MIMHID_CONTROL_TIMEOUT_MAX` (4000), an unknown action, or a value with bits outside its mask is refused with `PARAM`.

**How it is done.** The board wraps TinyUSB's `dcd_event_handler`, which every controller driver calls with the setup packet in the USB interrupt (`-Wl,--wrap=dcd_event_handler`). A request that matches a rule is not passed on; the USB side reports it (`EVENT CONTROL_SETUP`: token, flags, the 8 bytes) and waits. When the answer comes, or the time runs out, the request is given to TinyUSB: unchanged for `LEAVE` (the board's own handling), or with the type bits of `bmRequestType` set to vendor for `ANSWER` and `STALL`, which `tud_vendor_control_xfer_cb` then serves from the host's data (or stalls). The control endpoint's data and status stages therefore run in TinyUSB as for any request, and the controller NAKs until they start.

`CONTROL_REPLY` carries the token, the action (0 leave, 1 answer, 2 stall) and, for an answer to a device-to-host request, the data stage (sent whole whatever `wLength` says: a longer answer than the host asked for makes it report an overrun, and with `wLength` 0 the data stage is still started, as a real device that ignores a zero length does). It is acknowledged when the board still waits for that token; `NAK STATE` says it does not (late, or the target gave up and sent another request). For a host-to-device request with a data stage, an `ANSWER` lets the data stage happen; the board then sends `CONTROL_DATA` with the bytes and waits, within the same time, for a second `CONTROL_REPLY` that is the verdict: `ANSWER` or `LEAVE` acknowledges, `STALL` stalls, and the rule's action applies when none comes. The USB side of the firmware is busy during that wait; reports queue and go out after it.

Event flags: `0x01` held, `0x02` a host-to-device data stage longer than `max_control_data_bytes` (reported, never held), `0x04` standard OUT SET_ADDRESS (reported, never held: it changes the state of the board's own controller), `0x08` earlier transfers were not reported because the board's queue of 7 was full. Tokens run 1..255. A new request, a bus reset, a suspend and a `COMMIT` end a hold.

Limits of what this reaches: the board sees only the setup packet of a transfer it does not hold; the data stage of one it leaves to TinyUSB is not reported.

## Wire time

A report of `n` bytes occupies `n + 4` bytes on the wire, 10 bits each: 3.3 µs per byte at 3,000,000 baud and 10.9 µs at 921,600. A 9-byte touch report takes 43 µs at 3,000,000 baud and 141 µs at 921,600.
