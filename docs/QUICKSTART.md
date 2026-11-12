# Quickstart Guide

Set up `libmimhid` on Linux, macOS, or Windows with a microcontroller board that has a UART and a USB device port. The Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) have released firmware images; no release is hardware-verified ([TARGET_MATRIX.md](TARGET_MATRIX.md)).

You need:

- a board from [TARGET_MATRIX.md](TARGET_MATRIX.md) with its USB port reachable by a cable (Raspberry Pi Pico or Pico 2 recommended)
- a USB-to-UART bridge at 3.3 V logic and three jumper wires. `udev/51-mimhid.rules` lists the Silicon Labs CP210x, FTDI FT232, WCH CH340/CH341, and Prolific PL2303 bridges; the one recorded measurement ([LATENCY.md](LATENCY.md#measured-once)) used a PL2303, and no other bridge has a recorded run
- a USB cable from the board to the target device (an OTG adapter when the target is a phone)
- the toolchain for the firmware you build (see [BUILDING.md](BUILDING.md#building-the-firmware))

---

## 1. Wire the UART bridge

| USB-UART bridge | Function |
| :--- | :--- |
| **RXD** | board TX (board to host) |
| **TXD** | board RX (host to board) |
| **GND** | common ground |

Board pins:

| Board | Board TX | Board RX | USB device port |
| :--- | :--- | :--- | :--- |
| RP2040 (Raspberry Pi Pico) / RP2350 (Raspberry Pi Pico 2) | GP0 (UART0 TX) | GP1 (UART0 RX) | the board's USB connector |
| nRF52840 | `UART_TX_PIN` of the TinyUSB board definition (P0.06 on the nRF52840 DK) | `UART_RX_PIN` (P0.08 on the DK) | the nRF USB connector |
| STM32F4 (Black Pill) | PA2 (USART2 TX) | PA3 (USART2 RX) | the board's USB connector (OTG FS: PA11, PA12) |
| ESP32-S3 | GPIO17 | GPIO18 | native USB (GPIO19 D-, GPIO20 D+), not the UART or JTAG USB port |

Do not connect the bridge's 5 V or 3V3 pin. The board is powered over its own USB cable. The pins are stated at firmware build time (`-DMIMHID_UART_TX_PIN=<pin> -DMIMHID_UART_RX_PIN=<pin>`, and `-DMIMHID_UART_ID=uart0` on the Pico boards); the table above is the wiring of the released images. The nRF52840 and STM32F4 builds take them from the BSP board you select with `-DBOARD=`. The rate is not a build setting: the board starts at 115,200 baud and the library agrees the fastest rate the bridge carries.

---

## 2. Get the firmware and flash it

Each release has firmware images built by `.github/workflows/release.yml` with the wiring above: `mimhid_firmware-rp2040-X.Y.Z.uf2` (Raspberry Pi Pico), `mimhid_firmware-rp2350-X.Y.Z.uf2` (Pico 2), `mimhid_firmware-nrf52840-X.Y.Z.hex` / `.bin`, `mimhid_firmware-stm32f4-X.Y.Z.hex` / `.bin`, and `mimhid_firmware-esp32s3-X.Y.Z.bin` (a merged image). To build one yourself, follow [BUILDING.md](BUILDING.md#building-the-firmware); a source build names its output `mimhid_firmware.uf2` (Pico boards), `mimhid_firmware_nrf52840.hex` / `.bin`, or `mimhid_firmware_stm32f4.hex` / `.bin`.

| Board | Flash |
| :--- | :--- |
| RP2040 (Raspberry Pi Pico) / RP2350 (Raspberry Pi Pico 2) | Hold **BOOTSEL** while plugging the board into the PC. It appears as a USB drive; copy the `.uf2` file to it and the board reboots. |
| nRF52840 | Program the `.hex` file with your debug probe (for example `nrfjprog --program <file> --chiperase --verify --reset`, or OpenOCD / pyOCD). |
| STM32F4 | Program the `.bin` file at `0x08000000` with an ST-Link (`st-flash write <file> 0x08000000`) or the ROM DFU bootloader (`dfu-util -a 0 -s 0x08000000:leave -D <file>`). |
| ESP32-S3 | Write the merged image at `0x0`: `esptool.py --chip esp32s3 write_flash 0x0 <file>`, or `idf.py flash` from a source build. |

The firmware keeps USB disconnected until the host library opens the first node (`mimhid_node_open`), so the board does not show up on the target before then. Calling `mimhid_device_disconnect()` or closing the device detaches it from the bus (unless `keep_attached` is set).

---

## 3. Give your user access to the serial port

### Linux

```bash
sudo cp udev/51-mimhid.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Log in again if the port is still not accessible. The rules use the `uucp` group (the `dialout` group on Debian and Ubuntu: change `GROUP` in the file).

### macOS

No permissions are needed. The bridge appears as `/dev/cu.usbserial-*`, `/dev/cu.usbmodem*`, `/dev/cu.SLAB_USBtoUART`, or `/dev/cu.wchusbserial*`; discovery lists these. CP210x and CH340 bridges need the vendor driver on some macOS releases; FTDI and CDC-ACM devices work with the built-in drivers.

### Windows

No permissions are needed. The bridge appears as a COM port (`COM3`); discovery lists USB-backed ports and `mimhid_device_open` accepts the port path directly (e.g., `"COM3"`, or `"\\\\.\\COM10"` for COM10 and above). Install the bridge vendor's driver if the port does not appear in Device Manager. To reduce latency, set the port's latency timer to 1 ms (Device Manager > Port Settings > Advanced) for FTDI and CP210x bridges.

---

## 4. Build the library and check the link

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMIMHID_BUILD_TESTS=ON -DMIMHID_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

Run the C example. It discovers the board, opens a keyboard and a mouse node, commits, types `a`, moves the mouse, and clicks on the target:

```bash
build/examples/basic_keyboard_mouse          # Linux, macOS
build\bin\Release\basic_keyboard_mouse.exe   # Windows (Visual Studio generator)
```

`mimhid_discover` reports the port, the bridge description, the firmware version and limits (`max_interfaces` (of every class), `max_report_bytes`, `max_descriptor_bytes` (the answer memory)) the board sent in its answer, its chip and unique id, and the fastest rate the link carried without loss (`baud_rate`). Opening with `MIMHID_BAUD_FASTEST` instead measures it then (a second or two); passing the reported `baud_rate` is quicker.

---

## 5. Run the programs made to be watched

`examples/c/verify/` holds small programs for a person to watch. Each prints what to prepare, waits three seconds, then does one clearly described thing. They are built with the examples (`-DMIMHID_BUILD_EXAMPLES=ON`) as `mimhid_verify_<name>`.

| Program | Prepare on the target | Expected result |
|---|---|---|
| `mimhid_verify_keyboard` | Open a text field | The text `hello from libmimhid` typed out, three times |
| `mimhid_verify_mouse` | Nothing | The cursor moves in a circle for a few seconds; a touch-first target may show no cursor, and a clean exit then means the reports were accepted |
| `mimhid_verify_touch` | Have any screen open | A left-right drag, repeated five times |
| `mimhid_verify_toggle` | Unlock the target, put a media app in the foreground | Play/Pause, Volume Increment, Mute and "AC New" fire in turn; only Consumer Control Usages are sent (the comment at the top of the file says why System Control is left out) |
| `mimhid_verify_all` | Open a text field | Touch circles, `hello world` typed and mouse circles, one profile at a time for two rounds, then all three interleaved |

```bash
build/examples/mimhid_verify_keyboard
build/examples/mimhid_verify_touch
build/examples/mimhid_verify_mouse
build/examples/mimhid_verify_toggle
build/examples/mimhid_verify_all
```

Each uses the first board `mimhid_discover` finds, with the pid.codes test USB identity (`1209:0001`) that its code sets. That pair is for testing in-house only: the library fills in no identity, and a vendor ID of zero is refused.

---

## Troubleshooting

| Symptom | Likely cause |
| :--- | :--- |
| `No mimhid board found.` | TX and RX swapped, no common ground, board not flashed, or no access to the port (Linux: udev rules and group; Windows: bridge driver). |
| `MIMHID_ERR_ACCESS` | The port cannot be opened: no permission, wrong name, or another program holds it. |
| `MIMHID_ERR_TIMEOUT` from `mimhid_device_wait_ready` | The target did not enumerate the board: check the USB cable, OTG adapter, and that the target supplies USB host mode. |
| `MIMHID_ERR_BUSY` on an input call | The board is enumerating again after a node was opened or closed (wait with `mimhid_device_wait_ready`), the node was opened in a batch that has not ended, or the node's last report is still being sent. |
| `MIMHID_ERR_PROTOCOL` on open | The board runs firmware of another major or minor version than the library (the board reports its own in `mimhid_device_info.version_*`, and `firmware_compatible` is 0). Flash the firmware that matches the library. |
| `MIMHID_ERR_OVERFLOW` on `mimhid_node_open` | The board has no interface, endpoint, control rule, answer memory or queue slot left for this node (see [LIMITS.md](LIMITS.md)). |
| A held control transfer never completes (the target retries or gives up) | The rule's `timeout_ms` is longer than the target waits, or nothing calls `mimhid_device_control_reply`. Keep the time short and answer from the event sink. |
| `MIMHID_ERR_UNSET_FIELD` from `mimhid_device_set_control_rules` | A rule has `action` at zero, or an `MIMHID_CONTROL_ASK` rule has `timeout_ms` or `when_late` at zero. All are required. |
| Discovery works with other devices attached | Discovery sends a 5-byte ping to every USB serial port (`/dev/ttyUSB*` and `/dev/ttyACM*` on Linux, `/dev/cu.usb*` on macOS, USB COM ports on Windows) at the boot rate and, when nothing answers, a 10 ms BREAK and another ping; a device that does not answer is skipped, and a port another program has open is not touched. |
