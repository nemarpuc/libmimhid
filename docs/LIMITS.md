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
| Strings | printable ASCII, at most 31 characters | manufacturer, product, serial, configuration, interface |
| `max_power_ma` | 0..500 | written in 2 mA units |
| `poll_interval_ms` | 1..255 | Full-Speed interrupt endpoint |
| `ep_in_size` | 1 to the board's `max_endpoint_bytes` | IN endpoint packet size (typical 64) |
| `ep_out_size` | 0, or 1 to the board's `max_endpoint_bytes` | OUT endpoint packet size (`0` disables OUT endpoint) |
| `country_code` | 0..35 | |
| Line rate | one of `MIMHID_BAUD_RATES`, 115,200 to 4,000,000 | the board starts at 115,200 and makes the rates in its `baud_mask` (RP2040 and RP2350 all; ESP32-S3 all; STM32F4 those its USART clock divides within 2 %; nRF52840 up to 1,000,000). The link runs at the fastest rate the board, the operating system and the serial adapter carry, or at `baud_rate` (`MIMHID_ERR_UNSUPPORTED` when it is not carried). |
| Control routing | 8 rules (`max_control_rules`); a data stage of `max_control_data_bytes` (1024 by default, `MIMHID_CONTROL_DATA_MAX`); a time of 1 to 4000 ms per rule | protocol choice; USB 2.0 asks the first data packet of a standard request within 500 ms and allows 5 s for a control transfer (9.2.6.4, 9.2.6.1). Up to 7 control transfers wait in the board's queue to be reported. Standard OUT SET_ADDRESS and SET_CONFIGURATION are never held. |
| Queue per interface | 16 reports (8 when a report may be longer than 64 bytes) | a full queue drops the newest report |

The board's numbers are fixed when its firmware is built, at the most the chip can hold, and the board reports them in its PONG. The host library cannot raise them. `MIMHID_MAX_ENDPOINT_BYTES`, `MIMHID_MAX_REPORT_BYTES` and `MIMHID_FEATURE_MAX` (`firmware/common/config.h`) can be set by a port before the header is read; the endpoint size follows the port's USB speed.

In `libmimhid`, each node explicitly defines its USB `interface_number` (no auto-assignment). `mimhid_node_open` and `mimhid_node_open_raw` return `MIMHID_ERR_OVERFLOW` when the board's interface or descriptor limits are exceeded.

## Host library

Limits of each profile (axes, contact slots, usages, Report ID rules) are those of `libaoahid`, validated by the `mimhid_spec_create_*` calls; the options structures in `mimhid.h` document each field. The Linux HID parser limits in `mimhid_device_options` (`linux_*_policy`) are caller policy and must be nonzero.

## Invariants

1. Multi-byte report fields are little-endian.
2. Input calls do not allocate after the node is open.
3. A node close sends the neutral report first, so held keys and contacts are released.
4. Calls on a node return `MIMHID_ERR_BUSY` while the board is re-enumerating or a batch is open.
