# Wire protocol (version 3)

The constants live in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h), which both the host library and the firmware include. This page describes how they are used. The link is UART, 8N1. A board starts at 115,200 baud; the host then agrees a faster rate with it (see [Line rate](#line-rate)).

## Frame

```
A5  TYPE  LEN  payload[LEN]  XOR
```

`XOR` is `TYPE ^ LEN ^` every payload byte. A frame with a bad checksum or an unknown length is dropped and the parser looks for the next `A5`. A parser that stops mid-frame for 10 ms starts over, so a damaged length byte cannot stall the link.

| Range | Direction | Meaning |
|---|---|---|
| `0x01` | host to board | `PING`. Payload: the host's protocol version. No sequence number. |
| `0x10`..`0x17` | host to board | Commands. The payload starts with a sequence number. |
| `0x40`..`0x4F` | host to board | Report for node `TYPE & 0x0F`. Payload: the report exactly as the target receives it, Report ID included. No acknowledgement. |
| `0x81` | board to host | `PONG` |
| `0x82` / `0x83` | board to host | `ACK` (`seq, type`) / `NAK` (`seq, type, code`) |
| `0x84` | board to host | `EVENT` (`kind, ...`) |

Report payloads are at most 64 bytes. Other payloads are at most 255 bytes.

## Commands

| Type | Name | Payload after the sequence number |
|---|---|---|
| `0x10` | `DEVICE_CONFIG` | USB identity and power; starts a new staged configuration and clears every staged node |
| `0x11` | `NODE_BEGIN` | one node: id, interface index, descriptor length, poll interval, endpoint size, `bcdHID`, country, boot protocol, interface string. A repeated begin for a node that has not changed is acknowledged again. |
| `0x12` | `NODE_DATA` | node id, offset, up to 240 descriptor bytes |
| `0x13` | `COMMIT` | apply the staged configuration. Reconnect delay in payload. With payload byte `1`: detach from the bus and forget the configuration |
| `0x14` | `FEATURE` | node id, Report ID, up to 32 bytes returned for a Feature `GET_REPORT`; takes effect at once, also on a running configuration |
| `0x15` | `RESET` | drop every queued report |
| `0x16` | `REMOTE_WAKEUP` | signal remote wakeup (NAK `STATE` when the target has not suspended the device) |
| `0x17` | `SET_BAUD` | index of a line rate (see [Line rate](#line-rate)); NAK `PARAM` for a rate the board cannot make. Handled on the receive side, like `PING` |

Field layouts of `DEVICE_CONFIG`, `NODE_BEGIN`, and `NODE_DATA` are in the header comment.

The board acknowledges each command. The host waits `control_timeout_ms` (500 ms by default) and sends the same frame up to three times. NAK codes: `PARAM`, `OVERFLOW`, `STATE`, `DESCRIPTOR`, `UNSUPPORTED`, `BUSY`; the host maps them to `MIMHID_ERR_*`.

### Applying a configuration

1. `DEVICE_CONFIG`, then `NODE_BEGIN` and `NODE_DATA` for every node, then optional `FEATURE` frames. The host resends the whole configuration every time a node opens or closes.
2. `COMMIT`. The board builds the device, configuration, string, and report descriptors from the staged data (the NAK is `DESCRIPTOR` when they do not fit), drops queued reports, disconnects from the bus, waits `reconnect_delay_ms` without blocking, reconnects, and only then sends the `ACK`.
3. When the target mounts the device the board sends `EVENT MOUNTED`.

Only the USB side of the firmware changes the active descriptors; the receive side only stages. On a two-core MCU this means no core ever patches a descriptor that the other is serving.

## Interfaces and nodes

The caller explicitly assigns each node to a USB interface index (`interface_number` in `mimhid_node_options`), told to the board in `NODE_BEGIN`. With enough IN endpoints every node gets its own interface. The firmware does not look inside descriptors beyond what TinyUSB needs; it forwards reports unchanged.

## PONG

```
0 fw_major  1 fw_minor  2 fw_patch  3 ready  4 protocol_version
5 max_nodes  6 max_interfaces  7 max_report_bytes
8..9 max_descriptor_bytes (little endian)  10 flags (bit 0: chip unique id available)
11 chip  12 usb speed (0 full, 1 high)  13 index of the line rate the board is on
14..15 rates the board can make (little endian; bit i: rate i)
16 unique id length n (0..16)  17.. the chip's unique id, n bytes
```

The length is 17 + n. A reader ignores bytes past the ones it knows. `chip` is 0 (unknown), 1 RP2040, 2 RP2350, 3 ESP32-S3, 4 nRF52840, 5 STM32F4.

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

The host maps the interface to the node and delivers `mimhid_event` on its reader thread.

## Wire time

A report of `n` bytes occupies `n + 4` bytes on the wire, 10 bits each: 3.3 µs per byte at 3,000,000 baud and 10.9 µs at 921,600. A 9-byte touch report takes 43 µs at 3,000,000 baud and 141 µs at 921,600.
