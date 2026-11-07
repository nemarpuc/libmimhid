# Wire protocol

The constants live in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h), which both the host library and the firmware include. This page describes how they are used. The link is UART, 8N1. A board starts at 115,200 baud; the host then agrees the fastest rate the link carries with it (see [Line rate](#line-rate)).

## Frame

```
A5  TYPE  LEN  payload[LEN]  CRC_LO  CRC_HI
```

`LEN` is one byte for a payload shorter than 128 bytes. Longer, it is two: `0x80 | (LEN & 0x7F)`, then `LEN >> 7`. The check is CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection, no final xor; check value 0x29B1 for "123456789") of `TYPE`, every `LEN` byte and every payload byte, low byte first. (An XOR byte would let two flipped bits in the same position cancel; a link pushed to the edge of its rate produces exactly such errors.) A frame with a bad CRC or an unknown length is dropped and the parser looks for the next `A5`. A parser that stops mid-frame for 10 ms starts over, so a damaged length byte cannot stall the link.

| Range | Direction | Meaning |
|---|---|---|
| `0x01` | host to board | `PING`. No payload, no sequence number. |
| `0x10`..`0x1B`, `0x1D` | host to board | Commands. The payload starts with a sequence number. |
| `0x1C` | host to board | `PROBE`: link-test frame. No sequence number, no acknowledgement. |
| `0x40`..`0x4F` | host to board | Report for node `TYPE & 0x0F`. Payload: the report exactly as the target receives it, Report ID included. No acknowledgement. |
| `0x81` | board to host | `PONG` |
| `0x82` / `0x83` | board to host | `ACK` (`seq, type`, then the data of the command if it has any) / `NAK` (`seq, type, code`) |
| `0x84` | board to host | `EVENT` (`kind, ...`) |

A report payload is at most the board's `max_report_bytes`. Other payloads are at most the board's `max_frame_payload`, never below 255 (a `NODE_DATA` chunk fits; the firmware refuses to build with a limit below its longest configuration command) and never above `MIMHID_PROTO_MAX_PAYLOAD` (4096). The board says both in its PONG.

## Commands

| Type | Name | Payload after the sequence number |
|---|---|---|
| `0x10` | `DEVICE_CONFIG` | USB identity and power, then the manufacturer, product, serial and configuration strings, each a count of UTF-16 code units (at most 126) followed by the units, little endian; a zero unit is refused (NAK `PARAM`); starts a new staged configuration and clears every staged node. NAK `PARAM` for a control endpoint size that is not 8, 16, 32 or 64 (64 only on a high-speed board, USB 2.0 9.6.1), for `bmAttributes` bits 4..0 set (9.6.3) and for a device subclass under device class 0 (9.6.1); NAK `UNSUPPORTED` for a configuration value other than 1 (the board has one configuration, which `SET_CONFIGURATION(1)` selects) |
| `0x11` | `NODE_BEGIN` | one node: id, interface index, descriptor length (two bytes), interface class, subclass and protocol, `bcdHID` (two bytes), country code, then for the IN and the OUT endpoint its address, packet size (two bytes) and poll interval, the idle rate, flags, and the interface string (a count of UTF-16 code units, then the units, little endian). A repeated begin for a node that has not changed is acknowledged again. NAK `PARAM` for a subclass above 1 or a protocol above 2 (HID 1.11 4.2, 4.3), for an endpoint address that is not 1 to 15 with bits 6..4 clear and the right direction bit (USB 2.0 9.6.6; 0 means the default), and on a high-speed board for a poll interval outside 1 to 16 (USB 2.0 9.6.6); NAK `UNSUPPORTED` for an interface class other than HID (`0x03`), which the board's USB stack would not open. |
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
0 fw_major  1 fw_minor  2 fw_patch  3 ready
4 max_nodes  5 max_interfaces  6 flags (bit 0: chip unique id available)
7 chip  8 usb speed (0 full, 1 high)
9..12 the line rate the board is on (u32)
13..16 max_baud, the fastest rate its UART can make (u32)
17..20 uart flags (u32): bit 0 the board returns to the boot rate on a BREAK
21..22 max_descriptor_bytes  23..24 max_report_bytes  25..26 max_feature_bytes
27..28 max_endpoint_bytes  29..30 max_frame_payload  31..32 max_control_data_bytes
33 max_control_rules  34 unique id length n (0..16)  35.. the chip's unique id, n bytes
```

All wider values are little endian. The length is 35 + n. A reader ignores bytes past the ones it knows. `chip` is 0 (unknown), 1 RP2040, 2 RP2350, 3 ESP32-S3, 4 nRF52840, 5 STM32F4.

**Versions.** The firmware and the library of one release speak the same protocol. A host talks to a board whose `fw_major` and `fw_minor` equal its own; `fw_patch` never changes the wire. The first four bytes keep their place in every release, so the host can read them from any board: it lists a board of another major or minor version in `mimhid_discover` with only those values (`firmware_compatible` 0), and refuses to open it with `MIMHID_ERR_PROTOCOL`. A change of the frame format or of any layout in this document raises the minor number.

## Line rate

There is no table of rates. Only the boot rate is fixed: 115,200 baud (`MIMHID_BOOT_BAUD`), because two parties that have never spoken need one rate in common. Any other rate from there up to the board's `max_baud` (from its UART clock and divider limits, in the PONG) can be agreed.

`SET_BAUD` (payload after the sequence number: the rate, u32) is refused with `NAK PARAM` for 0, for a rate above `max_baud`, and for a rate whose divider is off by more than 2 % (`hal_uart_baud_actual`). Otherwise the board sends the `ACK` at the rate it is on, with the rate it will actually run at (u32), switches once the `ACK` has left, and keeps the new rate only when a valid frame arrives at it within 100 ms (`MIMHID_BAUD_CONFIRM_MS`). Otherwise it goes back to the boot rate. A switch to the boot rate needs no confirmation.

**BREAK.** A UART BREAK (the line held low for longer than a character) returns the board to the boot rate whatever rate it is on and drops the bytes it holds. A program that went away and left the board on a fast rate is therefore found without searching: ping at the boot rate, and if nothing answers, send a BREAK (10 ms) and ping again.

**Counters and `PROBE`.** The board counts, since the last reset and each saturating at 65535, the frames it dropped for a bad CRC, UART framing errors, bytes the UART lost, and reads that found the receive ring three quarters full or more (a lap of the ring cannot be seen afterwards, so this is the near miss that comes first), and the `PROBE` frames whose payload (byte *i* is `i*7+3` mod 256) was right. A counter the hardware only latches counts once per look: a count is a lower bound, zero is exact. `COUNTERS` (payload: flags, bit 0 = reset after reading) returns the five values as u16 in the `ACK`: `probes_good, bad_crc, framing, uart_overrun, ring_overrun`.

**The host never trusts a rate it has not seen work**, because a USB serial adapter may accept a rate from the operating system and send at another one, and a marginal rate loses bytes rarely:

1. Find the board (above). It is on the boot rate.
2. Try a rate: set the port to it once to see that the driver takes it (and reports within 3 % of it), send `SET_BAUD` at the boot rate, switch the port, ping three times (the PONG names the new rate), then run the link test: `COUNTERS` with reset, a burst of `PROBE` frames back to back, `COUNTERS`. The rate passes when every probe arrived and the loss counters are zero. `mimhid_device_open` does this with a short burst (32 frames of 96 bytes) for the caller's `baud_rate` and fails when it is not carried.
3. When a trial fails, set the port back to the boot rate, send a BREAK, and ping: the board is back at once instead of after the 100 ms of step 2's window.
4. `MIMHID_BAUD_FASTEST` and `mimhid_discover` search: from `max_baud` downward in steps of 20 % to the first rate that passes; then between the last failure and that pass by halving to 2 %; then the rate must carry a longer burst (250 ms of line time, up to 60000 frames), else 5 % lower until one does. At most about 30 trials; the boot rate when none passes. `mimhid_discover` reports the result as `baud_rate` and leaves the board on the boot rate.

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

A request matches a rule when each field ANDed with the mask equals the value; the first matching rule holds the request. The set replaces the previous one, the board starts with none, a timeout of 0 or above `MIMHID_CONTROL_TIMEOUT_MAX` (5000, the 5 seconds USB 2.0 9.2.6.1 gives any command), an unknown action, or a value with bits outside its mask is refused with `PARAM`.

**How it is done.** The board wraps TinyUSB's `dcd_event_handler`, which every controller driver calls with the setup packet in the USB interrupt (`-Wl,--wrap=dcd_event_handler`). A request that matches a rule is not passed on; the USB side reports it (`EVENT CONTROL_SETUP`: token, flags, the 8 bytes) and waits. When the answer comes, or the time runs out, the request is given to TinyUSB: unchanged for `LEAVE` (the board's own handling), or with the type bits of `bmRequestType` set to vendor for `ANSWER` and `STALL`, which `tud_vendor_control_xfer_cb` then serves from the host's data (or stalls). The control endpoint's data and status stages therefore run in TinyUSB as for any request, and the controller NAKs until they start.

`CONTROL_REPLY` carries the token, the action (0 leave, 1 answer, 2 stall) and, for an answer to a device-to-host request, the data stage (sent whole whatever `wLength` says: a longer answer than the host asked for makes it report an overrun, and with `wLength` 0 the data stage is still started, as a real device that ignores a zero length does). It is acknowledged when the board still waits for that token; `NAK STATE` says it does not (late, or the target gave up and sent another request). For a host-to-device request with a data stage, an `ANSWER` lets the data stage happen; the board then sends `CONTROL_DATA` with the bytes and waits, within the same time, for a second `CONTROL_REPLY` that is the verdict: `ANSWER` or `LEAVE` acknowledges, `STALL` stalls, and the rule's action applies when none comes. The USB side of the firmware is busy during that wait; reports queue and go out after it.

Event flags: `0x01` held, `0x02` a host-to-device data stage longer than `max_control_data_bytes` (reported, never held), `0x04` standard OUT SET_ADDRESS (reported, never held: it changes the state of the board's own controller), `0x08` earlier transfers were not reported because the board's queue of 7 was full. Tokens run 1..255. A new request, a bus reset, a suspend and a `COMMIT` end a hold.

Limits of what this reaches: the board sees only the setup packet of a transfer it does not hold; the data stage of one it leaves to TinyUSB is not reported.

## Wire time

A report of `n` bytes occupies `n + 5` bytes on the wire, 10 bits each: 3.3 µs per byte at 3,000,000 baud and 0.83 µs at 12,000,000. A 9-byte touch report takes 47 µs at 3,000,000 baud and 12 µs at 12,000,000. The target-facing USB endpoint dominates (see [LATENCY.md](LATENCY.md)).
