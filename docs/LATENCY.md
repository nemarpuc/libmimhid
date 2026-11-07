# Latency

This page separates numbers that follow from the design (arithmetic) from numbers that have been measured. One end-to-end measurement exists (below); everything else is arithmetic or not measured.

## Where the time goes

```text
application call
  -> host encodes the report (no allocation)
  -> write() to the serial port
  -> USB-UART bridge
  -> UART wire time                       (arithmetic, see PROTOCOL.md)
  -> UART receive (DMA ring buffer, polled by core 1)
  -> SPSC queue to core 0
  -> TinyUSB endpoint
  -> wait for the host's next poll        (bInterval = 1 ms)
  -> target operating system
```

| Stage | Bound | Basis |
|---|---|---|
| UART wire time, 9-byte report at 921,600 baud | 152.0 µs | (9 + 5) bytes x 10 bits / 921,600 |
| UART wire time, 9-byte report at 3,000,000 baud | 46.7 µs | 14 bytes x 10 bits / 3,000,000 |
| UART wire time, 9-byte report at 12,000,000 baud | 11.7 µs | 14 bytes x 10 bits / 12,000,000 |
| USB poll wait | 0 to 1000 µs, 500 µs on average | Full-Speed interrupt endpoint with `bInterval = 1` |
| USB-UART bridge buffering | not characterized | Depends on the adapter and driver |
| Core 1 to core 0 hand-off, TinyUSB, target OS | not measured separately | Inside the end-to-end figure below |

The USB poll wait dominates once the UART frame is on the wire. Raising the baud rate shortens only the UART term. The rate is the fastest one the board, the operating system and the serial adapter carry without loss: no table caps it, the search starts at the board's `max_baud` (see [PROTOCOL.md](PROTOCOL.md#line-rate)), and `mimhid_device_link_counters` shows whether the line keeps losing nothing while the program runs. A High-Speed target-facing port would shorten the larger term (125 µs microframes instead of 1 ms frames); the firmware has no High-Speed port yet.

## Measured, once

2026-11-07, libmimhid 0.1.0 on a Raspberry Pi Pico (RP2040), a Prolific PL2303 USB-UART adapter (USB Full-Speed), the program and the target on the same Linux machine, which is a VMware guest with both USB devices passed through. The program times a key press from just before `mimhid_node_submit` returns to the moment the target's evdev device reports it (monotonic clock, 8 to 9 ms between presses, the input device grabbed so nothing is typed). The link ran at 7,812,500 baud, the rate the search chose. 500 presses and 500 releases: none lost, and the board's link counters stayed zero.

| | min | median | p90 | p99 | max |
|---|---|---|---|---|---|
| submit to key-down event | 367 µs | 952 µs | 1331 µs | 1471 µs | 1800 µs |
| submit to key-up event | 377 µs | 830 µs | 1297 µs | 1491 µs | 3085 µs |

A command round trip (host to board and back) measured 995 to 1003 µs at the median at 0.92, 3 and 7.8 Mbaud alike; only its minimum fell (599, 357 and 387 µs). The adapter's 1 ms USB frame sets the median, not the baud rate. Calling `mimhid_node_submit` took 7 µs at the median when called in a tight loop, 110 µs with 2 ms between calls, with rare outliers of tens of milliseconds in the tight loop (not traced; a VM may cause them).

What this does not show: a real second machine as the target (the target's USB stack here is virtual), the time until an application receives the event, adapters other than this one (an FT232H, with 125 µs microframes, is expected to be faster; not measured), or how the numbers move with load. Treat them as one data point, not a specification.

## Host serial latency

Many USB-UART bridges hold received bytes until a latency timer expires. FTDI drivers default to 16 ms on Windows and macOS and to a similar value on Linux; CP210x bridges can add milliseconds as well. The timer delays only the direction from the adapter to the PC: the board's answers (`ACK`, `PONG`) and events, which the host waits for while it configures the board, and the link test. Reports go the other way (PC to adapter to board) and are not held by it.

Linux: `libmimhid` automatically sets `ASYNC_LOW_LATENCY` via `TIOCGSERIAL` / `TIOCSSERIAL` when opening the serial port. It can also be set manually:

```bash
setserial /dev/ttyUSB0 low_latency
```

For FTDI devices the timer is also exposed as `/sys/bus/usb-serial/devices/ttyUSB0/latency_timer`; `udev/51-mimhid.rules` sets it to 1 ms for the FT232R/H bridges when they are plugged in.

Windows: Device Manager > the COM port > Port Settings > Advanced > Latency Timer, set to 1 ms.

macOS: the vendor driver sets it; the built-in FTDI driver uses the device default.

Whether these settings apply depends on the adapter and driver; none has been measured here.

## Allocation and locking

- The host serializes into fixed buffers and parses incoming frames in a fixed-capacity ring buffer; neither profile mutation calls nor the reader thread allocate on the heap.
- On the board, the receive side parses UART bytes directly into one single-producer single-consumer ring per interface (zero-copy from UART ring to endpoint queue); the USB side pops and sends. On the Raspberry Pi Pico (RP2040) and Raspberry Pi Pico 2 (RP2350) these run on separate cores; the other ports run both in one loop. The rings use acquire/release atomics and no locks.
- Configuration changes (`node_open`, `node_close`, `device_commit`) are not on this path: they re-enumerate the USB device and take at least reconnect delay ms plus the target's enumeration time.
- Each ring is full-checked: when a ring is full the new report is dropped rather than overwriting an old one.

## Higher polling rates

Full-Speed USB is limited to 1000 Hz. A High-Speed capable MCU could poll faster, but the firmware has no High-Speed port yet. See [PORTING.md](PORTING.md).
