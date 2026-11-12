# Latency

This page separates numbers that follow from the design (arithmetic) from numbers that have been measured, and explains how to measure each stage cleanly. One end-to-end hardware measurement exists (below); everything else is arithmetic or not measured.

## Where the time goes

```text
application call
  -> host encodes the report (no allocation)
  -> write() to the serial port
  -> USB-UART bridge
  -> UART wire time                       (arithmetic, see PROTOCOL.md)
  -> UART receive (receive side; on the RP2040 and RP2350 a DMA ring read by core 1)
  -> the interface's report queue to the USB side (core 0 on the RP2040 and RP2350)
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
| Receive side to USB side hand-off, TinyUSB, target OS | not measured separately | Inside the end-to-end figure below |

The USB poll wait dominates once the UART frame is on the wire. Raising the baud rate shortens only the UART term. The rate is the fastest one the board, the operating system, and the serial adapter carry without loss: no table caps it, the search starts at the board's `max_baud` (see [PROTOCOL.md](PROTOCOL.md#line-rate)), and `mimhid_device_link_counters` shows whether the line keeps losing nothing while the program runs. A High-Speed target-facing port would shorten the larger term (125 µs microframes instead of 1 ms frames); the firmware has no High-Speed port yet.

## Measured, once {#measured-once}

Recorded as follows (the setup and the table are kept as they were written): 2026-11-07, libmimhid 0.1.0 on a Raspberry Pi Pico (RP2040), a Prolific PL2303 USB-UART adapter (USB Full-Speed), the program and the target on the same Linux machine, which is a VMware guest with both USB devices passed through. The program times a key press from just before `mimhid_node_submit` returns to the moment the target's evdev device reports it (monotonic clock, 8 to 9 ms between presses, the input device grabbed so nothing is typed). The link ran at 7,812,500 baud, the rate the search chose. 500 presses and 500 releases: none lost, and the board's link counters stayed zero.

| | min | median | p90 | p99 | max |
|---|---|---|---|---|---|
| submit to key-down event | 367 µs | 952 µs | 1331 µs | 1471 µs | 1800 µs |
| submit to key-up event | 377 µs | 830 µs | 1297 µs | 1491 µs | 3085 µs |

[CHANGELOG.md](../CHANGELOG.md) records for 0.1.0 that the 0.1.0 firmware had not run on a board ("an earlier firmware ran on a Raspberry Pi Pico, this one has not yet"), so which firmware this run used is not settled by the repository. No commit records the program that made it.

What this does not show: a real second machine as the target (the target's USB stack here is virtual), the time until an application receives the event, adapters other than this one (an FT232H, with 125 µs microframes, is expected to be faster; not measured), or how the numbers move with load. Treat them as one data point, not a specification.

## Measured, 2026-11-12 {#measured-2026-11-12}

Version 0.3.0 (every interface on the generic driver, endpoint 0 answered from control rules) on a Raspberry Pi Pico (RP2040), a Prolific PL2303 USB-UART adapter (USB Full-Speed), the program and the target on the same Linux machine, which is a VMware guest with both USB devices passed through. Method (b) below: a keyboard profile Node with a 29-byte report, the event device grabbed, 8.1 to 9.3 ms between edges, the time from just before `mimhid_node_submit` to the kernel's evdev timestamp. The link ran at 7,812,500 baud, the rate the search chose. Ten runs of 400 presses and 400 releases each; the board was re-enumerated for every run. No edge was lost in any run and the board's link counters stayed zero. The same day, same board, same program, 0.2.1 was measured the same way by writing its firmware in between.

| submit to key-down event, per run of 400 | min | median | p90 |
|---|---|---|---|
| 0.3.0, lowest and highest of ten runs | 272 to 586 µs | 818 to 1140 µs | 1236 to 1550 µs |
| 0.3.0, mean of ten runs | 371 µs | 980 µs | 1378 µs |
| 0.2.1, lowest and highest of ten runs | 267 to 565 µs | 871 to 1208 µs | 1228 to 1573 µs |
| 0.2.1, mean of ten runs | 368 µs | 1026 µs | 1402 µs |

The two versions are not told apart by this: a run's median moves by some 0.3 ms from one enumeration to the next with the same firmware, more than the versions differ. Comparing single runs is misleading (two runs taken alone showed 0.3.0 slower by 0.3 ms, two others faster). What the earlier entry does not show, this one does not show either.

Measured again the same day after the generic-device work (the byte device, the BOS descriptor, an answer's capacity, the refused second attach), same board, adapter, target and program, ten runs of 400 presses and 400 releases, the board re-enumerated for every run: the median of submit to key-down ran from 864 to 1281 µs, mean of the ten medians 1075 µs (0.3.0 before: 980 µs; 0.2.1: 1026 µs). That is 0.05 to 0.1 ms above the earlier means, less than the 0.3 ms by which a run's median moves between enumerations; the commit added nothing to the report path (no code in `endpoint_write`'s hot loop and nothing in the firmware's per-frame code changed), and this measurement does not tell it apart from the earlier ones. None lost; the link counters were not read in these runs.

Throughput of a bulk endpoint on the same setup, with the wait policy and the target reading all the time: 262,100 bytes to the target in 1.38 s (about 190 kB/s), nothing lost; 200 packets of 64 bytes from the target in 47 to 63 ms (about 200 to 270 kB/s). Not characterized further: the target's USB stack is the guest's.

## How to measure

Three distinct measurements answer three different questions. Keep them separate so host CPU overhead, USB frame phase, and cross-machine clock offsets are never conflated.

### (a) Library CPU cost per call (nanoseconds, no hardware)

A micro-benchmark against the simulated board (`tests/sim/fake_board.c`) or a pseudo-terminal (`posix_openpt` / `openpty`) measures the host library's CPU work per report in **nanoseconds** without any physical hardware.

- **How to build one:** Open a pseudo-terminal pair, run the simulated board (`fake_board.c`, or a background thread that completes the open/commit handshake and drains the master fd with `read(2)`), open a keyboard or mouse node with `mimhid_node_open`, and in a loop time `mimhid_keyboard_key` + `mimhid_node_submit` using `clock_gettime(CLOCK_MONOTONIC, &ts)`.
- **What it shows:** The CPU cost inside `libmimhid` on the host: state mutation, report serialization into the stack buffer, optional `validate_reports` checking, CRC-16/CCITT-FALSE framing, locking `tx_mutex_`, and the `write(2)` call into the pty. No such figure is recorded here.
- **What it cannot show:** UART wire transmission time, USB-UART bridge packetization or latency timers, MCU DMA/SPSC hand-off, USB interrupt endpoint polling intervals (`bInterval`), or the target OS input stack.

### (b) Send to target kernel on the same host (microseconds)

Plugging **both** the USB-UART bridge and the board's USB device cable into the **same** Linux host measures true end-to-end delivery from the library call to the target kernel's input subsystem against a single hardware clock.

- **How to run it:**
  1. Open the board's newly enumerated `/dev/input/eventX` node on the host, set its timestamp clock to `CLOCK_MONOTONIC` with `int clk = CLOCK_MONOTONIC; ioctl(ev_fd, EVIOCSCLOCKID, &clk);`, and grab it with `ioctl(ev_fd, EVIOCGRAB, 1)` so test inputs do not type into the desktop session.
  2. Space injections by an irregular interval (for example, `8.1 ms` to `9.3 ms`) so submissions do not phase-lock to the 1 ms USB frame clock.
  3. Record `t0` with `clock_gettime(CLOCK_MONOTONIC, &t0)` immediately before `mimhid_node_submit`, read the matching `struct input_event` from `ev_fd`, and compute `(ev.time.tv_sec - t0.tv_sec) * 1e6 + ev.time.tv_usec - t0.tv_nsec / 1000`.
  4. Report the distribution in **microseconds** (`p50`, `p90`, `p99`, `min`, `max`) alongside the sample count `N` (for example, `N = 500` presses and `500` releases, as in [Measured, once](#measured-once) above) and verify with `mimhid_device_link_counters` that zero frames were dropped.
- **Why microseconds (not nanoseconds):** A USB Full-Speed interrupt endpoint is polled once per `1 ms` (`1,000 µs`) frame (`125 µs` microframes at High-Speed). Because the submission time is uniformly distributed across the `1,000 µs` poll window, the USB frame phase alone introduces `0` to `1,000 µs` of variance (`500 µs` mean); reporting nanosecond digits on a millisecond-quantized bus is meaningless.
- **What it shows:** The complete physical pipeline—host library, kernel tty/usb-serial driver, USB-UART adapter, UART wire, MCU firmware core, TinyUSB device stack, USB host controller, and Linux `usbhid` / `evdev` driver, timed against one clock, so no offset between two machines' clocks enters the figure.
- **What it cannot show:** Latency into a separate target OS (Android, Windows, macOS, or a console) or application/compositor rendering latency above `evdev`.

### (c) Comparing against `libaoahid` on the same phone target

When the target is a separate device such as an Android phone, the host PC and the phone do not share a `CLOCK_MONOTONIC` epoch.

- **How to compare fairly:** Use the **same** phone, the **same** USB port/cable topology, and the **same** target-side timestamping method (such as `adb shell getevent -lt` or a small target agent recording `CLOCK_MONOTONIC` on input receipt) for both `libaoahid` (PC USB directly to phone in AOA mode) and `libmimhid` (PC UART to MCU board, MCU board USB via OTG adapter to the phone), running the two trials back-to-back.
- **What it shows:** The relative latency **difference** (`latency_mimhid - latency_aoahid`) when the host and phone clocks are compared against a common synchronization reference or over a short back-to-back window where clock drift is negligible; the unknown constant offset between the PC clock and the phone clock cancels **only in a difference**.
- **What it cannot show:** Absolute one-way latency in microseconds from the PC to a separate target machine. **Measuring absolute cross-machine latency requires a hardware USB protocol analyzer** (or a shared hardware GPIO pulse / logic analyzer probing both the host UART TX line and the target's response).

## Host serial latency

Many USB-UART bridges hold received bytes until a latency timer expires (`udev/51-mimhid.rules` gives 16 ms as the FTDI default). The timer delays only the direction from the adapter to the PC: the board's answers (`ACK`, `PONG`), events, which the host waits for while it configures the board, and the link test. Reports go the other way (PC to adapter to board) and are not held by it.

Linux: `libmimhid` sets `ASYNC_LOW_LATENCY` with `TIOCGSERIAL` / `TIOCSSERIAL` when it opens the serial port (`serial_posix.cpp`). It can also be set manually:

```bash
setserial /dev/ttyUSB0 low_latency
```

For FTDI devices the timer is also exposed as `/sys/bus/usb-serial/devices/ttyUSB0/latency_timer`; `udev/51-mimhid.rules` sets it to 1 ms for the FTDI product IDs `6001` and `6014` when they are plugged in.

Windows: Device Manager > the COM port > Port Settings > Advanced > Latency Timer, set to 1 ms.

Whether these settings apply depends on the adapter and driver; none has been measured here.

## Allocation and locking

- The host serializes a report into a stack buffer (`c_api_send.cpp`) and parses incoming frames in a fixed-capacity ring (`FrameParser`); profile mutation calls change fixed-size state.
- On the board, the receive side copies each report from the UART ring into its interface's single-producer single-consumer queue; the USB side takes it from there and sends it. On the RP2040 and RP2350 these run on separate cores; the other ports run both in one loop. The queues use acquire and release atomics and no lock.
- Configuration changes (`mimhid_node_open`, `mimhid_node_close`, `mimhid_device_commit`) are not on this path: they re-enumerate the USB device, and the target enumerates it after the call has returned (`mimhid_device_wait_ready`).
- When a queue is full the new report is dropped; an old one is not overwritten (a generic IN endpoint may choose `DROP_OLDEST`, which discards on the consuming side, or `WAIT`, which makes the writer block on the host).
- A target that floods an OUT endpoint makes the board send an `EP_OUT` frame for every packet, so the board's USB side runs no faster than the serial link carries them (a packet of n bytes takes n + 5 byte times; see [PROTOCOL.md](PROTOCOL.md#wire-time)). This is arithmetic from the frame format, not a measurement.

## Higher polling rates

Full-Speed USB is limited to 1000 Hz. A High-Speed capable MCU could poll faster, but the firmware has no High-Speed port yet. See [PORTING.md](PORTING.md).
