# Wire protocol (version 2)

The constants live in [`proto/mimhid_proto.h`](../proto/mimhid_proto.h), which both the host library and the firmware include. This page describes how they are used. The link is UART, 8N1, 921,600 baud by default.

## Frame

```
A5  TYPE  LEN  payload[LEN]  XOR
```

`XOR` is `TYPE ^ LEN ^` every payload byte. A frame with a bad checksum or an unknown length is dropped and the parser looks for the next `A5`. A parser that stops mid-frame for 10 ms starts over, so a damaged length byte cannot stall the link.

| Range | Direction | Meaning |
|---|---|---|
| `0x01` | host to board | `PING`. Payload: the host's protocol version. No sequence number. |
| `0x10`..`0x16` | host to board | Commands. The payload starts with a sequence number. |
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
```

The host rejects a board whose `protocol_version` differs from its own with `MIMHID_ERR_PROTOCOL`. Version 1 (libmimhid 0.1 and 0.2) is not understood.

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

A report of `n` bytes occupies `n + 4` bytes on the wire: 10 bits each at 921,600 baud, that is 10.9 µs per byte. A 9-byte touch report takes 141 µs.
