# Limits

Where a number is a project choice rather than a USB rule, the table says so.

## Protocol and board

| Item | Limit | Notes |
|---|---|---|
| Nodes | 8 (`MIMHID_MAX_NODES`) | protocol choice; node ids 0..7, 16 addressable |
| USB HID interfaces | `max_interfaces` of the board | 7 on Raspberry Pi Pico (RP2040), Raspberry Pi Pico 2 (RP2350), and nRF52840; 4 on ESP32-S3; 3 on STM32F4 full speed. A build option of the port. |
| Report size | `max_report_bytes`: 64 on a full-speed board | the largest endpoint packet; `mimhid_device_limits` has the figure |
| Endpoint packet | `max_endpoint_bytes`: 64 on a full-speed board, 1024 on a high-speed one | USB: a full-speed interrupt endpoint holds 64 bytes, a high-speed one 1024 |
| All report descriptors together | `max_descriptor_bytes`, 2048 by default | `MIMHID_MAX_DESC_BYTES` at firmware build time |
| Feature answers | 8 per configuration, `max_feature_bytes` each (1024 by default) | `MIMHID_MAX_FEATURES`, `MIMHID_FEATURE_MAX`. A Feature report travels on the control pipe, so it may be longer than an endpoint packet. |
| Strings | printable ASCII, at most 126 characters (`MIMHID_STRING_MAX`) | manufacturer, product, serial, configuration, interface; USB 2.0 9.6.7: a string descriptor is at most 255 bytes of UTF-16LE |
| `usb_config.bmax_power` | 0..250 | in 2 mA units (USB 2.0 9.6.3), so up to 500 mA |
| `ep_in.poll_interval_ms` | 1..255 | Full-Speed interrupt endpoint |
| `ep_in.max_packet_size` | 1 to the board's `max_endpoint_bytes` | IN endpoint packet size |
| `ep_out.max_packet_size` | 1 to the board's `max_endpoint_bytes` | OUT endpoint packet size (`ep_out.address` 0 disables the OUT endpoint) |
| `country_code` | 0..35 | |
| Line rate | one of `MIMHID_BAUD_RATES`, 115,200 to 4,000,000 | the board starts at 115,200 and makes the rates in its `baud_mask` (RP2040 and RP2350 all; ESP32-S3 all; STM32F4 those its USART clock divides within 2 %; nRF52840 up to 1,000,000). The link runs at the fastest rate the board, the operating system and the serial adapter carry, or at `baud_rate` (`MIMHID_ERR_UNSUPPORTED` when it is not carried). |
| Control routing | 8 rules (`max_control_rules`); a data stage of `max_control_data_bytes` (1024 by default, `MIMHID_CONTROL_DATA_MAX`); a time of 1 to 4000 ms per rule | protocol choice; USB 2.0 asks the first data packet of a standard request within 500 ms and allows 5 s for a control transfer (9.2.6.4, 9.2.6.1). Up to 7 control transfers wait in the board's queue to be reported. Standard OUT SET_ADDRESS is never held. |
| Queue per interface | 15 reports waiting (7 when a report may be longer than 64 bytes); `mimhid_node_set_queue_depth` makes it shorter | a full queue drops the newest report; `mimhid_device_set_loss_reporting` tells the program how many |

The board's numbers are fixed when its firmware is built, at the most the chip can hold, and the board reports them in its PONG. The host library cannot raise them. `MIMHID_MAX_ENDPOINT_BYTES`, `MIMHID_MAX_REPORT_BYTES` and `MIMHID_FEATURE_MAX` (`firmware/common/config.h`) can be set by a port before the header is read; the endpoint size follows the port's USB speed.

Each Node names its interface with `iface.number`; nothing is numbered for it. Nodes with the same number share the interface, and must then all have Report IDs, all different, and the same interface and endpoint settings; Nodes on different interfaces need different endpoint addresses (`MIMHID_ERR_PARAM` otherwise). `mimhid_node_open` and `mimhid_node_open_raw` return `MIMHID_ERR_OVERFLOW` when the board's interface or descriptor limits are exceeded.

## Host library

Limits of each profile (axes, contact slots, usages, Report ID rules) are those of `libaoahid`, validated by the `mimhid_spec_create_*` calls; the options structures in `mimhid.h` document each field. The Linux HID parser limits in `mimhid_device_config.transport.linux_hid` are the caller's and must be nonzero; `mimhid_device_config_init` fills in the values of the kernel source below, and `mimhid_node_open` refuses a Spec that exceeds one with `MIMHID_ERR_OVERFLOW`.

## Linux HID parser

`transport.linux_hid` describes the HID parser of a Linux target. `mimhid_device_config_init` fills in the values of one kernel revision, the one [libaoahid's LIMITS.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/LIMITS.md#linux-hid-values) cites (Android common kernel commit `35556bed836f8dc07ac55f69c8d17dce3e7f0e25`); other kernels may differ, so check the same symbols in the kernel you target. Each Spec records what it needs, using checked arithmetic without expanding Usage ranges.

| Field | Initial value | Kernel source |
|---|---:|---|
| `descriptor_bytes` | 4096 | `HID_MAX_DESCRIPTOR_SIZE`, `include/linux/hid.h` |
| `fields_per_report` | 256 | `HID_MAX_FIELDS`, `hid_register_field` |
| `global_stack_depth` | 4 | `HID_GLOBAL_STACK_SIZE`, `hid_parser_global` |
| `report_size_bits` | 256 | the Report Size check of `hid_parser_global` |
| `usages` | 12288 | `HID_MAX_USAGES`, `hid_parser_global` |
| `report_data_bits` | 65528 | `(HID_MAX_BUFFER_SIZE - 1) << 3`, `hid_add_field` |

Windows and macOS parse descriptors with their own limits; these fields do not describe them.

## Invariants

1. Multi-byte report fields are little-endian.
2. Input calls do not allocate after the node is open.
3. A node close sends the neutral report first, so held keys and contacts are released.
4. Calls on a node return `MIMHID_ERR_BUSY` while the board is re-enumerating, and on a node opened in a batch until the batch ends.
