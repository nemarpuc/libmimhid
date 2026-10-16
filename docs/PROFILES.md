# Profiles

The profiles, their options structures, report layouts, and report descriptors are those of `libaoahid`; this project reuses its HID layer, and the descriptors are tested byte for byte against `libaoahid`'s golden files (`tests/golden`) for the same options. The fields of each `mimhid_*_options` structure are documented in [`mimhid.h`](../include/mimhid.h). `mimhid_spec_descriptor` returns the report descriptor of a Spec, and `mimhid_spec_manifest` the reports it sends (Report ID, wire length, kind).

| Profile | Spec call | Input calls | Notes |
|---|---|---|---|
| Keyboard | `mimhid_spec_create_keyboard` | `keyboard_key` | usage range and modifiers set by the options; N-key rollover bitmap |
| Mouse | `mimhid_spec_create_mouse` | `mouse_move`, `mouse_scroll`, `mouse_button` | buttons, X/Y, optional wheel and pan; battery optional |
| Toggle | `mimhid_spec_create_toggle` | `toggle_usage` | consumer, system, telephony, or camera usages from an allow list |
| Gamepad | `mimhid_spec_create_gamepad` | `gamepad_button`, `gamepad_axis`, `gamepad_dpad` | axes with roles, buttons, D-pad as HAT or buttons |
| Touchscreen | `mimhid_spec_create_touchscreen` | `touchscreen_contact` | fixed contact slots, optional pressure, width, height, azimuth, scan time, multi-packet frames |
| Touchpad | `mimhid_spec_create_touchpad` | `touchpad_contact`, `touchpad_button` | as touchscreen, plus click buttons |
| Pen | `mimhid_spec_create_pen` | `pen_update` | tip, barrel buttons, eraser, hover, tilt, pressure |
| Digitizer | `mimhid_spec_create_digitizer` | `digitizer_update` | pen with the extended fields |
| Raw | `mimhid_spec_create_raw` | `raw_submit`, `report_send` | your own report descriptor and the reports you will send; or use `mimhid_node_open_raw` to stage a raw descriptor directly |

## Target assumptions

- `Touchscreen` and `Touchpad` are shaped for the Linux `hid-multitouch` driver,
  including the optional contact-count-maximum feature declaration. Their
  descriptors build and parse on any target, but only Linux 7.2 has been
  checked against the driver. Windows, macOS, and Android USB hosts were not
  tested with these profiles. Their logical maxima, slot counts, and feature
  responses must match the target driver's parser limits.

Battery Strength is a second Input report that profiles with `enable_battery` carry; `battery_update` sets it, and it is sent when no input report is pending.

## Rules

1. The library does not shape input. Coordinates, pressure, tilt, and timing are sent as given.
2. There are no timers: no key repeat, no debounce, no automatic release (closing a node sends the neutral report).
3. Mutating calls change state; `mimhid_node_submit` sends. A touch frame with more contacts than fit in one report takes several reports.
4. The target's HID parser limits are policy: `mimhid_device_options` requires the `linux_*_policy` values to be nonzero, and `node_open` rejects a descriptor that exceeds them.
5. Several profiles can run at once. Each is one node with an explicit `interface_number`; see [LIMITS.md](LIMITS.md) for how many a board holds.

## Target behavior

Linux is verified on hardware (see [TARGET_MATRIX.md](TARGET_MATRIX.md)). With the firmware, what the target sees is exactly the Spec's descriptor, so any behavior of a driver follows from that descriptor, not from this library. Windows and macOS targets have not been tried.
