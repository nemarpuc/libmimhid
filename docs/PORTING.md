# Porting

Host operating-system portability (Linux, macOS, and Windows requirements, builds, and serial backends) is covered in [BUILDING.md](BUILDING.md) and the [Host](#host) section below; this document focuses on porting microcontroller firmware.

## Firmware layout

```text
firmware/
  common/        portable core, shared by every port
    core.c         frame parser, staged configuration, command handling, queues
    control.c      control transfers of the target: reports and holds them (see PROTOCOL.md)
    ep0.c          endpoint 0 packet-size splitting when bMaxPacketSize0 < 64
    usb_descriptors.c  builds and serves device, configuration, string, and report descriptors
    config.h       configuration structures and limits
    tusb_config.h  TinyUSB configuration
    hal.h          the twelve functions a port provides
  rp/            RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2)
  ports/
    nrf52840/      nRF52840 (TinyUSB BSP, UARTE1 + interrupt)
    stm32f4/       STM32F4 (TinyUSB BSP, USART2 + circular DMA)
    esp32s3/       ESP-IDF component
  tests/         host-side tests of common/ against a fake HAL
proto/           wire protocol constants shared with the host (mimhid_proto.h)
```

`firmware/CMakeLists.txt` builds the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2) images. The other ports have their own project under `firmware/ports/`. See [BUILDING.md](BUILDING.md#building-the-firmware) for the exact build commands.

## Control transfers

`control.c` and `ep0.c` are built into every port and need two things from the port's build: the link options `-Wl,--wrap=dcd_event_handler`, `-Wl,--wrap=dcd_edpt_stall`, and `-Wl,--wrap=dcd_edpt_xfer` (GNU ld; every port's project in this tree sets them), and no other definition of `tud_vendor_control_xfer_cb` (a vendor class would own it). It uses `dcd_int_disable` and `dcd_int_enable` of root hub port 0 around its shared state and `hal_millis` in the interrupt, so both must be callable there. A port that runs the receive side on another core (RP2040, RP2350) leaves it at that; a port that runs `mimhid_run` reads the UART from the USB side while it waits for a verdict, and nothing else is needed.

`ep0.c` runs endpoint 0 in the size the device descriptor announces (`bMaxPacketSize0`). TinyUSB gives a controller driver up to `CFG_TUD_ENDPOINT0_SIZE` (64) bytes per endpoint 0 transfer, so below 64 the wrapped `dcd_edpt_xfer` passes one packet at a time and starts the next from the stack's task with `usbd_defer_func`; the port's `tusb_config.h` keeps `CFG_TUD_ENDPOINT0_SIZE` at 64.

## HAL

A port implements the twelve functions declared in `firmware/common/hal.h`:

| Function | Contract |
|---|---|
| `hal_init()` | Clocks, board, USB PHY if needed, and the UART at the boot rate (`MIMHID_BOOT_BAUD`, 115,200), 8N1. |
| `hal_uart_baud_actual(baud)` | The rate the UART's divider really gives for `baud`, or 0 when it cannot be made at all. The core refuses `SET_BAUD` when it differs by more than 2 % (`MIMHID_BAUD_TOLERANCE_PPM`) and reports the actual rate in the `ACK`. Compute it from the clock and the divider; do not bound-check. |
| `hal_uart_max_baud()` | The fastest rate the UART can make (peripheral clock over the smallest divider). Read once at start-up; it becomes the `PONG`'s `max_baud`. Choose the UART and its clock tree for it: on an STM32F4 USART1 and USART6 sit on the faster APB2 bus. |
| `hal_uart_has_break()` | True when the port sees a UART `BREAK` through `hal_uart_errors`. |
| `hal_uart_errors()` | What went wrong on the line since the last call, as `HAL_UART_ERR_*` bits (`BREAK`, framing, overrun, receive ring three quarters full or more), and clears it. A `BREAK` returns the board to the boot rate whatever rate it was on; the other bits feed the counters the host reads. A hardware flag that only latches is reported once per call. |
| `hal_uart_set_baud(baud)` | Wait until queued output has left, then switch the UART. Called from the receive context; it must not interleave with `hal_uart_write`. |
| `hal_chip()` | The chip, as `MIMHID_CHIP_*` names it. |
| `hal_uart_read(dst, max)` | Copy received bytes without blocking; called from one context only. |
| `hal_uart_write(data, len)` | Blocking send; callable from both the receive and USB contexts. |
| `hal_millis()` | Milliseconds since boot. |
| `hal_unique_id(out, max)` | Copy the chip's unique id and return its length, or 0 when there is none. Used for `serial_from_chip_id`. |
| `hal_control_silent_drop()` | Disarm endpoint 0 IN and OUT without a STALL, so that it NAKs until the target gives up. `control.c` calls it for `MIMHID_CONTROL_ACTION_DROP`, and in place of the endpoint 0 stall the stack makes after a dropped transfer. Only the RP2040/RP2350 port implements it; the other ports have an empty function. |

The port's `main()` calls `hal_init()` and `mimhid_core_init()`, then either runs `mimhid_run()` (one core) or splits the work: `mimhid_rx_poll()` on one core and `tud_task()` plus `mimhid_usb_poll()` on the other (the RP2040 and RP2350 port, `firmware/rp/main.c`, does this).

The receiver must hold bytes for the core between polls. Choose a ring or DMA buffer that can absorb the longest stall of the main loop; at 3,000,000 baud a byte arrives every 3.3 µs, and the longest frame is `MIMHID_MAX_FRAME_PAYLOAD` + 6 bytes (`MIMHID_MAX_FRAME_PAYLOAD` is 1,032 with the defaults of `firmware/common/config.h`, so 1,038 bytes). The core keeps its own receive ring (`RX_SIZE` in `firmware/common/core.c`: 4,096 bytes for a `MIMHID_MAX_FRAME_PAYLOAD` up to 1,500, else 8,192); the RP port's DMA ring is 1,024 bytes (`DMA_BUF_SIZE`).

## Endpoint count and the HID layout

Each USB HID interface needs one interrupt IN endpoint (and optionally one OUT endpoint). A port states what its USB controller has in its build, on the compiler command line; `core.h` and `tusb_config.h` stop the build with an error when one is missing (nothing is guessed from the chip family or from the headers read so far):

| Macro | Meaning |
|---|---|
| `MIMHID_HID_ITFS` | The interfaces (one IN endpoint each) the controller can open at once; reported as `max_interfaces` in the `PONG`, and the host packs nodes to fit. |
| `MIMHID_MAX_ENDPOINT_NUMBER` | The highest endpoint number it has; reported as `max_endpoint_number`. The firmware refuses a larger endpoint address (`NAK_OVERFLOW`), and so does the host library (`MIMHID_ERR_OVERFLOW`), because the target's `SET_CONFIGURATION` would fail and nothing would say why. |
| `MIMHID_MAX_ENDPOINT_BYTES` | 64 at full speed, 1024 at high speed (USB 2.0 5.7.3, 5.7.4); `core.c` checks it against the speed TinyUSB is built for. |
| `CFG_TUSB_OS`, `CFG_TUSB_RHPORT0_MODE`, `CFG_TUD_MAX_SPEED` | TinyUSB's own, stated the same way. |

| `MIMHID_HID_ITFS` | `MIMHID_MAX_ENDPOINT_NUMBER` | Used by |
|---:|---:|---|
| 15 | 15 | RP2040 (Raspberry Pi Pico), RP2350 (Raspberry Pi Pico 2): the endpoint numbers 1 to 15 in both directions (datasheets: RP2040 4.1.1.1.1, RP2350 12.7.1.1.1) |
| 7 | 7 | nRF52840: EPIN1 to EPIN7 and EPOUT1 to EPOUT7 (number 8 is the isochronous endpoint) |
| 4 | 6 | ESP32-S3: TinyUSB's controller table has `ep_count` 7 and `ep_in_count` 5, both with endpoint 0, so the numbers are 1 to 6 and four IN endpoints can be open. Whether the IN and OUT endpoints together are limited further (the TRM's "six additional endpoints") has not been checked against the TRM |
| 3 | 3 | STM32F4 full speed on the F401, F405, F407, F411, F429, and F439 (`USB_OTG_FS_MAX_IN_ENDPOINTS` 4 with endpoint 0) |
| 5 | 5 | STM32F4 full speed on the F412 (`USB_OTG_FS_MAX_IN_ENDPOINTS` 6 with endpoint 0); the F413, F423, F446, F469, and F479 have 6 as well, but no board of them is listed in `ports/stm32f4/CMakeLists.txt` |

Set `MIMHID_HID_ITFS` as high as the controller's endpoints and TinyUSB's `CFG_TUD_HID` allow, and no higher than 15 (USB has endpoint numbers 1 to 15, USB 2.0 glossary) and `MIMHID_MAX_NODES` (16, the nodes the wire format names); the wire protocol does not depend on the values. They are on the command line (the CMake file of the port) so that every source sees the same numbers whatever it includes first: an interface count that differs between sources overruns arrays (`firmware/tests` builds the check in both include orders, and builds the probe once with each macro left out to see the error). In `libmimhid`, callers explicitly configure each node's `iface.number`, `ep_in`, and `ep_out` via `mimhid_node_config`; the firmware takes every value as given and refuses a zero where one is required (a zero LANGID, `bcdUSB`, `bcdHID`, endpoint address, size, or interval), and a `COMMIT` whose nodes of one interface disagree or reuse an endpoint number.

`MIMHID_MAX_DESC_BYTES` (default 2048) bounds all report descriptors together. It is reported as `max_descriptor_bytes`. It, `MIMHID_MAX_FEATURES`, `MIMHID_FEATURE_MAX`, and `MIMHID_QUEUE_DEPTH` are sizes of the firmware's own buffers, announced in the `PONG`, not facts about the chip; the build RAM at the reference settings is 58 KB on the STM32F411, 108 KB on the nRF52840, and 125 KB on the RP2040, each well inside its RAM.

`MIMHID_MAX_REPORT_BYTES` defaults to `MIMHID_MAX_ENDPOINT_BYTES`, and `MIMHID_FEATURE_MAX` to 1024. TinyUSB keeps a buffer of the larger of the two for every HID interface (`MIMHID_HID_BUFSIZE`), so on a chip with little RAM a port sets them lower before `config.h` is read. The `PONG` reports what was built. The high-speed values have not been run on a board.

`firmware/tests` builds the core for 15, 7, 4, and 3 interfaces and high speed against a fake HAL and checks the descriptors and report routing. `tests/sim` does the same with the real host library over a pseudo-terminal (`mimhid.sim-15`, `mimhid.sim-7`, `mimhid.sim-3`, `mimhid.sim-hs`).

## Other MCUs

| MCU family | USB | Notes |
|---|---|---|
| i.MX RT1062 (Teensy 4.x), STM32H7 | High-Speed | A higher poll rate is possible in principle. Add a port with a DMA UART receiver and the full layout (eight or more IN endpoints). |
| STM32 with a High-Speed OTG core, or with at least eight IN endpoints | Full or High | Reuse `ports/stm32f4`, add the board to its table of `USB_OTG_FS_MAX_IN_ENDPOINTS` (a High-Speed core has 6 with endpoint 0, so five interfaces, not seven), and mind `TUP_DCD_ENDPOINT_MAX`; check the TinyUSB BSP for the board and the USART/DMA mapping in `hal_stm32f4.c`. |
| Other TinyUSB BSP families | Full | Copy `ports/nrf52840`, replace `hal_nrf52.c`. |

## STM32F4 port assumptions

`hal_stm32f4.c` uses USART2 (PA2 TX, PA3 RX) and DMA1 stream 5 channel 4, which is the mapping on every STM32F4. The TinyUSB BSP's `board_init()` must have enabled USART2 and its pins; this holds for `stm32f411blackpill`, `stm32f411disco`, and `stm32f407disco`. `-DBOARD` is required, and a board missing from the endpoint table in `ports/stm32f4/CMakeLists.txt` stops the build. For another board, check that the BSP's `UART_DEV` is `USART2`.

## nRF52840 notes

TinyUSB raises the D+ pull-up on every `USB_EVT_READY` (a cable plugged in, the target powering up), whatever `tud_disconnect()` did before; a board that has not been committed yet (or has been detached) would enumerate its placeholder identity with no interface. The port links `--wrap=tusb_hal_nrf_power_event` and takes the pull-up down again while `mimhid_core_attached()` is false. The ESP32-S3, STM32F4, RP2040, and RP2350 drivers keep the soft-disconnect across VBUS changes.

`hal_nrf52.c` uses UARTE1 with one-byte DMA transfers and a receive interrupt at priority 1, above the USB interrupt. The nRF52840 makes the rates of its `BAUDRATE` register only, up to 1,000,000; 921,600 runs at 941,176 (+2.1 %), which UART receivers accept. The rate is changed with the receiver stopped and the peripheral disabled.

## Host {#host}

The serial backend lives in `src/transport/`:

| File | Platform |
|---|---|
| `serial_posix.cpp` | Linux and macOS: `termios`, `poll()` (`select()` on macOS), non-blocking reads and writes |
| `serial_baud_linux.cpp` | Linux: arbitrary baud rate setting and querying via `termios2` (`BOTHER`, `TCGETS2` / `TCSETS2`) |
| `serial_win32.cpp` | Windows: overlapped I/O on a `\\.\COMn` handle |
| `ports_linux.cpp`, `ports_macos.cpp`, `ports_win32.cpp` | Candidate port enumeration for discovery |
| `frame.cpp` | Frame encoding and parsing (CRC-16), the `PONG`, the link counters; no port, no threads |
| `handshaker.cpp` | Finding a board and agreeing the line rate, before the reader thread runs |
| `link.cpp` | Reader thread, ACK and retry, report frames; shared by all platforms |

A new platform implements the `SerialPort` class (`serial_port.hpp`: open, close, read with timeout, write with stall timeout, drain) and the candidate port listing (`list_candidate_ports`), and adds its files to `MIMHID_SOURCES` in `CMakeLists.txt`. The framing (`frame.cpp`), the rate search (`handshaker.cpp`), the retries, and the reader thread (`link.cpp`) are shared.
