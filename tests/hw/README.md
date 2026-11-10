# Hardware tests

The `unit`, `scripted`, and `sim` layers run without hardware (see [`../README.md`](../README.md)). This page is the
procedure for the fourth layer (`hw`): a real board, a real serial adapter, a real target machine. Nothing
here runs by itself, and nothing in CI stands in for it. A release is "hardware-verified" only after
this procedure has been followed for it and the result recorded as below.

## What the simulated board cannot show

The sim tests run the real firmware core against the real host library, but over a pseudo-terminal
and with TinyUSB's device stack compiled for the host. They cannot show:

- **USB electrical behaviour**: attach and detach, the 0 V wait before enumeration, VBUS, cable and
  hub effects, suspend and resume, remote wakeup reaching a sleeping host.
- **Enumeration on real operating systems**: whether Linux, Windows, macOS, Android or a console
  accepts the descriptors, binds the HID driver, and delivers the events to applications.
- **The chip's drivers and timing**: the UART peripheral, its FIFO and DMA, the clock that sets the
  achievable baud rates, the second core's hand-off, the USB controller of each chip.
- **Real serial adapters**: their drivers, their USB frame (1 ms at Full-Speed, 125 µs at
  High-Speed), BREAK support, which rates they really carry, and what they do when unplugged.

## Equipment

| Item | Notes |
|---|---|
| Board | RP2040 (e.g. Raspberry Pi Pico) and RP2350 (e.g. Pico 2). Other ports when their firmware changes. |
| Serial adapter | A USB-UART bridge wired as in [`docs/QUICKSTART.md`](../../docs/QUICKSTART.md). Record its chip (FT232H, CP2102N, PL2303, CH343, ...). Test one that can send a BREAK, and one that cannot if the change touches the handshake. |
| Host | The machine running the library, with the release under test built from source. |
| Target | A second machine the board plugs into. Linux at least; Windows and macOS when descriptors, enumeration or the control path changed. Record its OS and version. |

## Steps

Run them in order; stop at the first failure and record it.

1. **Build and flash.** Build the release tag's firmware for the board
   ([`docs/QUICKSTART.md`](../../docs/QUICKSTART.md), section 2) and flash it. Build the host
   library at the same tag with tests and examples:
   ```sh
   cmake -S . -B ../../build/libmimhid_hw -DCMAKE_BUILD_TYPE=Release \
         -DMIMHID_BUILD_TESTS=ON -DMIMHID_BUILD_EXAMPLES=ON
   cmake --build ../../build/libmimhid_hw
   ctest --test-dir ../../build/libmimhid_hw
   ```
2. **The board stays off the bus.** Connect the board to the target and the adapter to the host.
   Before any program runs, the target must see no USB device from the board.
3. **Discovery.** Run `../../build/libmimhid_hw/examples/basic_keyboard_mouse`. It must find the
   board, report the firmware version of this release, and type `a`, move and click on the target.
   Note the `baud_rate` the search chose.
4. **Every profile.** Run each `mimhid_verify_*` program
   ([`docs/QUICKSTART.md`](../../docs/QUICKSTART.md), section 5) and watch the target. Each has one
   expected result; anything else is a failure.
5. **Re-enumeration.** Run a profile example from `examples/c/profiles/` twice in a row without
   unplugging anything: the target must see the old device leave and the new one arrive.
6. **Unplug.** While `mimhid_verify_all` runs, pull the adapter's USB cable. The program must end
   with `MIMHID_ERR_NO_DEVICE` and not hang. Plug it back and run step 3 again.
7. **A crashed program.** Kill a running example (`kill -9`) after it set a fast rate, then run
   step 3 again. With an adapter that sends a BREAK it must find the board without a power cycle.
8. **Link counters.** After the runs, read `mimhid_device_link_counters` from a short program
   that opens the board (no example prints them yet). They must show no CRC, framing or overrun
   errors.
9. **Latency (when the transport, the firmware's report path or the pacing changed).** Measure as
   described under "Measured, once" in [`docs/LATENCY.md`](../../docs/LATENCY.md) and record the
   same columns.

## What to record, and where

- **`CHANGELOG.md`**: in the release's section, the line that says whether it is hardware-verified.
  When it is: the date, the board(s), the adapter chip(s), the target OS(es) and version(s), and the
  steps run, for example "Hardware-verified on 2026-11-20: RP2040 and RP2350, FT232H and PL2303,
  targets Linux 6.12 and Windows 11 24H2, steps 1 to 8." When a step failed or was skipped, say
  which and why. Without a run the line stays "Not hardware-verified".
- **`docs/LATENCY.md`**: step 9's results go into the measured table, with the date, the library
  version, the board, the adapter, the rate, the setup, and the number of samples, as the existing
  entry does. A figure that was not measured stays out of the table.
- Failures that are bugs become issues, with the step, the equipment and the program's output.

## Using this layout elsewhere

The page has four parts that do not depend on libmimhid: what the simulation cannot show, the
equipment, numbered steps that stop at the first failure, and where the result is recorded. A
sibling library (libaoahid, with an Android phone as the target and no serial adapter) can keep the
headings and replace the rows and the steps.
