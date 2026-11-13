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
   library at the same tag with tests and examples, from the top of a clone:
   ```sh
   cmake -S . -B build/hw -DCMAKE_BUILD_TYPE=Release \
         -DMIMUSB_BUILD_TESTS=ON -DMIMUSB_BUILD_EXAMPLES=ON
   cmake --build build/hw
   ctest --test-dir build/hw
   ```
2. **The board stays off the bus.** Connect the board to the target and the adapter to the host.
   Before any program runs, the target must see no USB device from the board.
3. **Discovery.** Run `build/hw/examples/basic_keyboard_mouse`. It must find the
   board, report the firmware version of this release, and type `a`, move and click on the target.
   Note the `baud_rate` the search chose.
4. **Every profile.** Run each `mimusb_verify_*` program
   ([`docs/QUICKSTART.md`](../../docs/QUICKSTART.md), section 5) and watch the target. Each has one
   expected result; anything else is a failure.
5. **Re-enumeration.** Run a profile example from `examples/c/profiles/` twice in a row without
   unplugging anything: the target must see the old device leave and the new one arrive.
6. **Unplug.** While `mimusb_verify_all` runs, pull the adapter's USB cable. The program must end
   with `MIMUSB_ERR_NO_DEVICE` and not hang. Plug it back and run step 3 again.
7. **A crashed program.** Kill a running example (`kill -9`) after it set a fast rate, then run
   step 3 again. With an adapter that sends a BREAK it must find the board without a power cycle.
8. **Link counters.** After the runs, read `mimusb_device_link_counters` from a short program
   that opens the board (no example prints them yet). They must show no CRC, framing or overrun
   errors.
9. **Latency (when the transport, the firmware's report path or the pacing changed).** Measure as
   described under "Measured, once" in [`docs/LATENCY.md`](../../docs/LATENCY.md) and record the
   same columns.

## Programs for a target that is the same machine

A second machine is what the steps above ask for. When the board's USB port and the serial
adapter are both plugged into one Linux machine, the programs in this directory check a good part
of the same ground without a person watching the target, and without typing into the session:
they present vendor-defined devices, or grab the input device before a key is sent.

```sh
cmake -S tests/hw -B <build dir> && cmake --build <build dir>
sudo cp tests/hw/99-mimusb-hwtest.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```

The rule lets the group of the serial ports read the USB node, the hidraw node and the input
events of the device the programs present (the pid.codes test pair, `1209:0001`); edit the group
name in it for your distribution. The programs find the board themselves (`mimusb_discover`), so
only one board may be connected.

| Program | What it shows |
|---|---|
| `hw_bulk wait` | A vendor Node with bulk IN and OUT: transfers of 1 to 292 bytes with their boundaries, a zero-length packet, writes of up to 262,100 bytes without loss under the wait policy, 200 packets from the target, stall and clear. |
| `hw_bulk newest`, `hw_bulk oldest` | The two drop policies with a target that does not read: which transfers it then reads, and that the lost ones are reported. |
| `hw_control_rules` | An HID interface written by hand: the report descriptor the kernel read, `ANSWER_LAST_SENT`, an `ANSWER` and its bytes replaced, `ACCEPT` with a data stage, `ASK`, a stall for a request no rule matches, an Output report on the OUT endpoint. |
| `hw_composite` | A keyboard profile and a two-interface Node with an interface association, opened in one batch. |
| `hw_bytes bytes` | A device given as bytes with a BOS descriptor: the target reads every descriptor and two strings back byte for byte, a vendor request is answered by a rule of the Device, bulk IN and OUT, Halt. |
| `hw_bytes bos` | A composed Device with `bcdUSB` 0x0210 and a BOS given with `mimusb_device_set_bos`: the target reads it back. |
| `hw_iso` | A Node with an isochronous IN endpoint in setting 1 and none in setting 0: the endpoint counts of both settings from the descriptors, `SET_INTERFACE` to each (reported as `INTERFACE_ALTERNATE` with the Node's id), 100 packets of 192 bytes read as isochronous transfers in order and whole. |
| `hw_cdc` | A CDC-ACM function from `mimusb_spec_create_cdc_acm`: the `cdc_acm` driver's `/dev/ttyACM*` appears, setting its baud rate reaches the program as `SET_LINE_CODING`, bytes go both ways. |
| `hw_msc` | A mass storage function (class 0x08, SCSI, Bulk-Only): the program is a 2 MiB RAM disk answering SCSI. The kernel's `usb-storage` finds a disk of that size (`/sys/block`); then, as initiator with libusb, INQUIRY, READ CAPACITY, 64-block and 4 KiB WRITE(10)/READ(10) compared byte for byte, a stalled Bulk-In with a failed CSW and REQUEST SENSE, and the Bulk-Only Mass Storage Reset. |
| `hw_ecm` | A network adapter (CDC-ECM): a data interface with no endpoints in setting 0, the MAC address as a string, the notification endpoint. The kernel's `cdc_ether` makes an interface with that address and MTU 1500; then, with libusb, setting 1, NETWORK_CONNECTION, an ARP request and ICMP echo of frames from 60 to 1514 bytes (among them exact multiples of 64, ended by a zero-length packet) answered by the program, and 200 full-size round trips. |
| `hw_dfu` | A DFU 1.1 device in DFU mode, with no endpoint: every request is a control rule with a data stage. DNLOAD, CLRSTATUS and ABORT are `ACCEPT` rules; GETSTATUS, GETSTATE and UPLOAD are `ASK` rules answered by the program's state machine. With libusb: the functional descriptor, 16 blocks of 64 bytes each followed by GETSTATUS, the zero-length block and the manifestation states, an upload that gives the image back, a stall for a request the table does not name. |
| `hw_printer` | A bidirectional printer (class 7): the kernel's `usblp` binds it, when loaded. With libusb: GET_DEVICE_ID and GET_PORT_STATUS (`ANSWER` rules), SOFT_RESET (`ACCEPT`), a print job of 200,000 bytes on bulk OUT whose end marker the program finds, and its count and checksum answered on bulk IN. |
| `hw_midi` | A USB MIDI 1.0 device (an Audio Control and a MIDIStreaming interface, four jacks, two bulk endpoints with 9-byte descriptors): the kernel's `snd-usb-audio` makes a sound card. With libusb: 4,000 event packets written in 250 transfers and the same 4,000 read back in order. |
| `hw_keyboard_latency [presses]` | Step 9: submit to evdev event for a keyboard profile, with the event device grabbed. Compare versions over several runs each: one run's median moves by some 0.3 ms from one enumeration to the next. |

Each prints `PASSED` or what failed and returns nonzero on a failure. They do not replace the
steps above: the target's USB stack is this machine's, nothing is checked on Windows or macOS,
and the `mimusb_verify_*` programs are not run by them.

Two things learned while writing them. A read on the target side that is cancelled by its
timeout while a transfer is arriving can lose the packets the host controller had already taken
(seen in a VMware guest with a 50 ms timeout: data the board had sent correctly was gone), so
the programs read with a long timeout. And a board can be written again without its button:
`mimusb_device_enter_bootloader` restarts it into the chip's bootloader.

## What to record, and where

- **`CHANGELOG.md`**: in the release's section, the line that says whether it is hardware-verified.
  When it is: the date, the board(s), the adapter chip(s), the target OS(es) and version(s), and the
  steps run, in the form "Hardware-verified on <date>: <board(s)>, <adapter chip(s)>, targets
  <OS and version>, steps <numbers>." When a step failed or was skipped, say
  which and why. Without a run the line stays "Not hardware-verified".
- **`docs/LATENCY.md`**: step 9's results go into the measured table, with the date, the library
  version, the board, the adapter, the rate, the setup, and the number of samples, as the existing
  entry does. A figure that was not measured stays out of the table.
- Failures that are bugs become issues, with the step, the equipment and the program's output.
