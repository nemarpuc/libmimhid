# Wire protocol

The constants live in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h), which both the host library and the firmware include. This page describes how they are used. The link is UART, 8N1. A board starts at 115,200 baud; the host then agrees the fastest rate the link carries with it (see [Line rate](#line-rate)).

## Frame

```text
A5  TYPE  LEN  payload[LEN]  CRC_LO  CRC_HI
```

`LEN` is one byte for a payload shorter than 128 bytes. Longer, it is two: `0x80 | (LEN & 0x7F)`, then `LEN >> 7`. The check is CRC-16/CCITT-FALSE (polynomial `0x1021`, initial value `0xFFFF`, no reflection, no final xor; check value `0x29B1` for `"123456789"`) of `TYPE`, every `LEN` byte, and every payload byte, low byte first. (An XOR byte would let two flipped bits in the same position cancel; a link pushed to the edge of its rate produces exactly such errors.) A frame with a bad CRC or an unknown length is dropped and the parser looks for the next `A5`. A damaged length byte can announce a frame whose bytes never come, so both ends give up an incomplete frame when the line is idle: the firmware after 10 ms (`RX_IDLE_RESYNC_MS` in `firmware/common/core.c`), the host when a read of 10 ms brings nothing (`Link`'s reader thread calls `FrameParser::idle`). Each then drops one byte and looks for the next `A5`.

| Range | Direction | Meaning |
|---|---|---|
| `0x01` | host to board | `PING`. No payload, no sequence number. |
| `0x10`..`0x1B`, `0x1D`..`0x1F` | host to board | Commands. The payload starts with a one-byte sequence number, which the board echoes in its `ACK` or `NAK`. The host counts it up by one per command and lets it wrap. |
| `0x1C` | host to board | `PROBE`: link-test frame. No sequence number, no acknowledgement. |
| `0x41`..`0x4F` | host to board | `EP_IN`: data for the IN endpoint number `TYPE & 0x0F` (`MIMHID_T_EP_IN_BASE | number`). On a HID endpoint the payload is the report exactly as the target receives it, Report ID included, and an empty one is dropped; on an endpoint of the generic driver it is one whole transfer (at most `max_transfer_bytes`; empty is a zero-length packet, USB 2.0 5.8.3; a frame that is not the last of a longer transfer is a multiple of the packet size). No acknowledgement. A frame for an endpoint that is not an IN endpoint of the active configuration is dropped. `0x40` (endpoint 0) is refused: the control endpoint is not a data endpoint. |
| `0xC1`..`0xCF` | board to host | `EP_OUT`: a packet the target sent on the OUT endpoint number `TYPE & 0x0F` of the generic driver, as it came (`MIMHID_T_EP_OUT_BASE | number`). No acknowledgement. The host delivers it as `MIMHID_EVENT_ENDPOINT_DATA`. Packets of one endpoint arrive in order. |
| `0x81` | board to host | `PONG` |
| `0x82` / `0x83` | board to host | `ACK` (`seq, type`, then the data of the command if it has any) / `NAK` (`seq, type, code`) |
| `0x84` | board to host | `EVENT` (`kind, ...`) |

An `EP_IN` payload is at most the board's `max_report_bytes`. Other payloads are at most the board's `max_frame_payload`, never below 255 (a `DESC_DATA` chunk fits; the firmware refuses to build with a limit below its longest configuration command) and never above `MIMHID_PROTO_MAX_PAYLOAD` (4096). The board says both in its `PONG`.

## Commands

| Type | Name | Payload after the sequence number |
|---|---|---|
| `0x10` | `DESC_RESET` | None. Starts a new staged configuration: forgets every staged descriptor, string, Feature answer and endpoint setting. The active configuration is not touched. |
| `0x11` | `DESC_BEGIN` | `kind` (1 device, 2 configuration, 3 string, 4 HID report), `index`, language id (u16 LE), total length (u16 LE), `flags`. Announces one descriptor of `total` bytes, which `DESC_DATA` then sends. Device: index 0, language 0, 18 bytes (`NAK_OVERFLOW` above 18; a shorter one is refused at `COMMIT`). Configuration: index 0, language 0, at most `MIMHID_MAX_CONFIG_BYTES`. String: index 1..255, a language id, an even 2..254 bytes (USB 2.0 9.6.7); index 0 is never sent, the board answers it with the language ids of the strings it holds. Flag bit 0 (`MIMHID_DESC_F_SERIAL_FROM_ID`): total 0, the board makes the string from its chip id (`NAK_UNSUPPORTED` on a chip without one). HID report: index the interface number, language 0, at most `MIMHID_MAX_DESC_BYTES` for all of them together. `NAK_PARAM` for an index, language, flag or length the kind does not take; `NAK_OVERFLOW` when the descriptor, the string table (`MIMHID_MAX_STRINGS` strings, whatever their language) or the string store (`MIMHID_MAX_STRING_BYTES`) cannot hold it. The same announcement repeated is acknowledged again; another length or flag for a descriptor already announced is `NAK_STATE`. |
| `0x12` | `DESC_DATA` | `kind`, `index`, language id (u16 LE), offset (u16 LE), up to 240 bytes (`MIMHID_DESC_DATA_CHUNK`) of the announced descriptor. A chunk may start only at or before the end of the bytes received so far (`NAK_PARAM` for a gap or for bytes past the announced length); a chunk repeated, same offset and bytes, is acknowledged again. `NAK_STATE` for a descriptor that was not announced, and for a string the board makes itself. |
| `0x13` | `COMMIT` | Three bytes: detach flag, then two bytes the firmware ignores (the host sends them as zero on attach, and as `reconnect_delay_ms` on the detach that follows the last node's close). Attach (`0`): walk the staged configuration once (see [The commit walk](#the-commit-walk); `NAK_DESCRIPTOR` and the previous configuration kept when it fails), drop queued reports, apply the queue depths, clear held control transfers, call `tud_connect()`, and `ACK`. Detach (`1`): call `tud_disconnect()`, drop queued reports, clear held control transfers and the active configuration, and `ACK`. |
| `0x14` | `FEATURE` | Interface number, Report ID, up to `max_feature_bytes` returned for a Feature `GET_REPORT` of that interface; takes effect at once, also on a running configuration. `NAK_STATE` unless the staged configuration is complete and has that interface. |
| `0x15` | `RESET` | Drop every queued report across all interfaces. |
| `0x16` | `REMOTE_WAKEUP` | Signal USB remote wakeup (`NAK_STATE` when the target has not suspended the device). |
| `0x17` | `SET_BAUD` | Requested baud rate (`u32` LE; see [Line rate](#line-rate)); `NAK_PARAM` for `0`, a rate above `max_baud`, or a rate whose divider error exceeds 2 %. Handled on the receive side, like `PING`; `ACK` carries the actual baud rate (`u32` LE). |
| `0x18` | `CONTROL_CONFIG` | Which control transfers of the target are reported and which are held (see [Control transfers](#control-transfers)). |
| `0x19` | `CONTROL_REPLY` | The answer to a held control transfer: `token`, `action` (`0` leave, `1` answer, `2` stall, `3` drop), and optional data bytes. Handled on the receive side, like `PING` and `SET_BAUD`; the board replies with `ACK` (or `NAK_STATE` if the hold already expired), though the host's `Link::send_control_reply` writes the frame without waiting so it can be called inside `event_sink`. |
| `0x1A` | `LOSS_CONFIG` | Enabled (`0` or `1`), and the minimum interval between two `TRANSFER_LOST` events in ms (u16 LE; `0` while disabled). The board starts with it off, and then counts and sends nothing for it. |
| `0x1B` | `EP_CONFIG` | Endpoint address, depth, policy. The address is that of an IN endpoint of the staged configuration (`NAK_STATE` when there is none or it lacks that endpoint; `NAK_PARAM` for an address that is not `0x81..0x8F`; `NAK_UNSUPPORTED` for an OUT address). The depth is how many transfers may wait on the endpoint, the one the controller is sending not counted (`NAK_PARAM` for `0`, `NAK_OVERFLOW` beyond what the queue holds). The policy is what happens when that many wait: `MIMHID_EP_FULL_DROP_NEWEST` (`1`) drops the transfer that finds the queue full, `DROP_OLDEST` (`2`) drops the oldest that is not being sent, `WAIT` (`3`) is `DROP_NEWEST` on the board: the host counts and sends no more than the depth, and learns what the board finished from `EV_EP_DONE`. A HID endpoint takes `DROP_NEWEST` only (`NAK_UNSUPPORTED`); any other value is `NAK_PARAM`. A queue of the generic driver is cut from the board's pool of `endpoint_queue_slots` (`depth + 1` slots for `DROP_NEWEST` and `WAIT`, `2 * depth + 1` for `DROP_OLDEST`; `NAK_OVERFLOW` beyond the pool). The host sends one for every IN endpoint before the attach `COMMIT`, a generic one is required (`NAK_DESCRIPTOR` at the commit when missing), and it takes effect at once on a running configuration when the new queue fits. HID endpoints without one hold all they can: 15 reports on a board whose `max_report_bytes` is at most 64, 7 otherwise. |
| `0x1C` | `PROBE` | Link-test payload (no sequence number, no `ACK`; byte `i` is `(i * 7 + 3) & 0xFF`). Verified on the receive side and counted in `probes_good` / `bad_crc`. |
| `0x1D` | `COUNTERS` | Flags byte (bit 0: reset counters after reading). Handled on the receive side; `ACK` carries five `u16` LE counters (`probes_good`, `bad_crc`, `framing`, `uart_overrun`, `ring_overrun`). |
| `0x1E` | `EP_STALL` | Endpoint address, then `1` to stall or `0` to clear the Halt feature (USB 2.0 9.4.5): the endpoint answers STALL until it is cleared by this command or by the target; stalling an IN endpoint drops what waits and the transfer being sent. `NAK_STATE` when the address is not an endpoint of the generic driver in the active configuration (a HID endpoint included), `NAK_PARAM` for a bad flag. |
| `0x1F` | `EP_FLUSH` | IN endpoint address: drops what waits on the board; a transfer the controller already has completes. `NAK_STATE` as for `EP_STALL`, `NAK_PARAM` for an OUT endpoint. |

Field layouts of `DESC_BEGIN`, `DESC_DATA`, `FEATURE`, `EP_CONFIG`, `EP_STALL` and `EP_FLUSH` are documented in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h).

The board acknowledges each command (`0x10..0x1B`, `0x1D..0x1F`). The host waits the call's `control_timeout_ms` (no default; every call that sends a configuration frame takes one) and sends the same frame, with the same sequence number, up to three times (`kCommandAttempts` in `src/api/support.hpp`). The attach and detach `COMMIT` that `Link::connect` and `Link::disconnect` send go once. The board keeps the answer to the last command the USB side ran: a command arriving again with that sequence number and type is answered again and not run twice (a `COMMIT` does not re-enumerate, a `RESET` drops nothing more), and an answer that was only late still answers the host. A `PING` makes the board forget it, since a new host starts its numbers anew. NAK codes: `PARAM` (`1`), `OVERFLOW` (`2`), `STATE` (`3`), `DESCRIPTOR` (`4`), `UNSUPPORTED` (`5`), `BUSY` (`6`); the host maps them to `MIMHID_ERR_*`.

### Applying a configuration

`commit_device` (`src/api/device_ops.cpp`) does this on every commit:

1. When nodes are left, the host composes every descriptor (`src/usb/descriptor_composer.cpp`) and sends `DESC_RESET`, then the device descriptor, the configuration descriptor, the string descriptors and the report descriptor of every HID interface, each as a `DESC_BEGIN` and `DESC_DATA` chunks, then a `FEATURE` frame for each registered answer and an `EP_CONFIG` for every IN endpoint (a HID interface gets one command at least as deep as its Nodes' least depth). The board stores them; the target sees no change yet. The host resends the whole configuration every time a node opens or closes. When no node is left, the host sends only a detach `COMMIT`, and the steps below are skipped.
2. If the board's USB device is on the target, the host sends a detach `COMMIT` (`Link::disconnect`, waiting `control_timeout_ms`), and then waits `reconnect_delay_ms` when `mimhid_device_commit` was given one.
3. The host sends an attach `COMMIT` (`Link::connect`) and waits up to `reconnect_timeout_ms` for its `ACK`. The board walks the staged configuration (`NAK_DESCRIPTOR` when it does not hold together), makes it the active one, drops queued reports, calls `tud_connect()`, and sends `ACK`. The host returns on that `ACK`.
4. When the target configures the device (`SET_CONFIGURATION(1)`), the board sends `EVENT MOUNTED`. `mimhid_device_wait_ready` waits for it; no commit does.

The receive side of the firmware answers `PING`, `SET_BAUD`, `COUNTERS`, `PROBE`, and `CONTROL_REPLY` itself. It puts every other command in a mailbox (`g_cmd`, `cmd_push`: a ring of four slots that holds three commands; a full mailbox drops the frame, and the host sends it again when no answer comes), and the USB side runs it (`run_command`, from `mimhid_usb_poll`). Staging and applying a configuration therefore both happen on the USB side, so on a two-core MCU no core changes a descriptor that the other is serving.

## The commit walk {#the-commit-walk}

The host composes the USB descriptors; the board takes them as bytes and serves them to the target without changing them. At `COMMIT` it walks the staged configuration once and refuses it with `NAK_DESCRIPTOR`, keeping the previous configuration (the target sees no change and nothing is disconnected), unless all of this holds:

- **Device descriptor** (USB 2.0 9.6.1): complete, 18 bytes, `bLength` 18, type 1, `bcdUSB` not zero, one configuration, a control endpoint size of 8, 16, 32 or 64 (only 64 on a high-speed board; the board's endpoint 0 takes the size from byte 7 of this descriptor).
- **Configuration descriptor** (9.6.3): complete, `bLength` 9, type 2, `wTotalLength` equal to the bytes held, `bConfigurationValue` 1 (the stack maps `SET_CONFIGURATION(1)` to the one configuration), `bmAttributes` with bit 7 set and bits 4..0 clear, `bNumInterfaces` equal to the interfaces found.
- **Every descriptor inside it**: a `bLength` of at least 2 that stays inside the blob, and only interface, HID, and endpoint descriptors.
- **Interfaces** (9.6.5): numbered 0, 1, 2, ... in order, alternate setting 0 only; at most `max_interfaces_any` of them and at most `MIMHID_HID_ITFS` of class HID (`0x03`). A HID interface is followed at once by its HID descriptor (HID 1.11 6.2.1), whose one class descriptor is a report descriptor (type `0x22`), and has one interrupt IN endpoint and at most one interrupt OUT endpoint. An interface of any other class is served by the generic driver and has any number of interrupt or bulk endpoints, with the descriptors the host put between the interface and its first endpoint taken as given (each with a `bLength` of at least 2 inside the blob). `bNumEndpoints` equals the endpoints found. An interface association descriptor (USB ECN, `bLength` 8, type `0x0B`) may stand only before the first interface of a group of non-HID interfaces and names that many consecutive ones.
- **Endpoints** (9.6.6): interrupt or bulk (a type listed in `transfer_types`); the endpoint number from 1 to `max_endpoint_number`, bits 6..4 of the address clear; no address used twice; packet sizes from 1 to `max_endpoint_bytes` (bulk at full speed 8, 16, 32 or 64, at high speed 512, USB 2.0 5.8.3); an interrupt `bInterval` of at least 1, and from 1 to 16 on a high-speed board. Every IN endpoint of the generic driver has an `EP_CONFIG` staged.
- **Report descriptors**: one for every HID interface, complete, of the length its HID descriptor says, and none for an interface that is not HID.
- **Totals**: configuration descriptor at most `max_config_descriptor_bytes`, strings at most `max_strings`, queue slots at most `endpoint_queue_slots`.
- **Strings** (9.6.7): complete, `bLength` equal to the bytes held, type 3.

From the walk the board keeps which HID instance (TinyUSB numbers them in interface order) serves each HID interface and which driver and endpoint belongs to each interface of the generic driver. An `EP_IN` frame is queued on the instance or the endpoint it names, and an event names the interface by its number.

## Interfaces and nodes

The interface number (`mimhid_usb_interface.number`, `mimhid_hid_node_config.interface_number`) is the `bInterfaceNumber` the target sees. The host composes the configuration descriptor with the interfaces of all Nodes in number order, and the numbers in use must be 0, 1, 2, ... without a gap: otherwise the commit fails with `MIMHID_ERR_PARAM`, nothing is sent, and `mimhid_error_detail.offset` is the first number that is missing. Nothing is renumbered. Profile Nodes with the same number share a HID interface: the host joins their report descriptors in node order, and requires that they all have Report IDs, all different, and the same interface and endpoint settings (the queue depth excepted). The host sends the board no node: a report goes to the endpoint of its Node's interface (`EP_IN | (ep_in.address & 0x0F)`), so a Device holds as many Nodes as it likes (up to 65534) within the board's interface and descriptor limits. The caller's strings keep their index from 5 up (`mimhid_device_add_string`); an interface name takes the lowest index above 4 not taken, in interface order.

## PONG

```text
0 fw_major  1 fw_minor  2 fw_patch  3 ready
4 reserved, zero  5 max_interfaces  6 flags (bit 0: chip unique id available)
7 chip  8 usb speed (0 full, 1 high)
9..12 the line rate the board is on (u32)
13..16 max_baud, the fastest rate its UART can make (u32)
17..20 uart flags (u32): bit 0 the board returns to the boot rate on a BREAK
21..22 max_descriptor_bytes  23..24 max_report_bytes  25..26 max_feature_bytes
27..28 max_endpoint_bytes  29..30 max_frame_payload  31..32 max_control_data_bytes
33 max_control_rules  34 max_endpoint_number (the highest endpoint number the controller has)
35 unique id length n (0..16)
36 max_interfaces_any (interfaces of all classes)  37..38 max_config_descriptor_bytes
39 max_strings  40..41 max_transfer_bytes  42..43 endpoint_queue_slots
44 transfer_types (bit 2 bulk, bit 3 interrupt)  45.. the chip's unique id, n bytes
```

All wider values are little endian. The length is `45 + n` (the minimum, `MIMHID_PONG_MIN_LEN`, is 45). (Version 0.2.0 added byte 34 and moved the unique id; version 0.3.0 added bytes 36 to 44 and moved it again; the length byte stays at 35.) A reader ignores bytes past the ones it knows. `chip` is `0` (unknown), `1` RP2040, `2` RP2350, `3` ESP32-S3, `4` nRF52840, `5` STM32F4.

**Versions.** The firmware and the library of one release speak the same protocol. A host talks to a board whose `fw_major` and `fw_minor` equal its own; `fw_patch` never changes the wire. The first four bytes keep their place in every release, so the host can read them from any board: it lists a board of another major or minor version in `mimhid_discover` with only those values (`firmware_compatible` 0), and refuses to open it with `MIMHID_ERR_PROTOCOL`. A change of the frame format or of any layout in this document raises the minor number.

## Line rate {#line-rate}

There is no table of rates. Only the boot rate is fixed: 115,200 baud (`MIMHID_BOOT_BAUD`), because two parties that have never spoken need one rate in common. Any other rate from there up to the board's `max_baud` (from its UART clock and divider limits, in the `PONG`) can be agreed.

`SET_BAUD` (payload after the sequence number: the rate, `u32` LE) is refused with `NAK_PARAM` for `0`, for a rate above `max_baud`, and for a rate whose divider is off by more than 2 % (`hal_uart_baud_actual`). Otherwise the board sends the `ACK` at the rate it is on, with the rate it will actually run at (`u32` LE), switches once the `ACK` has left, and keeps the new rate only when a valid frame arrives at it within 100 ms (`MIMHID_BAUD_CONFIRM_MS`). Otherwise it goes back to the boot rate. A switch to the boot rate needs no confirmation.

**BREAK.** A UART `BREAK` (the line held low for longer than a character) returns the board to the boot rate whatever rate it is on and drops the bytes it holds. A program that went away and left the board on a fast rate is therefore found without searching: ping at the boot rate, and if nothing answers, send a `BREAK` (10 ms) and ping again.

**Counters and `PROBE`.** The board counts, since the last reset and each saturating at 65535, the frames it dropped for a bad CRC, UART framing errors, bytes the UART lost, and reads that found the receive ring three quarters full or more (a lap of the ring cannot be seen afterwards, so this is the near miss that comes first), and the `PROBE` frames whose payload (byte *i* is `i*7+3` mod 256) was right. A counter the hardware only latches counts once per look: a count is a lower bound, zero is exact. `COUNTERS` (payload: flags, bit 0 = reset after reading) returns the five values as `u16` LE in the `ACK`: `probes_good, bad_crc, framing, uart_overrun, ring_overrun`.

**The host never trusts a rate it has not seen work**, because a USB serial adapter may accept a rate from the operating system and send at another one, and a marginal rate loses bytes rarely:

1. Find the board (above). It is on the boot rate.
2. Try a rate: set the port to it once to see that the driver takes it (on Linux and macOS the driver must report within 3 % of it), send `SET_BAUD` at the boot rate, switch the port, ping three times (the `PONG` names the new rate), then run the link test: `COUNTERS` with reset, a burst of `PROBE` frames back to back, `COUNTERS`. The rate passes when every probe arrived and the loss counters are zero. `mimhid_device_open` does this with a short burst (32 frames of 96 bytes) for the caller's `baud_rate` and fails when it is not carried.
3. When a trial fails, set the port back to the boot rate, send a `BREAK`, and ping: the board is back at once instead of after the 100 ms of step 2's window.
4. `MIMHID_BAUD_FASTEST` and `mimhid_discover` search: from `max_baud` downward in steps of 20 % to the first rate that passes; then between the last failure and that pass by halving to 2 %; then the rate must carry a longer burst (250 ms of line time, up to 60000 frames), else 5 % lower until one does. The boot rate when none passes. `mimhid_discover` reports the result as `baud_rate` and leaves the board on the boot rate.

`mimhid_device_close` sends `SET_BAUD` to the boot rate, so the next program finds the board at once. The host opens a port for its process alone (`TIOCEXCL` on Linux and macOS; Windows ports are exclusive), so a search does not ping a board another program is using.

## Events

| Kind | Code | Data |
|---|---:|---|
| `MOUNTED`, `UNMOUNTED`, `RESUME` | `0x01`, `0x02`, `0x04` | none |
| `SUSPEND` | `0x03` | remote wakeup enabled by the target (`0` or `1`) |
| `SET_REPORT` | `0x05` | interface, report type (`1` Input, `2` Output, `3` Feature), Report ID, bytes |
| `SET_PROTOCOL` | `0x06` | interface, protocol (`0` boot, `1` report) |
| `SET_IDLE` | `0x07` | interface, idle rate |
| `OUTPUT_REPORT` | `0x08` | interface, length (`u16` LE), bytes (interrupt OUT transfer or `SET_REPORT` Output with Report ID `0`) |
| `CONTROL_SETUP` | `0x09` | token, flags, the 8 setup bytes (see [Control transfers](#control-transfers)) |
| `CONTROL_DATA` | `0x0A` | token, the data stage of a host-to-device control transfer |
| `TRANSFER_LOST` | `0x0B` | endpoint address, count (`u16` LE, at most 65535): transfers dropped because the endpoint's queue was full, or that were in flight when the target halted it, since the last such event. Only after `LOSS_CONFIG` enabled it, at most once per its interval |
| `EP_DONE` | `0x0C` | IN endpoint address, count (`u16` LE): transfers completed since the last such event. Only for endpoints with the `WAIT` policy; the host's wait count goes down by it, never below zero. |
| `EP_HALT` | `0x0D` | endpoint address, halted (`1` the target set the Halt feature, `0` cleared it). Only for endpoints of the generic driver. |

The host maps the interface number to a node (for `TRANSFER_LOST`, `EP_DONE` and `EP_HALT`, the endpoint address to the Node that owns it, and reports them as `MIMHID_EVENT_TRANSFER_LOST` and `MIMHID_EVENT_ENDPOINT_HALT`; an `EP_OUT` frame becomes `MIMHID_EVENT_ENDPOINT_DATA` with the endpoint address and interface number) and delivers `mimhid_event` on its reader thread. An unmount sets every wait count to zero.

## Control transfers {#control-transfers}

Besides the reports, the target talks to the board on endpoint 0. Without configuration the board answers it all itself (TinyUSB does, and the HID class driver for the report requests). `CONTROL_CONFIG` changes that, for as long as the host wants:

```text
flags (bit 0: report every control transfer)  rule count n (0..max_control_rules)
n rules of 15 bytes: bmRequestType mask, value  bRequest mask, value  wValue mask, value (2+2)
                     wIndex mask, value (2+2)  timeout in ms (2)  action after the timeout
```

A request matches a rule when each field ANDed with the mask equals the value; the first matching rule holds the request. The set replaces the previous one, the board starts with none, a timeout of 0 or above `MIMHID_CONTROL_TIMEOUT_MAX` (5000, the 5 seconds USB 2.0 9.2.6.1 gives any command), an unknown action, or a value with bits outside its mask is refused with `NAK_PARAM`.

**How it is done.** The board wraps TinyUSB's `dcd_event_handler`, which every controller driver calls with the setup packet in the USB interrupt (`-Wl,--wrap=dcd_event_handler`). A request that matches a rule is not passed on; the USB side reports it (`EVENT CONTROL_SETUP`: token, flags, the 8 bytes) and waits. When the answer comes, or the time runs out, the request is either dropped silently (`DROP`, `3`: the port's `hal_control_silent_drop` disarms endpoint 0 IN and OUT so the controller NAKs until the target times out; only the RP2040 and RP2350 port does this, and on the nRF52840, STM32F4, and ESP32-S3 ports the function is empty) or given to TinyUSB: unchanged for `LEAVE` (`0`, the board's own handling), or with the type bits of `bmRequestType` set to vendor for `ANSWER` (`1`) and `STALL` (`2`), which `tud_vendor_control_xfer_cb` then serves from the host's data (or stalls). The control endpoint's data and status stages therefore run in TinyUSB as for any request, and the controller NAKs until they start.

`CONTROL_REPLY` carries the token, the action (`0` leave, `1` answer, `2` stall, `3` drop) and, for an answer to a device-to-host request, the data stage (sent whole whatever `wLength` says: a longer answer than the host asked for makes it report an overrun, and with `wLength` 0 the data stage is still started, as a real device that ignores a zero length does). It is acknowledged when the board still waits for that token; `NAK_STATE` says it does not (late, or the target gave up and sent another request). For a host-to-device request with a data stage, an `ANSWER` lets the data stage happen; the board then sends `CONTROL_DATA` with the bytes and waits, within the same time, for a second `CONTROL_REPLY` that is the verdict: `ANSWER` or `LEAVE` acknowledges, `STALL` stalls, `DROP` disarms endpoint 0 without a stall, and the rule's action applies when none comes. The USB side of the firmware is busy during that wait; reports queue and go out after it.

Event flags: `0x01` held (`MIMHID_CONTROL_FLAG_HELD`), `0x02` a host-to-device data stage longer than `max_control_data_bytes` (`MIMHID_CONTROL_FLAG_TOO_LONG`, reported, never held), `0x04` standard OUT `SET_ADDRESS` or `SET_CONFIGURATION(1)` (`MIMHID_CONTROL_FLAG_FIXED`, reported, never held: they change the state of the board's own controller and active configuration), `0x08` earlier transfers were not reported because the board's queue of 7 was full (`MIMHID_CONTROL_FLAG_LOST`). Tokens run `1..255`. A new request, a bus reset, a suspend, and a `COMMIT` end a hold.

Limits of what this reaches: the board sees only the setup packet of a transfer it does not hold; the data stage of one it leaves to TinyUSB is not reported.

## Wire time

A report of `n` bytes occupies `n + 5` bytes on the wire, 10 bits each: 3.3 µs per byte at 3,000,000 baud and 0.83 µs at 12,000,000. A 9-byte touch report takes 47 µs at 3,000,000 baud and 12 µs at 12,000,000. The target-facing USB endpoint dominates (see [LATENCY.md](LATENCY.md)).
