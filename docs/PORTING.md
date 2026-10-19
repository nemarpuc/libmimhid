# Porting

## Firmware layout

```
firmware/
  common/        portable core, shared by every port
    core.c         frame parser, staged configuration, command handling, queues
    control.c      control transfers of the target: reports and holds them (see PROTOCOL.md)
    usb_descriptors.c  builds and serves device, configuration, string, and report descriptors
    config.h       configuration structures and limits
    tusb_config.h  TinyUSB configuration
    hal.h          the functions a port provides
  rp/            RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2) - official support
  ports/
    nrf52840/      nRF52840 (TinyUSB BSP, UARTE1 + interrupt)
    stm32f4/       STM32F4 (TinyUSB BSP, USART2 + circular DMA)
    esp32s3/       ESP-IDF component
  tests/         host-side tests of common/ against a fake HAL
proto/           wire protocol constants shared with the host (mimhid_proto.h)
```

`firmware/CMakeLists.txt` builds the RP2040 (Raspberry Pi Pico) and RP2350 (Raspberry Pi Pico 2) images. The other ports have their own project under `firmware/ports/`.

## Control transfers

`control.c` is built into every port and needs two things from the port's build: the link option `-Wl,--wrap=dcd_event_handler` (GNU ld; every port's project in this tree sets it), and no other definition of `tud_vendor_control_xfer_cb` (a vendor class would own it). It uses `dcd_int_disable` and `dcd_int_enable` of root hub port 0 around its shared state and `hal_millis` in the interrupt, so both must be callable there. A port that runs the receive side on another core (RP2040, RP2350) leaves it at that; a port that runs `mimhid_run` reads the UART from the USB side while it waits for a verdict, and nothing else is needed.

## HAL

A port implements eight functions (see `common/hal.h`):

| Function | Contract |
|---|---|
| `hal_init()` | Clocks, board, USB PHY if needed, and the UART at the boot rate (`MIMHID_BOOT_BAUD`, 115,200), 8N1. |
| `hal_uart_baud_ok(baud)` | True when the UART can run within 2 % of `baud`. Asked for every rate of the protocol's table at start-up; the answers become the PONG's rate mask. |
| `hal_uart_set_baud(baud)` | Wait until queued output has left, then switch the UART. Called from the receive context; it must not interleave with `hal_uart_write`. |
| `hal_chip()` | The chip, as `MIMHID_CHIP_*` names it. |
| `hal_uart_read(dst, max)` | Copy received bytes without blocking; called from one context only. |
| `hal_uart_write(data, len)` | Blocking send; callable from both the receive and USB contexts. |
| `hal_millis()` | Milliseconds since boot. |
| `hal_unique_id(out, max)` | Copy the chip's unique id and return its length, or 0 when there is none. Used for `serial_from_chip_id`. |

The port's `main()` calls `hal_init()` and `mimhid_core_init()`, then either runs `mimhid_run()` (one core) or splits the work: `mimhid_rx_poll()` on one core and `tud_task()` plus `mimhid_usb_poll()` on the other (the official RP2040 and RP2350 dual-core Pico builds do this).

The receiver must hold bytes for the core between polls. Choose a ring or DMA buffer that can absorb the longest stall of the main loop; at 3,000,000 baud a byte arrives every 3.3 µs, and the longest frame is `MIMHID_MAX_FRAME_PAYLOAD` + 5 bytes (1,045 with the defaults). The core keeps its own ring of 4,096 bytes (1,024 when the frame limit is 250 or less), so a port buffer of 512 bytes is a reasonable minimum.

## Endpoint count and the HID layout

Each USB HID interface needs one interrupt IN endpoint (and optionally one OUT endpoint). The number the MCU can spare is the port's `MIMHID_HID_ITFS` (the `max_interfaces` the board reports in its PONG):

| `MIMHID_HID_ITFS` | Used by |
|---:|---|
| 7 | RP2040 (Raspberry Pi Pico), RP2350 (Raspberry Pi Pico 2), nRF52840 |
| 4 | ESP32-S3 (four usable IN endpoints) |
| 3 | STM32F4 full speed (three usable IN endpoints) |

Set it as high as the MCU's endpoints and TinyUSB's `CFG_TUD_HID` allow; the firmware, host library, and wire protocol do not depend on the value. In `libmimhid`, callers explicitly configure each node's `interface_number`, `ep_in_size`, and `ep_out_size` via `mimhid_node_options`.

`MIMHID_MAX_DESC_BYTES` (default 2048) bounds all report descriptors together. It is reported as `max_descriptor_bytes`.

The buffers follow the chip: `MIMHID_MAX_ENDPOINT_BYTES` is 64, or 1024 when the port's `CFG_TUSB_RHPORT0_MODE` asks for high speed; `MIMHID_MAX_REPORT_BYTES` defaults to it, and `MIMHID_FEATURE_MAX` to 1024. TinyUSB keeps a buffer of the larger of the two for every HID interface (`MIMHID_HID_BUFSIZE`), so on a chip with little RAM a port sets them lower before `config.h` is read. The PONG reports what was built. The high-speed values have not been run on a board.

`firmware/tests` builds the core for 7, 4, and 3 interfaces against a fake HAL and checks the descriptors and report routing. `tests/e2e` does the same with the real host library.

## Other MCUs

| MCU family | USB | Notes |
|---|---|---|
| i.MX RT1062 (Teensy 4.x), STM32H7 | High-Speed | A higher poll rate is possible in principle. Add a port with a DMA UART receiver and the full layout (eight or more IN endpoints). |
| STM32 with a High-Speed OTG core, or with at least eight IN endpoints | Full or High | Reuse `ports/stm32f4` and set `MIMHID_HID_ITFS=7`; check the TinyUSB BSP for the board and the USART/DMA mapping in `hal_stm32f4.c`. |
| Other TinyUSB BSP families | Full | Copy `ports/nrf52840`, replace `hal_nrf52.c`. |

## STM32F4 port assumptions

`hal_stm32f4.c` uses USART2 (PA2 TX, PA3 RX) and DMA1 stream 5 channel 4, which is the mapping on every STM32F4. The TinyUSB BSP's `board_init()` must have enabled USART2 and its pins; this holds for `stm32f411blackpill` (the default), `stm32f411disco`, and `stm32f407disco`. For another board, check that the BSP's `UART_DEV` is `USART2`.

## nRF52840 notes

`hal_nrf52.c` uses UARTE1 with one-byte DMA transfers and a receive interrupt at priority 1, above the USB interrupt. The nRF52840 makes the rates of its BAUDRATE register only, up to 1,000,000; 921,600 runs at 941,176 (+2.1 %), which UART receivers accept. The rate is changed with the receiver stopped and the peripheral disabled.

## Host

The serial backend lives in `src/transport/`:

| File | Platform |
|---|---|
| `serial_posix.cpp` | Linux and macOS: termios, `poll()` (`select()` on macOS), non-blocking reads and writes |
| `serial_win32.cpp` | Windows: overlapped I/O on a `\\.\COMn` handle |
| `ports_linux.cpp`, `ports_macos.cpp`, `ports_win32.cpp` | candidate port enumeration for discovery |
| `link.cpp` | frame encoding, ACK and retry, reader thread; shared by all platforms |

A new platform implements the `SerialPort` class (`serial_port.hpp`: open, close, read with timeout, write with stall timeout, drain) and the candidate port listing (`list_candidate_ports`), and adds its files to `MIMHID_SOURCES` in `CMakeLists.txt`. The framing, retries, and reader thread are in `link.cpp` and are shared.
