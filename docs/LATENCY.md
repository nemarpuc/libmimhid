# Latency

This page separates numbers that follow from the design (arithmetic) from numbers that have been measured. Nothing in the pipeline has been measured end to end yet.

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
| UART wire time, 9-byte report at 921,600 baud | 141.0 µs | (9 + 4) bytes x 10 bits / 921,600 |
| UART wire time, 9-byte report at 2,000,000 baud | 65.0 µs | 13 bytes x 10 bits / 2,000,000 |
| USB poll wait | 0 to 1000 µs, 500 µs on average | Full-Speed interrupt endpoint with `bInterval = 1` |
| USB-UART bridge buffering | not characterized | Depends on the adapter and driver |
| Core 1 to core 0 hand-off, TinyUSB, target OS | not measured | |

The USB poll wait dominates once the UART frame is on the wire. Raising the baud rate shortens only the UART term.

## Host serial latency

Many USB-UART bridges hold received bytes until a latency timer expires. FTDI drivers default to 16 ms on Windows and macOS and to a similar value on Linux; CP210x bridges can add milliseconds as well.

Linux: `libmimhid` automatically sets `ASYNC_LOW_LATENCY` via `TIOCGSERIAL` / `TIOCSSERIAL` when opening the serial port. It can also be set manually:

```bash
setserial /dev/ttyUSB0 low_latency
```

For FTDI devices the timer is also exposed as `/sys/bus/usb-serial/devices/ttyUSB0/latency_timer`.

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
