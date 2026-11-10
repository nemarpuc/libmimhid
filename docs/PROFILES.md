# Profiles

Every generated `mimhid_spec` pairs a USB HID 1.11 report descriptor with a
bit-exact report serializer and state machine. This page states what each of the
nine profile kinds and the optional battery report emit, what options they
require and reject, and how their state machines behave across
`mimhid_node_submit`, `mimhid_node_submit_blocking`, and `mimhid_node_close`.

## Hardware status

- **RP2040 verified on 2026-11-07** on a Raspberry Pi Pico wired to a Prolific
  PL2303 USB-UART adapter against a Linux 7.2 target: keyboard (with Caps Lock
  LED Output report echo), mouse (buttons, X/Y, wheel, pan), 10-contact
  touchscreen (with `Contact Count Maximum` Feature read), touchpad, pen (hover,
  tip, pressure, tilt, barrel switch, eraser transition), gamepad (hat, buttons,
  axes), and Consumer Control media keys took effect in the 7-interface
  `verify_all` run, and 15 simultaneous mouse interfaces ran at `7,812,500` baud
  with zero link errors ([TARGET_MATRIX.md](TARGET_MATRIX.md)).
- **Release 0.2.0 itself is not hardware-verified:** the `*_init` contract and
  wire protocol `0.2.0` changed after the 2026-11-07 hardware run
  ([CHANGELOG.md](../CHANGELOG.md)).
- **Not tested on hardware:** System Control, Camera, and Telephony toggles,
  battery reports, `mimhid_spec_create_raw`, and all non-RP2040 boards and
  non-Linux targets ([VERIFICATION.md](VERIFICATION.md)).

## Rules shared by every profile

### Zero-only initializers (`*_init`)

Every `mimhid_*_options_init` function zeroes the structure and sets
`struct_size = sizeof(*options)`. Unlike `libaoahid` 0.1.0, `libmimhid` 0.2.0
fills in **no** default Usage range, bit width, contact count, or axis domain.
Leaving a required field zero returns `MIMHID_ERR_UNSET_FIELD` or
`MIMHID_ERR_PARAM`, and `mimhid_last_error()` names the offending field. One
complete set of values for a Linux target lives in
[examples/c/example_values.h](../examples/c/example_values.h).

### Integer fields and physical properties

Numeric fields in profile options use `mimhid_integer_field` (and the helper
`mimhid_integer_field_make(logical_minimum, logical_maximum, bit_width)`):

- `bit_width` must be `1..32` (`0` returns `MIMHID_ERR_UNSET_FIELD`).
- `logical_minimum` and `logical_maximum` must satisfy
  `logical_minimum <= logical_maximum` and fit within `bit_width` bits
  (two's-complement signed when either bound is negative, unsigned when both are
  nonnegative).
- `physical` (`mimhid_physical_properties`) is optional:
  - When `physical.enabled == 0`, `minimum`, `maximum`, `unit_exponent`, and
    `unit` must all be `0` (`MIMHID_ERR_PARAM` otherwise).
  - When `physical.enabled == 1`, `maximum > minimum`, `unit_exponent` is in
    `-8..7`, and `unit` is a valid nonzero HID 1.11 section 6.2.2.7 unit code.
- Whenever an option structure has an `enable_<feature>` flag (`0` or `1`) and
  that flag is `0`, the corresponding `mimhid_integer_field` or pointer/count
  fields must be canonical zero/null.

### Report IDs (`mimhid_report_id_option`)

- When `report_id.enabled == 0`, `report_id.value` must be `0`, and no `Report ID`
  item (`0x85`) is emitted in the descriptor or prefixed on the wire.
- When `report_id.enabled == 1`, `report_id.value` must be `1..255`, and the 1-byte
  Report ID prefixes every wire report of that layout.
- `report_id.reserved8[0..2]` must always be `0`.

### Byte padding between caller-sized fields

HID 1.11 section 8.4 forbids a single field from spanning more than four bytes.
Because field widths are caller-declared (`1..32` bits), the descriptor builder
pads to the next byte boundary with a `Constant` (`Input (Constant)`) item after
button/switch bitmaps and between multi-bit integer fields so no field can start
at a nonzero bit offset and cross a fifth byte.

### Two-pass construction and Linux parser limits

Every `mimhid_spec_create_*` call runs two passes (`counting_builder` then
`emitting_builder`), verifies that both passes produce identical byte lengths and
parser requirements, and runs `validate_generated` on the resulting bytes. It
records five parser maxima on the `Spec`:

1. `descriptor_length` (bytes)
2. `maximum_fields_per_report`
3. `maximum_global_stack_depth`
4. `maximum_report_size_bits`
5. `maximum_usages` and `maximum_report_data_bits`

When you attach the `Spec` to a `Device` via `mimhid_node_open`, those maxima
are checked against the `mimhid_linux_hid_policy` values you supplied in
`mimhid_device_config.transport.linux_hid` and against
`transport.maximum_report_bytes` and the board's `max_report_bytes` and
`max_descriptor_bytes` (`MIMHID_ERR_OVERFLOW` if any limit is exceeded).

### State machines, edge guards, and one report in flight

- Profile mutation calls (`mimhid_keyboard_key`, `mimhid_mouse_move`,
  `mimhid_touchscreen_contact`, and so on) update in-memory state on the
  `mimhid_node` with no heap allocation and no serial I/O.
- `mimhid_node_submit(node, send_timeout_ms)` serializes one pending report and
  writes it to the serial port; `mimhid_node_submit_blocking(node, timeout_ms)`
  loops `mimhid_node_submit` until all pending packets (such as multi-packet
  touch frames, clamped mouse deltas, or a queued battery update) have been sent.
- **Pending-edge guard:** if a key, button, toggle, contact slot, or pen tool
  changes state (for example, `mimhid_keyboard_key(node, 0x04, 1)`), attempting
  to reverse that transition (`mimhid_keyboard_key(node, 0x04, 0)`) *before* at
  least one `mimhid_node_submit` has delivered the first edge returns
  `MIMHID_ERR_BUSY`. This prevents an unsubmitted press-and-release pair from
  collapsing into a no-op before the target ever sees the press.
- **Neutral report on close:** when `mimhid_node_close` or `mimhid_device_close`
  closes a Node that still has pressed keys/buttons, active touch contacts, an
  in-range stylus, or non-neutral gamepad axes/hat, the library synthesizes and
  submits the neutral release report(s) within `drain_timeout_ms` before
  re-enumerating or detaching the board.

### Two-way HID traffic (Output and Feature reports)

Every `mimhid_spec` reports `input_supported = 1`, `output_supported = 1`, and
`feature_transport_supported = 1` in `mimhid_capability_manifest`:

- **Output reports** sent by the target OS (`SET_REPORT(Output)` on endpoint 0
  or an interrupt `OUT` transfer when `ep_out` is configured) arrive at
  `transport.event_sink` as `MIMHID_EVENT_OUTPUT_REPORT`.
- **Feature writes** (`SET_REPORT(Feature)`) arrive at `transport.event_sink` as
  `MIMHID_EVENT_FEATURE_REPORT`.
- **Feature reads** (`GET_REPORT(Feature)`) are answered by the board from the
  byte slice registered with `mimhid_node_set_feature_response()`.

---

## Keyboard

**Factory:** `mimhid_spec_create_keyboard(const mimhid_keyboard_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_KEYBOARD`  
**Mutation:** `mimhid_keyboard_key(node, usage, down)`

### Descriptor layout

Application Collection `Generic Desktop / Keyboard` (`0x0001 / 0x0006`):

1. Optional `Report ID` (`options->report_id`).
2. **Modifier byte:** 8 one-bit `Data, Variable, Absolute` fields on
   `Keyboard/Keypad` (`0x0007`) Usages `0xE0` (Left Control) through `0xE7`
   (Right GUI).
3. **Non-modifier key bitmap:** `usage_maximum - usage_minimum + 1` one-bit
   `Data, Variable, Absolute` fields on `Keyboard/Keypad` (`0x0007`) from
   `options->usage_minimum` through `options->usage_maximum`, padded to the next
   byte boundary with `Constant` bits.

Because every key has its own bit (`Variable`), the profile is natively full
N-key rollover (NKRO) with no 6-key array limit and no `ErrorRollOver` state.

### Option validation

- `usage_minimum >= 0x04` (selectors `0x00..0x03` are reserved HID keyboard
  error codes and are rejected with `MIMHID_ERR_PARAM`).
- `usage_maximum >= usage_minimum`.
- `[usage_minimum, usage_maximum]` must not overlap the modifier range
  `0xE0..0xE7` (`MIMHID_ERR_PARAM`).

### State and LEDs

- `mimhid_keyboard_key(node, usage, down)` accepts either a modifier Usage
  (`0xE0..0xE7`) or a Usage within `[usage_minimum, usage_maximum]`, with
  `down` set to `1` (press) or `0` (release). Reversing an unsubmitted key
  transition returns `MIMHID_ERR_BUSY`.
- **Keyboard LEDs:** `mimhid_spec_create_keyboard` emits an Input-only keyboard
  descriptor. If you want the target OS to send Caps Lock / Num Lock / Scroll
  Lock `Output` reports, declare an `Output` Main item on Usage Page `0x08`
  (LEDs) in a raw descriptor (`mimhid_spec_create_raw` with
  `requires_output = 1` or `mimhid_node_open_raw`, as shown in
  [examples/c/verify/verify_keyboard.c](../examples/c/verify/verify_keyboard.c))
  and handle `MIMHID_EVENT_OUTPUT_REPORT` in `transport.event_sink`.

---

## Mouse

**Factory:** `mimhid_spec_create_mouse(const mimhid_mouse_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_MOUSE`  
**Mutations:** `mimhid_mouse_move(node, dx, dy)`, `mimhid_mouse_scroll(node, wheel, pan)`, `mimhid_mouse_button(node, button, down)`

### Descriptor layout

Application Collection `Generic Desktop / Mouse` (`0x0001 / 0x0002`) wrapping
`Physical (Pointer)` (`0x0001 / 0x0001`):

1. Optional `Report ID`.
2. **Buttons:** `1..button_count` on `Button` page (`0x0009`), 1 bit each,
   `Data, Variable, Absolute`, padded to a byte boundary.
3. **X and Y:** `Generic Desktop / X` (`0x30`) and `Y` (`0x31`),
   `Data, Variable, Relative`, using `options->x` and `options->y`, padded to a
   byte boundary.
4. **Wheel (optional):** when `enable_wheel == 1`, `Generic Desktop / Wheel`
   (`0x38`), `Data, Variable, Relative`, padded to a byte boundary.
5. **Horizontal pan (optional):** when `enable_pan == 1`, `Consumer / AC Pan`
   (`0x000C / 0x0238`), `Data, Variable, Relative`, padded to a byte boundary.

### Option validation

- `button_count` must be `1..65535`.
- `x` and `y` must have `logical_minimum < 0` and `logical_maximum > 0`.
- When `enable_wheel == 1` (or `enable_pan == 1`), `wheel` (or `pan`) must also
  have `logical_minimum < 0` and `logical_maximum > 0`. When disabled (`0`), the
  field must be all zero.

### State and delta accumulation

- `mimhid_mouse_move(node, dx, dy)` and `mimhid_mouse_scroll(node, wheel, pan)`
  add their deltas into 64-bit pending totals (`MIMHID_ERR_OVERFLOW` if a
  64-bit total would overflow, or `MIMHID_ERR_UNSUPPORTED` when a nonzero
  `wheel` or `pan` is passed for a field not enabled in the `Spec`).
- Each `mimhid_node_submit` clamps the pending delta of each axis to the
  declared `[logical_minimum, logical_maximum]` range for that report and
  subtracts what was sent when the write succeeds, keeping any remainder for
  the next report (`mimhid_node_submit_blocking` drains the full remainder).
- `mimhid_mouse_button(node, button, down)` takes a 1-based button number
  (`1..button_count`).

---

## Toggle: Consumer, System, Camera, and Telephony controls

**Factory:** `mimhid_spec_create_toggle(const mimhid_toggle_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_TOGGLE`  
**Mutation:** `mimhid_toggle_usage(node, usage, down)`

### Descriptor layout

Application Collection (`options->application_page`, `options->application_usage`):

- Optional `Report ID`.
- One 1-bit `Variable` field on `options->field_page` for each entry in
  `options->allowed_usages[0 .. allowed_usage_count - 1]`, followed by byte
  padding.

Common combinations:

| Control family | `application_page` | `application_usage` | `field_page` | Example usages |
|---|---|---|---|---|
| Consumer Control (media keys) | `0x000C` (Consumer) | `0x0001` (Consumer Control) | `0x000C` | `0x00CD` Play/Pause, `0x00E9` Volume Increment, `0x00EA` Volume Decrement, `0x00E2` Mute |
| System Control | `0x0001` (Generic Desktop) | `0x0080` (System Control) | `0x0001` | `0x0081` Power Down, `0x0082` Sleep, `0x0083` Wake Up |
| Camera Control | `0x0090` (Camera Control) | `0x0001` | `0x0090` | `0x0020` Camera Auto-focus, `0x0021` Camera Shutter |
| Telephony | `0x000B` (Telephony) | `0x0001` (Phone) | `0x000B` | `0x0020` Hook Switch, `0x002F` Phone Mute |

### HUT semantics (`mimhid_usage_semantic`)

Every entry in `allowed_usages` must declare its HUT 1.7 control type in
`usage_semantics[index]` and supply non-empty target evidence strings in
`expected_linux_event_types[index]` and `expected_linux_codes[index]`:

| `mimhid_usage_semantic` | HUT abbreviation | Accepted? | Emitted HID Main item flags |
|---|---|---|---|
| `MIMHID_USAGE_SELECTOR_BITMAP` | `Sel` (1-bit bitmap) | Yes | `Data, Variable, Absolute, Preferred State` |
| `MIMHID_USAGE_MOMENTARY` | `MC` | Yes | `Data, Variable, Absolute, Preferred State` |
| `MIMHID_USAGE_RETRIGGER` | `RTC` | Yes | `Data, Variable, Absolute, Preferred State` |
| `MIMHID_USAGE_ON_OFF_TOGGLE` | `OOC` (toggle) | Yes | `Data, Variable, Relative, Preferred State` |
| `MIMHID_USAGE_ONE_SHOT` | `OSC` | Yes | `Data, Variable, Relative, Preferred State` |
| `MIMHID_USAGE_ON_OFF_MAINTAINED` | `OOC` (`0`/`1`) | Yes | `Data, Variable, Absolute, No Preferred` |
| `MIMHID_USAGE_ON_OFF_PAIR` | `OOC` (`-1`/`1`) | No (`MIMHID_ERR_UNSUPPORTED`) | Requires a 2-bit/signed value API |
| `MIMHID_USAGE_LINEAR` | `LC` | No (`MIMHID_ERR_UNSUPPORTED`) | Requires a multi-bit value API |
| `MIMHID_USAGE_DYNAMIC_VALUE` | `DV` | No (`MIMHID_ERR_UNSUPPORTED`) | Requires a multi-bit value API |
| `MIMHID_USAGE_NAMED_ARRAY` | `NAry` | No (`MIMHID_ERR_UNSUPPORTED`) | Requires a collection/selector API |

### Option validation

- `allowed_usage_count` must be `1..65535`.
- Every `allowed_usages[i]` must be nonzero, unique, and (when
  `application_page == field_page`) distinct from `application_usage`.
- When `field_page == 0x0090` (Camera Control), only `0x0020` (Auto-focus) and
  `0x0021` (Shutter) are valid, and `usage_semantics[i]` must be
  `MIMHID_USAGE_ONE_SHOT` (HUT 1.7 Table 35.1).

---

## Gamepad

**Factory:** `mimhid_spec_create_gamepad(const mimhid_gamepad_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_GAMEPAD`  
**Mutations:** `mimhid_gamepad_button(node, button, down)`, `mimhid_gamepad_axis(node, axis_index, value)`, `mimhid_gamepad_dpad(node, up, down, right, left)`

### Descriptor layout

Application Collection `Generic Desktop / Gamepad` (`0x0001 / 0x0005`):

1. Optional `Report ID`.
2. **Buttons:** `button_usage_minimum .. button_usage_minimum + button_count - 1`
   on `Button` (`0x0009`), 1 bit each, padded to a byte boundary.
3. **D-pad (optional):**
   - `MIMHID_DPAD_NONE`: nothing emitted.
   - `MIMHID_DPAD_HAT`: 4-bit `Generic Desktop / Hat switch` (`0x39`), logical
     `0..7`, physical `0..315` degrees (`unit = 0x14`), `Null State` enabled,
     padded to a byte boundary.
   - `MIMHID_DPAD_BUTTONS`: four 1-bit `Generic Desktop` fields (`0x90` D-pad Up,
     `0x91` Down, `0x92` Right, `0x93` Left) with `No Preferred`, padded to a
     byte boundary.
4. **Axes:** each `options->axes[i]` emitted in array order as `Data, Variable,
   Absolute` on its audited Usage Page and Usage, each followed by byte padding.

### Audited axis roles (`mimhid_axis_role`)

Every axis entry in `options->axes` must match its HUT 1.7 Usage Page and Usage
and provide a non-empty `expected_linux_code`:

| `mimhid_axis_role` | `usage_page` | `usage` | Required range / `neutral_value` rule |
|---|---:|---:|---|
| `MIMHID_AXIS_X` | `0x0001` | `0x0030` | `neutral_value` in `[logical_minimum, logical_maximum]` (required on every gamepad) |
| `MIMHID_AXIS_Y` | `0x0001` | `0x0031` | `neutral_value` in `[logical_minimum, logical_maximum]` (required on every gamepad) |
| `MIMHID_AXIS_Z` | `0x0001` | `0x0032` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_RX` | `0x0001` | `0x0033` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_RY` | `0x0001` | `0x0034` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_RZ` | `0x0001` | `0x0035` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_SLIDER` | `0x0001` | `0x0036` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_DIAL` | `0x0001` | `0x0037` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_WHEEL` | `0x0001` | `0x0038` | `neutral_value` in `[logical_minimum, logical_maximum]` |
| `MIMHID_AXIS_SIMULATION_RUDDER` | `0x0002` | `0x00BA` | `logical_minimum < 0`, `logical_maximum > 0`, `neutral_value == 0` |
| `MIMHID_AXIS_SIMULATION_THROTTLE` | `0x0002` | `0x00BB` | `logical_minimum == 0`, `logical_maximum > 0`, `neutral_value == 0` |
| `MIMHID_AXIS_SIMULATION_ACCELERATOR` | `0x0002` | `0x00C4` | `logical_minimum == 0`, `logical_maximum > 0`, `neutral_value == 0` |
| `MIMHID_AXIS_SIMULATION_BRAKE` | `0x0002` | `0x00C5` | `logical_minimum == 0`, `logical_maximum > 0`, `neutral_value == 0` |
| `MIMHID_AXIS_SIMULATION_STEERING` | `0x0002` | `0x00C8` | `logical_minimum < 0`, `logical_maximum > 0`, `neutral_value == 0` |

### State mutations

- `mimhid_gamepad_axis(node, axis_index, value)` sets `options->axes[axis_index]`
  (`0 .. axis_count - 1`) to `value`.
- `mimhid_gamepad_button(node, button, down)` presses (`1`) or releases (`0`)
  1-based button `1 .. button_count`.
- `mimhid_gamepad_dpad(node, up, down, right, left)` sets the D-pad from four
  `0`/`1` direction flags. With `MIMHID_DPAD_HAT`, opposing directions (`up && down`
  or `left && right`) are rejected with `MIMHID_ERR_PARAM`, and all-zero encodes
  the Hat Switch Null State (`8`).

---

## Touchscreen

**Factory:** `mimhid_spec_create_touchscreen(const mimhid_touchscreen_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_TOUCHSCREEN`  
**Mutation:** `mimhid_touchscreen_contact(node, contact_id, down, x, y, extra)`

### Descriptor layout

Application Collection `Digitizers / Touch Screen` (`0x000D / 0x0004`):

1. Optional `Report ID`.
2. **Contact Count Maximum Feature (optional):** when
   `enable_contact_count_maximum_feature_declaration == 1`, a `Feature (Data,
   Variable, Absolute)` item on `Digitizers / Contact Count Maximum` (`0x0055`)
   with range `0..maximum_contacts` and width `contact_count.bit_width`. When
   `mimhid_node_open` attaches the `Spec` to a `Device`, `libmimhid`
   automatically registers a `FEATURE` response on the board containing
   `maximum_contacts` so the target OS receives the contact limit during USB
   enumeration without caller intervention.
3. **`contacts_per_report` Finger slots:** each slot is a `Logical (Finger)`
   (`0x000D / 0x0022`) collection containing:
   - `Tip Switch` (`0x0042`), 1 bit, padded to a byte boundary.
   - `Contact Identifier` (`0x0051`), `X` (`0x0001 / 0x0030`), and `Y`
     (`0x0001 / 0x0031`), padded to a byte boundary.
   - Optional `Tip Pressure` (`0x0030`), `Width` (`0x0048`), `Height` (`0x0049`),
     and `Azimuth` (`0x003F`), each padded to a byte boundary.
4. **Scan Time (optional):** when `enable_scan_time == 1` (requires
   `scan_time_unit_100us == 1`), `Digitizers / Scan Time` (`0x0056`) with HID
   Unit `0x1001` (seconds) and Unit Exponent `-4` (`100 µs` steps).
5. **Contact Count:** `Digitizers / Contact Count` (`0x0054`).

### Option validation

- `maximum_contacts` must be `1..16`.
- `contacts_per_report` must be `1..maximum_contacts`. When
  `enable_multi_packet_frames == 0`, `contacts_per_report` must equal
  `maximum_contacts`.
- `contact_id` must include `0` (used for inactive fixed slots) and provide at
  least `maximum_contacts` nonnegative values (`logical_maximum + 1 >= maximum_contacts`).
- `contact_count` must cover `0..maximum_contacts`.
- `x` and `y` must include `0` (`logical_minimum <= 0 <= logical_maximum`).
- When `enable_pressure == 1`, `pressure.logical_minimum == 0` and
  `pressure.logical_maximum >= 1`.
- When `enable_width == 1` (or `enable_height == 1`), the range must include `0`
  and its `physical` unit and unit exponent must match `x.physical` (or
  `y.physical`).
- When `enable_azimuth == 1`, `azimuth.logical_minimum == 0` and
  `azimuth.logical_maximum > 0`.
- When `enable_scan_time == 1`, `scan_time_unit_100us == 1`,
  `scan_time.logical_minimum == 0`, `scan_time.logical_maximum >= 1`, and
  `scan_time.physical` must be disabled (`0`), because the builder supplies the
  mandatory `100 µs` unit items itself.

### Contact state machine, Scan Time, and multi-packet frames

- `mimhid_touchscreen_contact(node, contact_id, down, x, y, extra)` places or
  moves `contact_id` when `down == 1`, or lifts it at `(x, y)` when `down == 0`.
  `extra` (`const mimhid_touchscreen_extra*`) may be `NULL` when optional fields
  are `0` (or disabled).
- Lifting a `contact_id` whose `down == 1` edge has not yet been submitted
  returns `MIMHID_ERR_BUSY`; placing a `contact_id` whose lift has not yet been
  submitted also returns `MIMHID_ERR_BUSY`.
- When `enable_scan_time == 1`, `mimhid_node_submit` computes `Scan Time`
  automatically from `std::chrono::steady_clock` in `100 µs` ticks
  modulo `scan_time.logical_maximum + 1` (rolling the epoch back if the serial
  write fails).
- When `enable_multi_packet_frames == 1` and more than `contacts_per_report`
  contacts are active or lifting, each `mimhid_node_submit` sends one packet of
  the frame (first packet carries the frame's total `Contact Count`, continuation
  packets carry `Contact Count = 0`), and `mimhid_node_submit_blocking` sends all
  packets of the frame.

---

## Touchpad

**Factory:** `mimhid_spec_create_touchpad(const mimhid_touchpad_options*, mimhid_spec**)`  
**Profile kind:** `MIMHID_PROFILE_TOUCHPAD`  
**Mutations:** `mimhid_touchpad_contact(node, contact_id, down, x, y, extra)`, `mimhid_touchpad_button(node, button, down)`

`mimhid_touchpad_options` shares every field and validation rule of
`mimhid_touchscreen_options`, with two additions:

1. The top-level Application Collection is `Digitizers / Touch Pad`
   (`0x000D / 0x0005`), which causes Linux to tag the input device with
   `INPUT_PROP_POINTER` instead of `INPUT_PROP_DIRECT`.
2. `button_count` (`0..65535`) optionally adds clickpad/housing buttons
   (`Button` page `1..button_count`) after `Contact Count`, mutated with
   `mimhid_touchpad_button(node, button, down)`.

---

## Pen and Digitizer

**Factories:** `mimhid_spec_create_pen(const mimhid_pen_options*, mimhid_spec**)`, `mimhid_spec_create_digitizer(const mimhid_digitizer_options*, mimhid_spec**)`  
**Profile kinds:** `MIMHID_PROFILE_PEN`, `MIMHID_PROFILE_DIGITIZER`  
**Mutations:** `mimhid_pen_update(node, sample)` (for Pen), `mimhid_digitizer_update(node, sample)` (for Digitizer)

### Descriptor layout

Application Collection `Digitizers / Pen` (`0x000D / 0x0002`) for Pen, or
`Digitizers / Digitizer` (`0x000D / 0x0001`) for Digitizer, wrapping
`Physical (Stylus)` (`0x000D / 0x0020`):

1. Optional `Report ID`.
2. **Switch bits** (padded to a byte boundary):
   - Optional `Invert` (`0x003C`, 1 bit) when `enable_eraser == 1` — emitted
     **before** `Tip Switch` and `In Range` so the Linux HID input driver has
     already updated its tool selection when it processes `In Range`.
   - `Tip Switch` (`0x0042`, 1 bit).
   - `In Range` (`0x0032`, 1 bit).
   - `0..2` barrel switches from `options->barrel_usages`: only HUT 1.7
     `Barrel Switch` (`0x0044`) and `Secondary Barrel Switch` (`0x005A`) are
     accepted, each at most once.
3. **Coordinates:** `Generic Desktop / X` (`0x30`) and `Y` (`0x31`), padded to a
   byte boundary.
4. **Tip Pressure (optional):** `Digitizers / Tip Pressure` (`0x0030`) when
   `enable_pressure == 1`, padded to a byte boundary.
5. **Tilt (optional):** `Digitizers / X Tilt` (`0x003D`) and `Y Tilt` (`0x003E`)
   when `enable_tilt == 1`, padded to a byte boundary.
6. **Twist (optional):** `Digitizers / Twist` (`0x0041`) when
   `enable_twist_target_specific == 1`, padded to a byte boundary.

### State and tool transitions (`mimhid_pen_sample` / `mimhid_digitizer_sample`)

- `tip == 1` requires `in_range == 1`; when `enable_hover == 0`, `in_range` must
  equal `tip`.
- Bit `i` of `sample->barrel_buttons` sets `options->barrel_usages[i]`.
- When `in_range == 0`, `In Range`, `Tip Switch`, `Invert`, `pressure`, and
  `barrel_buttons` go out as `0` alongside the sample's `x`, `y`, `tilt_x`,
  `tilt_y`, and `twist`.
- Switching `eraser` between `0` and `1` while the stylus is already in range
  automatically inserts an out-of-range transition report first so the target OS
  closes the previous tool before opening the new one (`mimhid_node_submit_blocking`
  sends both reports).

---

## Battery report

Every generated profile option struct ends with `enable_battery` (`0` or `1`)
and `battery` (`mimhid_battery_report`):

- When `enable_battery == 1`, both the profile's primary `report_id` and
  `battery.report_id` must have `enabled == 1` with **different** Report IDs
  (`1..255`), and `battery.strength` must have `logical_minimum >= 0` and
  `logical_maximum > logical_minimum`.
- The builder appends a second Input report at the end of the top-level
  Application Collection on `Generic Device Controls / Battery Strength`
  (`0x0006 / 0x0020`).
- `mimhid_battery_update(node, strength)` records the new battery level; it is
  sent on the next `mimhid_node_submit` (or `mimhid_node_submit_blocking`) when
  no primary Input report is pending. Closing the Node drops an unsent battery
  update and leaves the last sent battery level intact on the target.

---

## Raw

`libmimhid` provides two ways to use a caller-supplied HID report descriptor:

### 1. Validated raw `Spec` (`mimhid_spec_create_raw` + `mimhid_node_open` + `mimhid_raw_submit`)

`mimhid_spec_create_raw(const mimhid_raw_options*, mimhid_spec**)` parses the
descriptor with `validate_raw`:

- `descriptor` (`1..65535` bytes) must have balanced collections, valid Global
  and Local items, and only top-level `Application` collections.
- `reports` (`1..256` `mimhid_raw_report` entries) must list the exact set of
  Input reports (`report_id` and `wire_length`, including the 1-byte Report ID
  prefix when `report_id.enabled == 1`) that the descriptor declares.
- If the descriptor contains `Output` Main items, `options->requires_output`
  must be `1`; if it contains `Feature` Main items,
  `options->requires_feature_response` must be `1` (`MIMHID_ERR_UNSUPPORTED`
  otherwise).
- On every `mimhid_raw_submit(node, report, length, send_timeout_ms)` call, the
  library verifies that `length` and the leading Report ID byte match an entry in
  `reports`, and that any trailing Constant padding bits in the final byte are `0`.

### 2. Unchecked interface descriptor (`mimhid_node_open_raw` + `mimhid_report_send`)

`mimhid_node_open_raw(device, descriptor, descriptor_length, &node_config, &timeouts, &node)`
sends any `1..max_descriptor_bytes` byte slice directly to the board without
running `validate_raw` or checking `transport.linux_hid`. This is used when
replaying a captured physical device descriptor verbatim (including vendor
collections, Output LED items, or non-standard item sequences). Send reports on
that Node with `mimhid_report_send(node, report_bytes, report_length, send_timeout_ms)`.

---

## Checking a profile on a target

1. **Without a board:** compile and run `examples/c/profiles/<profile>.c`
   ([EXAMPLES.md](EXAMPLES.md)) to print the descriptor length, wire byte count,
   and call sequence, or run `ctest -R mimhid.golden-descriptors` to compare all
   generated descriptors byte-for-byte against [tests/golden/](../tests/golden).
2. **With a board on Linux:** run the corresponding program in
   [examples/c/verify/](../examples/c/verify) ([QUICKSTART.md](QUICKSTART.md))
   while inspecting kernel events with `sudo evtest` or `sudo hid-recorder`.
