# Limits

Every number below says where it comes from: a rule of a USB-IF specification, a limit of a chip, or a choice of this project. A rule is cited by document and section; the documents read for this page are the USB 2.0 Specification (the `usb_20.pdf` of the June 2025 package), the USB 3.2 Specification Revision 1.1, the Device Class Definition for HID 1.11, and the RP2040 and RP2350 datasheets. Where a number is a project choice rather than a USB rule, the table says so.

The board takes the speed it runs at from its chip: the Pico, the Pico 2, the nRF52840, the STM32F4 as built here and the ESP32-S3 are full-speed devices, and the board says in its PONG which it is (`usb_high_speed`). A rule that differs by speed is checked against that speed. No board here is a USB 3 device, so the USB 3.2 columns below describe what a USB 3 port would have to change; the library does not present at those speeds.

## Rules of the USB-IF specifications

| Item | Limit | Source |
|---|---|---|
| `usb_device.ep0_max_packet_size` | 8, 16, 32 or 64 at full speed; 64 at high speed | USB 2.0 9.6.1 ("only 8, 16, 32, or 64 are valid"; a high-speed device must use 64), 5.5.3. USB 3.2 9.6.1 gives `bMaxPacketSize0` 9 (2^9 = 512) at Gen X speeds |
| `ep_in.max_packet_size`, `ep_out.max_packet_size` | at most 64 at full speed; at most 1024 at high speed, but only in an alternate interface setting: "All high-speed device default interface settings must not include any interrupt endpoints with a data payload size ... greater than 64 bytes" | USB 2.0 5.7.3, 9.6.6. The board has no alternate settings, so a packet above 64 bytes at high speed is outside the specification; the library takes it (the board's `max_endpoint_bytes` is the limit it enforces). USB 3.2 9.6.6: 1 to 1024 for an interrupt endpoint with `bMaxBurst` 0 |
| `ep_in.poll_interval_ms`, `ep_out.poll_interval_ms` (`bInterval`) | 1 to 255 frames (ms) at full speed; 1 to 16 at high speed, where it is the exponent of 2^(n-1) microframes | USB 2.0 9.6.6; USB 3.2 9.6.6 gives 1 to 16 as well |
| `ep_in.address`, `ep_out.address` | IN `0x81` to `0x8F`, OUT `0x01` to `0x0F`: bit 7 the direction, bits 6..4 zero, endpoint number 1 to 15 (0 is the control endpoint, which has no endpoint descriptor) | USB 2.0 9.6.6 ("Bit 6...4: Reserved, reset to zero"; "There is never an endpoint descriptor for endpoint zero"), glossary "Endpoint Number: A four-bit value between 0H and FH" |
| `usb_config.configuration_value` | 1 to 255 by the specification; **1 here** | USB 2.0 9.6.3. The board has one configuration, and TinyUSB maps `SET_CONFIGURATION(n)` to the n-th configuration descriptor (`usbd.c`, `process_set_config`), so any other value would not enumerate. Refused with `MIMHID_ERR_UNSUPPORTED` |
| `usb_config.bm_attributes` | bit 7 set; bit 6 self-powered; bit 5 remote wakeup; bits 4..0 zero | USB 2.0 9.6.3 ("D7: Reserved (set to one) ... D4...0: Reserved (reset to zero)") |
| `usb_device.device_subclass` | 0 when `device_class` is 0 | USB 2.0 9.6.1 ("If the bDeviceClass field is reset to zero, this field must also be reset to zero") |
| `usb_config.bmax_power` | 0 to 250 (2 mA units, so 500 mA) | USB 2.0 9.6.3, 7.2.1 (a high-power function draws at most 500 mA). At Gen X speeds the unit is 8 mA (USB 3.2 9.6.3) |
| Strings | at most 126 characters | USB 2.0 9.6.7: `bLength` is one byte and a character takes two bytes of UTF-16LE, so 2 + 2n is at most 254. The library takes UTF-8 and sends UTF-16 (a code point from U+10000 up takes two units, so 126 units hold fewer such characters); control characters (U+0000 to U+001F, U+007F) are refused as a project choice, and `language_id` says which language the strings are in, which the library does not check |
| `iface.class_code` | the specification leaves the class to the USB-IF (9.6.5; zero is reserved); **0x03 (HID) here** | The board's USB stack opens HID interfaces only, and an interface of any other class leaves the whole device unconfigured. Refused with `MIMHID_ERR_UNSUPPORTED` |
| `iface.subclass` | 0 (none) or 1 (boot interface) | HID 1.11 4.2; 2 to 255 are reserved |
| `iface.protocol` | 0 (none), 1 (keyboard) or 2 (mouse) | HID 1.11 4.3; 3 to 255 are reserved. The specification says the protocol "only has meaning" with subclass 1 and is 0 otherwise; the library does not enforce that, so a device that sets it anyway can be copied |
| `iface.country_code` | 0 to 35 | HID 1.11 6.2.1: 0 not supported, 1 to 35 the listed countries (35 Turkish-F), 36 to 255 reserved |
| HID descriptor | one class descriptor, the report descriptor | HID 1.11 6.2.1 requires at least one and allows further ones (physical descriptors, 6.2.3). The board lists the report descriptor only |
| Interrupt OUT | optional | HID 1.11 4.4: without it Output reports use `SET_REPORT` on endpoint 0. `ep_out.address` 0 leaves it out |
| Control transfer wait (`timeout_ms` of a rule) | 1 to 5000 ms (`MIMHID_CONTROL_MAX_TIMEOUT_MS`) | USB 2.0 9.2.6.1: "USB sets an upper limit of 5 seconds as the upper limit for any command". 9.2.6.4 and 9.2.6.5 ask a class request without a data stage to finish in 50 ms and one with data to the host to start its answer within 500 ms, and a host may give up sooner than either; a rule holds a request as long as its program needs, up to the 5 seconds |
| `language_id` | one 16-bit LANGID | USB 2.0 9.6.7 allows an array of them; the board lists one |

## Limits of the chips

| Item | Limit | Notes |
|---|---|---|
| USB HID interfaces | `max_interfaces` of the board | **15 on the Raspberry Pi Pico (RP2040) and Pico 2 (RP2350)**: both controllers have the endpoint numbers 1 to 15 in both directions (RP2040 datasheet 4.1.1.1.1, RP2350 datasheet 12.7.1.1.1: "Endpoints 0 -> 15 in both in and out directions"), which is also the most USB allows. 7 on the nRF52840 (its product specification, USBD: "14 bulk/interrupt (7 IN, 7 OUT)", plus the control endpoint 0); 4 on the ESP32-S3 (its TRM, 32.2.2 Device Mode Features: "Maximum of five IN endpoints concurrently active at any time (including EP0 IN)", which leaves four for HID, so this is the chip's limit); 3 on the STM32F4 at full speed, a build option of the port (`MIMHID_HID_ITFS`), which is the chip's limit: ST's CMSIS device headers (`stm32f401xc.h`, `stm32f405xx.h`, `stm32f407xx.h`, `stm32f411xe.h`) define `USB_OTG_FS_MAX_IN_ENDPOINTS 4U /* Including EP0 */`, which leaves three for HID, and the STM32F405/F407 datasheet (DS8626 Rev 12, 3.30) gives the OTG_FS "4 bidirectional endpoints". The OTG_HS core (`USB_OTG_HS_MAX_IN_ENDPOINTS 6U /* Including EP0 */`, so five for HID) is not used by this port. The reference manuals were not read for this page |
| Endpoint buffers | 3840 bytes of DPSRAM on the RP2040 and RP2350, 60 buffers of 64 bytes | RP2040 datasheet 4.1.1.1.1; 15 IN and 15 OUT endpoints of 64 bytes fit with room to spare |
| Endpoint packet | `max_endpoint_bytes`: 64 on a full-speed board, 1024 on a high-speed one | USB 2.0 5.7.3: a full-speed interrupt endpoint holds 64 bytes, a high-speed one 1024 (see the note on the default setting above) |
| Report size | `max_report_bytes`: 64 on a full-speed board | the largest endpoint packet; `mimhid_device_limits` has the figure |
| Speed | full speed (12 Mb/s) on the RP2040 and RP2350 | RP2040 datasheet 4.1.1.1.1; a low-speed device cannot be presented |

## Limits of this project

| Item | Limit | Notes |
|---|---|---|
| Nodes | 16 (`MIMHID_MAX_NODES`) | the wire format: a report frame names its node in the low four bits of its type, `0x40 | id`. The board's own number is in its PONG (`max_nodes`) |
| All report descriptors together | `max_descriptor_bytes`, 2048 by default | `MIMHID_MAX_DESC_BYTES` at firmware build time. One report descriptor may be up to 65535 bytes by HID 1.11 6.2.1 (`wDescriptorLength` is two bytes); the RAM of the board is what holds it to 2048 |
| Feature answers | 8 per configuration, `max_feature_bytes` each (1024 by default) | `MIMHID_MAX_FEATURES`, `MIMHID_FEATURE_MAX`. A Feature report travels on the control pipe, so it may be longer than an endpoint packet |
| Strings | UTF-16 code units, none zero | the host converts from UTF-8 and the board stores and sends the units as they are |
| `ep_in.max_packet_size` | 1 to the board's `max_endpoint_bytes` | IN endpoint packet size |
| `ep_out.max_packet_size` | 1 to the board's `max_endpoint_bytes` | OUT endpoint packet size (`ep_out.address` 0 disables the OUT endpoint) |
| Line rate | `MIMHID_BOOT_BAUD` (115,200) up to the board's `max_baud`; no table | the board starts at 115,200 and makes any rate its UART divider gives within 2 % (RP2040 and RP2350: `clk_peri / 16`, 7.8 Mbaud at the default 125 MHz; ESP32-S3: 5 Mbaud; STM32F4: PCLK1 / 8; nRF52840: 1 Mbaud). The link runs at the fastest rate the board, the operating system and the serial adapter carry without loss (`MIMHID_BAUD_FASTEST`), or at `baud_rate` (`MIMHID_ERR_UNSUPPORTED` when it is not carried). The figures for the ports other than the RP2040 and RP2350 follow their data sheets and are not measured |
| Control routing | 8 rules (`max_control_rules`); a data stage of `max_control_data_bytes` (1024 by default, `MIMHID_CONTROL_DATA_MAX`) | A control transfer's data stage can be 65535 bytes by USB 2.0 9.3 (`wLength` is two bytes); the board's RAM holds it to 1024. Up to 7 control transfers wait in the board's queue to be reported. Standard OUT SET_ADDRESS is never held, and neither is `SET_CONFIGURATION(1)` |
| Queue per interface | 15 reports waiting (7 when a report may be longer than 64 bytes); `mimhid_node_set_queue_depth` makes it shorter | a full queue drops the newest report; `mimhid_device_set_loss_reporting` tells the program how many |

The board's numbers are fixed when its firmware is built, at the most the chip can hold, and the board reports them in its PONG. The host library cannot raise them. `MIMHID_MAX_ENDPOINT_BYTES`, `MIMHID_MAX_REPORT_BYTES` and `MIMHID_FEATURE_MAX` (`firmware/common/config.h`) can be set by a port before the header is read; the endpoint size follows the port's USB speed.

## Who answers a control request

A rule of `mimhid_device_set_control_routing` hands a request to the program, and a rule is stronger than everything below. One rule with every mask zero matches every request except `SET_ADDRESS`, `SET_CONFIGURATION(1)` and a host-to-device data stage longer than `max_control_data_bytes`; the program then answers each of the others itself (from the real device in a proxy). A request is held for at most the rule's `timeout_ms` (5000 at the most), which the program must meet, so with such a rule the target depends on the program for every request, descriptors included.

When no rule matches, the board answers from what the program gave it, from the stack, or with a STALL (the USB 2.0 Request Error, 9.2.7):

| Request | Without a rule | Source of the answer |
|---|---|---|
| `GET_DESCRIPTOR` device, configuration, string, HID, report | The descriptors of the committed configuration | The program's configuration, byte for byte |
| `GET_DESCRIPTOR` device qualifier, other speed | STALL on a full-speed board | USB 2.0 9.6.2: "If a full-speed only device (with a device descriptor version number equal to 0200H) receives a GetDescriptor() request for a device_qualifier, it must respond with a request error." The board has no other speed to describe; for a `bcd_usb` other than 0x0200 the sentence does not apply, and the board answers the same way |
| `GET_DESCRIPTOR` BOS | STALL: the board has no BOS to give | Not invented. USB 3.2 9.2.6.6: "Devices with a value of at least 0210H in the bcdUSB field ... shall support GetDescriptor (BOS Descriptor) requests". With `usb_device.bcd_usb` of 0x0210 or more, add a rule for it (request type `0x80`, request `0x06`, `wValue` high byte `0x0F`) and answer it; without one the target's enumeration fails (measured with `bcd_usb` 0x0210, 0x0300 and 0x0320: the host asks for the BOS right after the configuration descriptor, gets a STALL, resets the port and tries again until it gives up; with a rule that answered it all three enumerated, a Linux guest behind a Windows host) |
| `GET_STATUS` to the device, to an endpoint | `00 00` | The stack |
| `GET_STATUS` to an interface of the configuration | `00 00` | USB 2.0 9.4.5, figure 9-5: all bits reserved, zero. Answered by the board when it is configured and `wLength` is 2; another interface, the address state or another `wLength` is the stack's (a STALL) |
| `GET_CONFIGURATION` | The configuration value, 1 | The stack |
| `SET_ADDRESS`, `SET_CONFIGURATION(1)`, bus reset, suspend | The board's own state | Never held (they change the controller) |
| `GET_REPORT` Feature | The answer the program gave (`FEATURE`) | The program |
| `GET_REPORT` Input | The last report sent on that interface, if one was; without a Report ID it is the whole report, with one the report whose first byte is that ID (the stack puts the ID in front). None sent since the configuration was committed: no data, as below | HID 1.11 7.2 (mandatory) and 7.2.1. Not invented: only a report the program sent is returned, so with several Report IDs on an interface only the latest report is known |
| `GET_REPORT` Output; Input with no report to give | With a Report ID (and a `wLength` above 1) the stack replies with that one byte; without one it stalls | The stack |
| `SET_REPORT` | The data is reported to the program (Output on Report ID 0 as `OUTPUT_REPORT`, the others as `SET_REPORT`); a Feature value the target writes is not stored, so the next `GET_REPORT` returns the program's answer | The program decides what a write means |
| `GET_IDLE`, `SET_IDLE`, `GET_PROTOCOL`, `SET_PROTOCOL` | The stack keeps the value it was given and returns it; the program is told of `SET_IDLE` and `SET_PROTOCOL` | The stack. The board neither repeats reports at the idle rate (HID 1.11 7.2.4) nor changes report format for the boot protocol; the real device's state is not read |
| `SET_FEATURE`, `CLEAR_FEATURE` (remote wakeup, `ENDPOINT_HALT`) | Handled by the stack on the board's own endpoints | The stack; not measured. The state of a real device's endpoint is not reflected |
| `SET_INTERFACE`, `GET_INTERFACE` | `SET_INTERFACE` is acknowledged whatever the setting, and `GET_INTERFACE` returns 0: the board has one alternate setting | The stack (TinyUSB `usbd.c`: "it is mandatory to respond even if the class driver doesn't use alternate settings") |
| Any other class or vendor request | STALL | The stack |

The first column is what the target sees without a rule. A request in the table that the real device answers differently (a Feature that changes when written, an idle rate, a protocol, a vendor request) is made transparent by a rule that hands it to the program: a rule per request, or one with every mask zero. The program can also answer `LEAVE` to a held request, which gives it back to the board's own handling in this table.

The speed the target sees is the speed of the link, which the host measures from the signalling and not from `bcd_usb`; a board on a USB 3 port is a full-speed device on its USB 2 pair (the port takes it), and no value of `bcd_usb` changes that.

Each Node names its interface with `iface.number`. The board numbers its interfaces 0, 1, 2, ... in the order of those labels, so a gap in the labels is closed and the interface number the target sees can differ from the label. Nodes with the same number share the interface, and must then all have Report IDs, all different, and the same interface and endpoint settings; Nodes on different interfaces need different endpoint addresses (`MIMHID_ERR_PARAM` otherwise). `mimhid_node_open` and `mimhid_node_open_raw` return `MIMHID_ERR_OVERFLOW` when the board's interface or descriptor limits are exceeded.

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
