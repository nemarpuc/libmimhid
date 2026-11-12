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
| `0x10`..`0x1B`, `0x1D`..`0x22` | host to board | Commands. The payload starts with a one-byte sequence number, which the board echoes in its `ACK` or `NAK`. The host counts it up by one per command and lets it wrap. |
| `0x1C` | host to board | `PROBE`: link-test frame. No sequence number, no acknowledgement. |
| `0x41`..`0x4F` | host to board | `EP_IN`: data for the IN endpoint number `TYPE & 0x0F` (`MIMHID_T_EP_IN_BASE | number`). The payload is one whole transfer, which for a HID interface is the report exactly as the target receives it, Report ID included (a Report ID is only the first byte: the board reads no class) (at most `max_transfer_bytes`; empty is a zero-length packet, USB 2.0 5.8.3; a frame that is not the last of a longer transfer is a multiple of the packet size). No acknowledgement. A frame for an endpoint that is not an IN endpoint of the active configuration is dropped. `0x40` (endpoint 0) is refused: the control endpoint is not a data endpoint. |
| `0xC1`..`0xCF` | board to host | `EP_OUT`: a packet the target sent on the OUT endpoint number `TYPE & 0x0F`, as it came (an Output report of a HID interface included) (`MIMHID_T_EP_OUT_BASE | number`). No acknowledgement. The host delivers it as `MIMHID_EVENT_ENDPOINT_DATA`, or, for a profile Node's OUT endpoint, as `MIMHID_EVENT_OUTPUT_REPORT`. Packets of one endpoint arrive in order. |
| `0x81` | board to host | `PONG` |
| `0x82` / `0x83` | board to host | `ACK` (`seq, type`, then the data of the command if it has any) / `NAK` (`seq, type, code`) |
| `0x84` | board to host | `EVENT` (`kind, ...`) |

An `EP_IN` payload is at most the board's `max_report_bytes`. Other payloads are at most the board's `max_frame_payload`, never below 255 (a `DESC_DATA` chunk fits; the firmware refuses to build with a limit below its longest configuration command) and never above `MIMHID_PROTO_MAX_PAYLOAD` (4096). The board says both in its `PONG`.

## Commands

| Type | Name | Payload after the sequence number |
|---|---|---|
| `0x10` | `DESC_RESET` | None. Starts a new staged configuration: forgets every staged descriptor, string, control rule and endpoint setting. The active configuration is not touched. |
| `0x11` | `DESC_BEGIN` | `kind` (1 device, 2 configuration, 3 string, 5 the bytes of an `ANSWER` rule; 4 was the HID report descriptor), `index`, language id (u16 LE), total length (u16 LE), `flags`. Announces one descriptor of `total` bytes, which `DESC_DATA` then sends. Device: index 0, language 0, 18 bytes (`NAK_OVERFLOW` above 18; a shorter one is refused at `COMMIT`). Configuration: index 0, language 0, at most `MIMHID_MAX_CONFIG_BYTES`. String: index 1..255, a language id, an even 2..254 bytes (USB 2.0 9.6.7); index 0 is never sent, the board answers it with the language ids of the strings it holds. Flag bit 0 (`MIMHID_DESC_F_SERIAL_FROM_ID`): total 0, the board makes the string from its chip id (`NAK_UNSUPPORTED` on a chip without one). `ANSWER` bytes: index the rule number, language 0, flags 0, the total length the rule was staged with (`NAK_PARAM` otherwise, `NAK_STATE` when the rule is not an `ANSWER` rule staged before). `NAK_PARAM` for an index, language, flag or length the kind does not take; `NAK_OVERFLOW` when the descriptor, the string table (`MIMHID_MAX_STRINGS` strings, whatever their language) or the string store (`MIMHID_MAX_STRING_BYTES`) cannot hold it. The same announcement repeated is acknowledged again; another length or flag for a descriptor already announced is `NAK_STATE`. |
| `0x12` | `DESC_DATA` | `kind`, `index`, language id (u16 LE), offset (u16 LE), up to 240 bytes (`MIMHID_DESC_DATA_CHUNK`) of the announced descriptor. A chunk may start only at or before the end of the bytes received so far (`NAK_PARAM` for a gap or for bytes past the announced length); a chunk repeated, same offset and bytes, is acknowledged again. `NAK_STATE` for a descriptor that was not announced, and for a string the board makes itself. |
| `0x13` | `COMMIT` | Three bytes: detach flag, then two bytes the firmware ignores (the host sends them as zero on attach, and as `reconnect_delay_ms` on the detach that follows the last node's close). Attach (`0`): walk the staged configuration and check the staged rules (see [The commit walk](#the-commit-walk) and [Control rules](#control-rules); `NAK_DESCRIPTOR` for the configuration and `NAK_STATE` for a rule whose bytes are not all there or whose endpoint is not an IN endpoint of it, the previous configuration and rules kept), drop queued reports, apply the queue depths, make the staged rules the active ones (flag 0: nothing is reported beyond what the rules say), clear held control transfers, call `tud_connect()`, and `ACK`. Detach (`1`): call `tud_disconnect()`, drop queued reports, clear held control transfers, the active configuration and the active rules, and `ACK`. |
| `0x14` | (unused) | Was `FEATURE`, which set the answer to a Feature `GET_REPORT`. The board has no HID driver any more: that answer is the data of an `ANSWER` rule. The number is not reused in this release. |
| `0x15` | `RESET` | Drop every queued report across all endpoints, and forget what the `ANSWER_LAST_SENT` rules hold. |
| `0x16` | `REMOTE_WAKEUP` | Signal USB remote wakeup (`NAK_STATE` when the target has not suspended the device). |
| `0x17` | `SET_BAUD` | Requested baud rate (`u32` LE; see [Line rate](#line-rate)); `NAK_PARAM` for `0`, a rate above `max_baud`, or a rate whose divider error exceeds 2 %. Handled on the receive side, like `PING`; `ACK` carries the actual baud rate (`u32` LE). |
| `0x18` | `RULE` | Stages one control rule: its number, the four masked fields of the setup packet, the action and what the action needs (see [Control rules](#control-rules)). |
| `0x19` | `CONTROL_REPLY` | The answer to a held control transfer: `token`, `action` (`0` leave, `1` answer, `2` stall, `3` drop: `MIMHID_REPLY_*`), and optional data bytes. Handled on the receive side, like `PING` and `SET_BAUD`; the board replies with `ACK` (or `NAK_STATE` if the hold already expired), though the host's `Link::send_control_reply` writes the frame without waiting so it can be called inside `event_sink`. |
| `0x1A` | `LOSS_CONFIG` | Enabled (`0` or `1`), and the minimum interval between two `TRANSFER_LOST` events in ms (u16 LE; `0` while disabled). The board starts with it off, and then counts and sends nothing for it. |
| `0x1B` | `EP_CONFIG` | Endpoint address, depth, policy. The address is that of an IN endpoint of the staged configuration (`NAK_STATE` when there is none or it lacks that endpoint; `NAK_PARAM` for an address that is not `0x81..0x8F`; `NAK_UNSUPPORTED` for an OUT address). The depth is how many transfers may wait on the endpoint, the one the controller is sending not counted (`NAK_PARAM` for `0`, `NAK_OVERFLOW` beyond what the queue holds). The policy is what happens when that many wait: `MIMHID_EP_FULL_DROP_NEWEST` (`1`) drops the transfer that finds the queue full, `DROP_OLDEST` (`2`) drops the oldest that is not being sent, `WAIT` (`3`) is `DROP_NEWEST` on the board: the host counts and sends no more than the depth, and learns what the board finished from `EV_EP_DONE`. Any other value is `NAK_PARAM`. A queue is cut from the board's pool of `endpoint_queue_slots` (`depth + 1` slots for `DROP_NEWEST` and `WAIT`, `2 * depth + 1` for `DROP_OLDEST`; `NAK_OVERFLOW` beyond the pool). The host sends one for every IN endpoint, HID interfaces' included, before the attach `COMMIT`; one is required (`NAK_DESCRIPTOR` at the commit when missing), and it takes effect at once on a running configuration when the new queue fits. |
| `0x1C` | `PROBE` | Link-test payload (no sequence number, no `ACK`; byte `i` is `(i * 7 + 3) & 0xFF`). Verified on the receive side and counted in `probes_good` / `bad_crc`. |
| `0x1D` | `COUNTERS` | Flags byte (bit 0: reset counters after reading). Handled on the receive side; `ACK` carries five `u16` LE counters (`probes_good`, `bad_crc`, `framing`, `uart_overrun`, `ring_overrun`). |
| `0x1E` | `EP_STALL` | Endpoint address, then `1` to stall or `0` to clear the Halt feature (USB 2.0 9.4.5): the endpoint answers STALL until it is cleared by this command or by the target; stalling an IN endpoint drops what waits and the transfer being sent. `NAK_STATE` when the address is not an endpoint of the active configuration, `NAK_PARAM` for a bad flag. |
| `0x1F` | `EP_FLUSH` | IN endpoint address: drops what waits on the board; a transfer the controller already has completes. `NAK_STATE` as for `EP_STALL`, `NAK_PARAM` for an OUT endpoint. |
| `0x20` | `BOOTLOADER` | The four bytes `BOOT`. The board takes its USB device off the target, acknowledges, and restarts into the chip's own bootloader, where new firmware is written (the RP2040 and RP2350; the other ports answer `NAK_UNSUPPORTED`). `NAK_PARAM` for another payload. |
| `0x21` | `RULES_APPLY` | Flags (bit 0: report every control transfer), rule count `n`. Makes the first `n` staged rules the active ones without a `COMMIT` (see [Control rules](#control-rules)). `NAK_PARAM` when `n` is more than the staged rules, `NAK_STATE` when an `ANSWER` rule lacks bytes or an `ANSWER_LAST_SENT` rule names an endpoint that is not an IN endpoint of the active configuration, `NAK_BUSY` while the board answers a control transfer from the active answer memory. |
| `0x22` | `RULE_DATA` | Rule number, then the new bytes of an active `ANSWER` rule, at most as many as it was staged with. `NAK_PARAM` for another rule or a longer answer, `NAK_BUSY` while the board answers from that rule. |

Field layouts of `DESC_BEGIN`, `DESC_DATA`, `RULE`, `RULES_APPLY`, `RULE_DATA`, `EP_CONFIG`, `EP_STALL` and `EP_FLUSH` are documented in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h).

The board acknowledges each command (`0x10..0x1B`, `0x1D..0x22`). The host waits the call's `control_timeout_ms` (no default; every call that sends a configuration frame takes one) and sends the same frame, with the same sequence number, up to three times (`kCommandAttempts` in `src/api/support.hpp`). The attach and detach `COMMIT` that `Link::connect` and `Link::disconnect` send go once. The board keeps the answer to the last command the USB side ran: a command arriving again with that sequence number and type is answered again and not run twice (a `COMMIT` does not re-enumerate, a `RESET` drops nothing more), and an answer that was only late still answers the host. A `PING` makes the board forget it, since a new host starts its numbers anew. NAK codes: `PARAM` (`1`), `OVERFLOW` (`2`), `STATE` (`3`), `DESCRIPTOR` (`4`), `UNSUPPORTED` (`5`), `BUSY` (`6`); the host maps them to `MIMHID_ERR_*`.

### Applying a configuration

`commit_device` (`src/api/device_ops.cpp`) does this on every commit:

1. When nodes are left, the host composes every descriptor (`src/usb/descriptor_composer.cpp`) and the control rules (`build_rule_table`: the rules of the Nodes in node order, among them those `src/usb/hid_layer.cpp` writes for each profile interface, then the Device's own) and sends `DESC_RESET`, then the device descriptor, the configuration descriptor and the string descriptors, each as a `DESC_BEGIN` and `DESC_DATA` chunks, then every rule as a `RULE` frame with, for an `ANSWER` rule, its bytes as a `DESC_BEGIN` and `DESC_DATA` chunks of the kind `ANSWER`, and an `EP_CONFIG` for every IN endpoint (a HID interface gets one command at least as deep as its Nodes' least depth). The board stores them; the target sees no change yet. The host resends the whole configuration every time a node opens or closes. When no node is left, the host sends only a detach `COMMIT`, and the steps below are skipped.
2. If the board's USB device is on the target, the host sends a detach `COMMIT` (`Link::disconnect`, waiting `control_timeout_ms`), and then waits `reconnect_delay_ms` when `mimhid_device_commit` was given one.
3. The host sends an attach `COMMIT` (`Link::connect`) and waits up to `reconnect_timeout_ms` for its `ACK`. The board walks the staged configuration (`NAK_DESCRIPTOR` when it does not hold together), checks the staged rules, makes both the active ones, drops queued reports, calls `tud_connect()`, and sends `ACK`. The host returns on that `ACK`. The rules are then active with flag 0; when the Device asked to report every control transfer (`report_all`), the host sends `RULES_APPLY` with the flag set and the rule count straight after (it is the library, not the commit, that applies `report_all`).
4. When the target configures the device (`SET_CONFIGURATION(1)`), the board sends `EVENT MOUNTED`. `mimhid_device_wait_ready` waits for it; no commit does.

The receive side of the firmware answers `PING`, `SET_BAUD`, `COUNTERS`, `PROBE`, and `CONTROL_REPLY` itself. It puts every other command in a mailbox (`g_cmd`, `cmd_push`: a ring of four slots that holds three commands; a full mailbox drops the frame, and the host sends it again when no answer comes), and the USB side runs it (`run_command`, from `mimhid_usb_poll`). Staging and applying a configuration therefore both happen on the USB side, so on a two-core MCU no core changes a descriptor that the other is serving.

## The commit walk {#the-commit-walk}

The host composes the USB descriptors; the board takes them as bytes and serves them to the target without changing them. At `COMMIT` it walks the staged configuration once and refuses it with `NAK_DESCRIPTOR`, keeping the previous configuration (the target sees no change and nothing is disconnected), unless all of this holds:

- **Device descriptor** (USB 2.0 9.6.1): complete, 18 bytes, `bLength` 18, type 1, `bcdUSB` not zero, one configuration, a control endpoint size of 8, 16, 32 or 64 (only 64 on a high-speed board; the board's endpoint 0 takes the size from byte 7 of this descriptor).
- **Configuration descriptor** (9.6.3): complete, `bLength` 9, type 2, `wTotalLength` equal to the bytes held, `bConfigurationValue` 1 (the stack maps `SET_CONFIGURATION(1)` to the one configuration), `bmAttributes` with bit 7 set and bits 4..0 clear, `bNumInterfaces` equal to the interfaces found.
- **Every descriptor inside it**: a `bLength` of at least 2 that stays inside the blob, and only interface, HID, and endpoint descriptors.
- **Interfaces** (9.6.5): numbered 0, 1, 2, ... in order, alternate setting 0 only; at most `max_interfaces_any` of them. The board reads no class: every interface, HID (`0x03`) included, is served by the generic driver and has any number of interrupt or bulk endpoints, with the descriptors the host put between the interface and its first endpoint (a HID descriptor, HID 1.11 6.2.1, among them) taken as given, each with a `bLength` of at least 2 inside the blob. `bNumEndpoints` equals the endpoints found. An interface association descriptor (USB ECN, `bLength` 8, type `0x0B`) may stand only before the first interface of a group and names that many consecutive ones.
- **Endpoints** (9.6.6): interrupt or bulk (a type listed in `transfer_types`); the endpoint number from 1 to `max_endpoint_number`, bits 6..4 of the address clear; no address used twice; packet sizes from 1 to `max_endpoint_bytes` (bulk at full speed 8, 16, 32 or 64, at high speed 512, USB 2.0 5.8.3); an interrupt `bInterval` of at least 1, and from 1 to 16 on a high-speed board. Every IN endpoint has an `EP_CONFIG` staged.
- **Rules** (see [Control rules](#control-rules)): every `ANSWER` rule has all its bytes, and every `ANSWER_LAST_SENT` rule names an IN endpoint of the configuration (`NAK_STATE` otherwise, at the `COMMIT` that applies them). The board does not look for a report descriptor: the host answers `GET_DESCRIPTOR` for it with an `ANSWER` rule.
- **Totals**: configuration descriptor at most `max_config_descriptor_bytes`, strings at most `max_strings`, queue slots at most `endpoint_queue_slots`.
- **Strings** (9.6.7): complete, `bLength` equal to the bytes held, type 3.

From the walk the board keeps which endpoints each interface has. An `EP_IN` frame is queued on the endpoint it names, and an event names an endpoint by its address.

## Interfaces and nodes

The interface number (`mimhid_usb_interface.number`, `mimhid_hid_node_config.interface_number`) is the `bInterfaceNumber` the target sees. The host composes the configuration descriptor with the interfaces of all Nodes in number order, and the numbers in use must be 0, 1, 2, ... without a gap: otherwise the commit fails with `MIMHID_ERR_PARAM`, nothing is sent, and `mimhid_error_detail.offset` is the first number that is missing. Nothing is renumbered. Profile Nodes with the same number share a HID interface: the host joins their report descriptors in node order (the answer of one `ANSWER` rule), and requires that they all have Report IDs, all different, and the same interface and endpoint settings (the queue depth excepted). The host sends the board no node: a report goes to the endpoint of its Node's interface (`EP_IN | (ep_in.address & 0x0F)`), so a Device holds as many Nodes as it likes (up to 65534) within the board's interface and descriptor limits. The caller's strings keep their index from 5 up (`mimhid_device_add_string`); an interface name takes the lowest index above 4 not taken, in interface order.

## PONG

```text
0 fw_major  1 fw_minor  2 fw_patch  3 ready
4 reserved, zero  5 reserved, zero  6 flags (bit 0: chip unique id available)
7 chip  8 usb speed (0 full, 1 high)
9..12 the line rate the board is on (u32)
13..16 max_baud, the fastest rate its UART can make (u32)
17..20 uart flags (u32): bit 0 the board returns to the boot rate on a BREAK
21..22 reserved, zero  23..24 max_report_bytes  25..26 reserved, zero
27..28 max_endpoint_bytes  29..30 max_frame_payload  31..32 max_control_data_bytes
33 max_control_rules (the table: the rules of every Node and of the Device)  34 max_endpoint_number (the highest endpoint number the controller has)
35 unique id length n (0..16)
36 max_interfaces_any (interfaces of all classes)  37..38 max_config_descriptor_bytes
39 max_strings  40..41 max_transfer_bytes  42..43 endpoint_queue_slots
44 transfer_types (bit 2 bulk, bit 3 interrupt)
45..46 max_answer_bytes (the answer memory: the bytes of every ANSWER rule together)
47.. the chip's unique id, n bytes
```

All wider values are little endian. The length is `47 + n` (the minimum, `MIMHID_PONG_MIN_LEN`, is 47). (Version 0.2.0 added byte 34 and moved the unique id; version 0.3.0 added bytes 36 to 46 and moved it again, and zeroed the bytes 5, 21..22 and 25..26 that no longer mean anything; the length byte stays at 35.) The host reads `max_control_data_bytes` as the largest Feature answer and the answer memory as the largest report descriptor and Feature answers together. A reader ignores bytes past the ones it knows. `chip` is `0` (unknown), `1` RP2040, `2` RP2350, `3` ESP32-S3, `4` nRF52840, `5` STM32F4.

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
| (unused) | `0x05`..`0x08` | Were `SET_REPORT`, `SET_PROTOCOL`, `SET_IDLE` and `OUTPUT_REPORT`, which the board's HID driver made. A HID request is now a control transfer taken by an `ACCEPT` rule, and an Output report on an OUT endpoint is an `EP_OUT` frame. The numbers are not reused in this release. |
| `CONTROL_SETUP` | `0x09` | token, flags, the 8 setup bytes (see [Control rules](#control-rules)) |
| `CONTROL_DATA` | `0x0A` | token, the data stage of a host-to-device control transfer (held, or taken by an `ACCEPT` rule) |
| `TRANSFER_LOST` | `0x0B` | endpoint address, count (`u16` LE, at most 65535): transfers dropped because the endpoint's queue was full, or that were in flight when the target halted it, since the last such event. Only after `LOSS_CONFIG` enabled it, at most once per its interval |
| `EP_DONE` | `0x0C` | IN endpoint address, count (`u16` LE): transfers completed since the last such event. Only for endpoints with the `WAIT` policy; the host's wait count goes down by it, never below zero. |
| `EP_HALT` | `0x0D` | endpoint address, halted (`1` the target set the Halt feature, `0` cleared it). |

The host maps the endpoint address to the Node that owns it (for `TRANSFER_LOST`, `EP_DONE` and `EP_HALT`, the endpoint address to the Node that owns it, and reports them as `MIMHID_EVENT_TRANSFER_LOST` and `MIMHID_EVENT_ENDPOINT_HALT`; an `EP_OUT` frame becomes `MIMHID_EVENT_ENDPOINT_DATA` with the endpoint address and interface number) and delivers `mimhid_event` on its reader thread. An unmount sets every wait count to zero.

## Control rules {#control-rules}

Besides the reports, the target talks to the board on endpoint 0. Every request it sends (the setup packet of a control transfer, USB 2.0 9.3) is looked up in a table of rules the host wrote. The first rule that matches decides what the board does; a request no rule matches is left to the board's USB stack, which answers the standard requests it can (`SET_ADDRESS`, `SET_CONFIGURATION`, `GET_DESCRIPTOR` of the device, configuration and string descriptors, `GET_STATUS`) and stalls the rest (USB 2.0 9.2.7). The board has no HID driver, so everything a HID interface needs on endpoint 0 (HID 1.11 7.1.1 `GET_DESCRIPTOR` of the report and HID descriptors, 7.2.1 `GET_REPORT`, 7.2.2 `SET_REPORT`, 7.2.3 to 7.2.6 the idle rate and the protocol) is a rule that the host's profile layer writes (`compose_hid_rules` in `src/usb/hid_layer.cpp`), or the caller writes for an interface of its own (`examples/c/hid_interface.c`). The board starts with no rules, and stalls every class and vendor request until it has some.

```text
RULE:   0 rule number  1 bmRequestType mask  2 value  3 bRequest mask  4 value
        5..6 wValue mask  7..8 wValue value  9..10 wIndex mask  11..12 wIndex value
        13 action, then by action:
          ANSWER            14..15 length of the answer (u16 LE); the bytes follow as DESC_BEGIN
                            and DESC_DATA of the kind ANSWER with the rule number as index
          ANSWER_LAST_SENT  14 IN endpoint address  15 filter (0 or 1)  16 first byte
          ASK               14..15 timeout in ms (1..5000)  16 action after it (stall 2, drop 3)
          ACCEPT, STALL     nothing more
```

A request matches a rule when each of the four fields ANDed with its mask equals the value. The rules are numbered from 0 in the order they are tried. The staged table is a stack: a `RULE` with number `i` (at most the number of rules staged) replaces rule `i` and forgets every rule after it, so the host writes the table in order; `DESC_RESET` forgets it. The table holds at most `max_control_rules` rules (`MIMHID_CONTROL_RULES`, 160) and the `ANSWER` bytes together at most `max_answer_bytes` (`MIMHID_ANSWER_BYTES`, 4096); more is `NAK_OVERFLOW`. `NAK_PARAM`: a value outside its mask, an unknown action, an `ANSWER` or `ANSWER_LAST_SENT` rule for a request that is not device to host (bit 7 of `bmRequestType`, USB 2.0 9.3.1, set in both mask and value), an `ASK` without its timeout or with another action after it, an `ANSWER_LAST_SENT` whose endpoint is not an IN address (USB 2.0 9.6.6). More than `MIMHID_LAST_SENT_SLOTS` (32) `ANSWER_LAST_SENT` rules is `NAK_OVERFLOW`.

**Actions.**

| Action | What the board does |
|---|---|
| `ANSWER` (1) | Returns the rule's bytes, cut to `wLength`, straight from the answer memory. |
| `ANSWER_LAST_SENT` (2) | Returns the last transfer the board started on the IN endpoint (with the filter 1: the last one whose first byte is the given one, which is how a Report ID is told), cut to `wLength`; stalls when there is none since the configuration was committed. The board keeps the transfer as it starts it in a buffer of `MIMHID_MAX_TRANSFER_BYTES` that belongs to the rule. That buffer is written and read on the USB side only (from `tud_task` and `mimhid_usb_poll`, never at once); the answer is copied from it into the control answer buffer when the setup stage is handled, so a transfer started while a multi-packet answer is going out cannot change the bytes of that answer. |
| `ACCEPT` (3) | Takes the data stage (at most `MIMHID_CONTROL_DATA_MAX`, 1024 bytes) and completes the transfer; the host is told with `CONTROL_SETUP` (flag `0x10`, accepted) and, when there was a data stage, `CONTROL_DATA`. Nothing is awaited. |
| `ASK` (4) | Holds the transfer and asks the host (`CONTROL_SETUP`, flag `0x01`), which answers with `CONTROL_REPLY` within the timeout; after it the board does the rule's action (stall or drop). |
| `STALL` (5) | Stalls the request (USB 2.0 8.5.3: a control endpoint answers its status stage with STALL). |

`RULES_APPLY` makes the first `n` staged rules the active ones without a `COMMIT`: the two tables are swapped with the USB interrupt masked, a transfer held under the old table goes to the stack as if no rule had matched, and the rules after `n` are forgotten. An `ANSWER_LAST_SENT` rule that was not changed keeps what it holds; a `COMMIT` starts all of them empty. It is refused with `NAK_BUSY` while the board is answering a control transfer from the active answer memory (the host asks again). It may also come while a fresh configuration is staged but not committed: the staged table is the same one, so those rules are what the next `COMMIT` applies,, and the host is responsible for the order of its frames. `RULE_DATA` changes the bytes of an active `ANSWER` rule in place (the staged copy too, so that a later `COMMIT` keeps them); the rule then answers with that many bytes, at most what it was staged with, so a rule that must change length is staged with the longest. It is refused with `NAK_BUSY` while the board is answering from that rule, so a reader of the answer never sees half old and half new bytes: the answer memory is written by the receive side and read by the USB interrupt, and the two are kept apart by that check.

A `COMMIT` applies all the staged rules with the descriptors and with flag 0; the flag of `RULES_APPLY` (bit 0) asks for a `CONTROL_SETUP` event for every transfer, whatever its rule.

**How a held transfer is done.** The board wraps TinyUSB's `dcd_event_handler`, which every controller driver calls with the setup packet in the USB interrupt (`-Wl,--wrap=dcd_event_handler`). A request that matches an `ASK` rule is not passed on; the USB side reports it (`EVENT CONTROL_SETUP`: token, flags, the 8 bytes) and waits. When the answer comes, or the time runs out, the request is either dropped silently (`DROP`, `3`: the port's `hal_control_silent_drop` disarms endpoint 0 IN and OUT so the controller NAKs until the target times out; only the RP2040 and RP2350 port does this, and on the nRF52840, STM32F4, and ESP32-S3 ports the function is empty) or given to TinyUSB with the type bits of `bmRequestType` set to vendor, which `tud_vendor_control_xfer_cb` then serves from the host's data, from the rule's bytes, from the last sent transfer, by accepting, or by stalling. The control endpoint's data and status stages therefore run in TinyUSB as for any request, and the controller NAKs until they start. A request taken by an `ANSWER`, `ANSWER_LAST_SENT`, `ACCEPT` or `STALL` rule is not held: the board answers it by itself (the answer is made from the retyped request in the vendor callback).

`CONTROL_REPLY` carries the token, the action (`0` leave, `1` answer, `2` stall, `3` drop: `MIMHID_REPLY_*`) and, for an answer to a device-to-host request, the data stage (sent whole whatever `wLength` says: a longer answer than the host asked for makes it report an overrun, and with `wLength` 0 the data stage is still started, as a real device that ignores a zero length does). `leave` hands the request to the board's own handling, as if no rule had held it. It is acknowledged when the board still waits for that token; `NAK_STATE` says it does not (late, or the target gave up and sent another request). For a host-to-device request with a data stage, an `ANSWER` lets the data stage happen; the board then sends `CONTROL_DATA` with the bytes and waits, within the same time, for a second `CONTROL_REPLY` that is the verdict: `ANSWER` or `LEAVE` acknowledges, `STALL` stalls, `DROP` disarms endpoint 0 without a stall, and the rule's action applies when none comes. The USB side of the firmware is busy during that wait; reports queue and go out after it.

Event flags: `0x01` held (`MIMHID_CONTROL_FLAG_HELD`), `0x02` a host-to-device data stage longer than `max_control_data_bytes` (`MIMHID_CONTROL_FLAG_TOO_LONG`, reported, never held), `0x04` standard OUT `SET_ADDRESS` or `SET_CONFIGURATION(1)` (`MIMHID_CONTROL_FLAG_FIXED`, reported, never held: they change the state of the board's own controller and active configuration), `0x08` earlier transfers were not reported because the board's queue of 7 was full (`MIMHID_CONTROL_FLAG_LOST`), `0x10` an `ACCEPT` rule took it (`MIMHID_CONTROL_FLAG_ACCEPTED`; a `CONTROL_DATA` with this token follows when it had a data stage). Rules other than `ACCEPT` and `ASK` are reported only when every transfer is. Tokens run `1..255`. A new request, a bus reset, a suspend, and a `COMMIT` end a hold.

**What the host makes of an accepted request.** For a profile interface the host classifies an `ACCEPT` event by the first rule of its table that matches the setup packet: `SET_REPORT` becomes `MIMHID_EVENT_OUTPUT_REPORT` or `MIMHID_EVENT_FEATURE_REPORT` when the data stage has arrived (not at the acknowledgement: the bytes are what the event carries; the Report ID in the data stage is stripped, HID 1.11 7.2.2), `SET_IDLE` and `SET_PROTOCOL` become `MIMHID_EVENT_SET_IDLE` and `MIMHID_EVENT_SET_PROTOCOL` (the values come from `wValue`: HID 1.11 7.2.4 and 7.2.6), and none of these reaches the program as a `CONTROL_SETUP`. An Output report on a profile Node's OUT endpoint becomes `MIMHID_EVENT_OUTPUT_REPORT` too. The answers to `GET_IDLE` and `GET_PROTOCOL` are one-byte `ANSWER` rules whose bytes the host changes with `RULE_DATA` as the target sets them, from a thread of the library's own (the reader thread must not wait for the board); until it has run, a `GET_IDLE` may still return the old value. The host resets them to the idle rate 0 and the report protocol 1 when the target mounts the device (HID 1.11 7.2.4, 7.2.6). Limits of what this reaches: the board sees only the setup packet of a transfer it does not hold; the data stage of one it leaves to TinyUSB is not reported.

## Wire time

A report of `n` bytes occupies `n + 5` bytes on the wire, 10 bits each: 3.3 µs per byte at 3,000,000 baud and 0.83 µs at 12,000,000. A 9-byte touch report takes 47 µs at 3,000,000 baud and 12 µs at 12,000,000. The target-facing USB endpoint dominates (see [LATENCY.md](LATENCY.md)).
