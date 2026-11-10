# Latency

This page separates numbers that follow from the design (arithmetic) from numbers that have been measured, and explains how to measure each stage cleanly. One end-to-end hardware measurement exists (below); everything else is arithmetic or not measured.

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

The USB poll wait dominates once the UART frame is on the wire. Raising the baud rate shortens only the UART term. The rate is the fastest one the board, the operating system, and the serial adapter carry without loss: no table caps it, the search starts at the board's `max_baud` (see [PROTOCOL.md](PROTOCOL.md#line-rate)), and `mimhid_device_link_counters` shows whether the line keeps losing nothing while the program runs. A High-Speed target-facing port would shorten the larger term (125 µs microframes instead of 1 ms frames); the firmware has no High-Speed port yet.

## Measured, once {#measured-once}

2026-11-07, libmimhid 0.1.0 on a Raspberry Pi Pico (RP2040), a Prolific PL2303 USB-UART adapter (USB Full-Speed), the program and the target on the same Linux machine, which is a VMware guest with both USB devices passed through. The program times a key press from just before `mimhid_node_submit` returns to the moment the target's evdev device reports it (monotonic clock, 8 to 9 ms between presses, the input device grabbed so nothing is typed). The link ran at 7,812,500 baud, the rate the search chose. 500 presses and 500 releases: none lost, and the board's link counters stayed zero.

| | min | median | p90 | p99 | max |
|---|---|---|---|---|---|
| submit to key-down event | 367 µs | 952 µs | 1331 µs | 1471 µs | 1800 µs |
| submit to key-up event | 377 µs | 830 µs | 1297 µs | 1491 µs | 3085 µs |

A command round trip (host to board and back) measured 995 to 1003 µs at the median at 0.92, 3, and 7.8 Mbaud alike; only its minimum fell (599, 357, and 387 µs). The adapter's 1 ms USB frame sets the median, not the baud rate. Calling `mimhid_node_submit` took 7 µs at the median when called in a tight loop, 110 µs with 2 ms between calls, with rare outliers of tens of milliseconds in the tight loop (not traced; a VM may cause them).

What this does not show: a real second machine as the target (the target's USB stack here is virtual), the time until an application receives the event, adapters other than this one (an FT232H, with 125 µs microframes, is expected to be faster; not measured), or how the numbers move with load. Treat them as one data point, not a specification.

## How to measure

Three distinct measurements answer three different questions. Keep them separate so host CPU overhead, USB frame phase, and cross-machine clock offsets are never conflated.

### (a) Library CPU cost per call (nanoseconds, no hardware)

A micro-benchmark against the simulated board (`tests/sim/fake_board.c`) or a pseudo-terminal (`posix_openpt` / `openpty`) measures the host library's CPU work per report in **nanoseconds** without any physical hardware.

- **How to build one:** Open a pseudo-terminal pair, run the simulated board (`fake_board.c`, or a background thread that completes the open/commit handshake and drains the master fd with `read(2)`), open a keyboard or mouse node with `mimhid_node_open`, and in a loop time `mimhid_keyboard_key` + `mimhid_node_submit` using `clock_gettime(CLOCK_MONOTONIC, &ts)`.
- **What it shows:** The CPU cost inside `libmimhid` on the host—state mutation, bit-exact report serialization into the stack buffer, optional `validate_reports` checking, CRC-16/CCITT-FALSE framing, locking `tx_mutex_`, and the kernel `write(2)` syscall into the pty buffer—typically a few hundred nanoseconds to a few microseconds depending on whether the pty syscall is included.
- **What it cannot show:** UART wire transmission time, USB-UART bridge packetization or latency timers, MCU DMA/SPSC hand-off, USB interrupt endpoint polling intervals (`bInterval`), or the target OS input stack.

### (b) Send to target kernel on the same host (microseconds)

Plugging **both** the USB-UART bridge and the board's USB device cable into the **same** Linux host measures true end-to-end delivery from the library call to the target kernel's input subsystem against a single hardware clock.

- **How to run it:**
  1. Open the board's newly enumerated `/dev/input/eventX` node on the host, set its timestamp clock to `CLOCK_MONOTONIC` with `int clk = CLOCK_MONOTONIC; ioctl(ev_fd, EVIOCSCLOCKID, &clk);`, and grab it with `ioctl(ev_fd, EVIOCGRAB, 1)` so test inputs do not type into the desktop session.
  2. Space injections by an irregular interval (for example, `8.1 ms` to `9.3 ms`) so submissions do not phase-lock to the 1 ms USB frame clock.
  3. Record `t0` with `clock_gettime(CLOCK_MONOTONIC, &t0)` immediately before `mimhid_node_submit`, read the matching `struct input_event` from `ev_fd`, and compute `(ev.time.tv_sec - t0.tv_sec) * 1e6 + ev.time.tv_usec - t0.tv_nsec / 1000`.
  4. Report the distribution in **microseconds** (`p50`, `p90`, `p99`, `min`, `max`) alongside the sample count `N` (for example, `N = 500` presses and `500` releases, as in [Measured, once](#measured-once) above) and verify with `mimhid_device_link_counters` that zero frames were dropped.
- **Why microseconds (not nanoseconds):** A USB Full-Speed interrupt endpoint is polled once per `1 ms` (`1,000 µs`) frame (`125 µs` microframes at High-Speed). Because the submission time is uniformly distributed across the `1,000 µs` poll window, the USB frame phase alone introduces `0` to `1,000 µs` of variance (`500 µs` mean); reporting nanosecond digits on a millisecond-quantized bus is meaningless.
- **What it shows:** The complete physical pipeline—host library, kernel tty/usb-serial driver, USB-UART adapter, UART wire, MCU firmware core, TinyUSB device stack, USB host controller, and Linux `usbhid` / `evdev` driver—with zero clock-synchronization error.
- **What it cannot show:** Latency into a separate target OS (Android, Windows, macOS, or a console) or application/compositor rendering latency above `evdev`.

### (c) Comparing against `libaoahid` on the same phone target

When the target is a separate device such as an Android phone, the host PC and the phone do not share a `CLOCK_MONOTONIC` epoch.

- **How to compare fairly:** Use the **same** phone, the **same** USB port/cable topology, and the **same** target-side timestamping method (such as `adb shell getevent -lt` or a small target agent recording `CLOCK_MONOTONIC` on input receipt) for both `libaoahid` (PC USB directly to phone in AOA mode) and `libmimhid` (PC UART to MCU board, MCU board USB via OTG adapter to the phone), running the two trials back-to-back.
- **What it shows:** The relative latency **difference** (`latency_mimhid - latency_aoahid`) when the host and phone clocks are compared against a common synchronization reference or over a short back-to-back window where clock drift is negligible; the unknown constant offset between the PC clock and the phone clock cancels **only in a difference**.
- **What it cannot show:** Absolute one-way latency in microseconds from the PC to a separate target machine. **Measuring absolute cross-machine latency requires a hardware USB protocol analyzer** (or a shared hardware GPIO pulse / logic analyzer probing both the host UART TX line and the target's response).

## Host serial latency

Many USB-UART bridges hold received bytes until a latency timer expires. FTDI drivers default to 16 ms on Windows and macOS and to a similar value on Linux; CP210x bridges can add milliseconds as well. The timer delays only the direction from the adapter to the PC: the board's answers (`ACK`, `PONG`), events, which the host waits for while it configures the board, and the link test. Reports go the other way (PC to adapter to board) and are not held by it.

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
- Configuration changes (`node_open`, `node_close`, `device_commit`) are not on this path: they re-enumerate the USB device and take at least `reconnect_delay_ms` plus the target's enumeration time.
- Each ring is full-checked: when a ring is full the new report is dropped rather than overwriting an old one.

## Higher polling rates

Full-Speed USB is limited to 1000 Hz. A High-Speed capable MCU could poll faster, but the firmware has no High-Speed port yet. See [PORTING.md](PORTING.md).
