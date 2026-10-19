# Limits

Where a number is a project choice rather than a USB rule, the table says so.

## Protocol and board

| Item | Limit | Notes |
|---|---|---|
| Nodes | 8 (`MIMHID_MAX_NODES`) | protocol choice; node ids 0..7, 16 addressable |
| USB HID interfaces | `max_interfaces` of the board | 7 on Raspberry Pi Pico (RP2040), Raspberry Pi Pico 2 (RP2350), and nRF52840; 4 on ESP32-S3; 3 on STM32F4 full speed. A build option of the port. |
| Report size | 64 bytes (`max_report_bytes`) | the HID endpoint buffer |
| All report descriptors together | `max_descriptor_bytes`, 1024 by default | `MIMHID_MAX_DESC_BYTES` at firmware build time |
| Feature answers | 8 per configuration, 32 bytes each | `MIMHID_MAX_FEATURES`, `MIMHID_FEATURE_MAX` |
| Strings | printable ASCII, at most 31 characters | manufacturer, product, serial, configuration, interface |
| `max_power_ma` | 0..500 | written in 2 mA units |
| `poll_interval_ms` | 1..255 | Full-Speed interrupt endpoint |
| `ep_in_size` | 1 to 64 | IN endpoint packet size (typical 64) |
| `ep_out_size` | 0, or 1 to 64 | OUT endpoint packet size (`0` disables OUT endpoint) |
| `country_code` | 0..35 | |
| Line rate | one of `MIMHID_BAUD_RATES`, 115,200 to 4,000,000 | the board starts at 115,200 and makes the rates in its `baud_mask` (RP2040 and RP2350 all; ESP32-S3 all; STM32F4 those its USART clock divides within 2 %; nRF52840 up to 1,000,000). The link runs at the fastest rate the board, the operating system and the serial adapter carry, or at `baud_rate` (`MIMHID_ERR_UNSUPPORTED` when it is not carried). |
| Queue per interface | 16 reports | a full queue drops the newest report |

In `libmimhid`, each node explicitly defines its USB `interface_number` (no auto-assignment). `mimhid_node_open` and `mimhid_node_open_raw` return `MIMHID_ERR_OVERFLOW` when the board's interface or descriptor limits are exceeded.

## Host library

Limits of each profile (axes, contact slots, usages, Report ID rules) are those of `libaoahid`, validated by the `mimhid_spec_create_*` calls; the options structures in `mimhid.h` document each field. The Linux HID parser limits in `mimhid_device_options` (`linux_*_policy`) are caller policy and must be nonzero.

## Invariants

1. Multi-byte report fields are little-endian.
2. Input calls do not allocate after the node is open.
3. A node close sends the neutral report first, so held keys and contacts are released.
4. Calls on a node return `MIMHID_ERR_BUSY` while the board is re-enumerating or a batch is open.
