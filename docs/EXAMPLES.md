# Examples

The programs find a board with `mimhid_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMHID_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimhid_discover`, `mimhid_device_open` with the initial values and a USB identity, a keyboard and a mouse Spec on interfaces 0 and 1 opened in one batch, a key press, mouse motion and a click. |
| [`examples/c/control_transfers.c`](../examples/c/control_transfers.c) | Control transfers of the target: report every request, hold one by a rule, and answer it from the event sink. Runs for 10 s and types nothing. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimhid::` forwarders. |

libaoahid has one large program, `multi_profile.c`, that CI runs against a fake transport that finds no device. libmimhid has no such transport for its examples: `mimhid_discover` messages every serial port of the host, so a CI run would touch real ports. The same ground is covered by `basic_keyboard_mouse.c` (open, Specs, batch, submit, close), `control_transfers.c` (the event sink and control routing) and `examples/c/verify/verify_all.c` (three profiles interleaved against one real target); the build of each is a CTest case only for the per-profile programs, which need no board.

## One example per profile kind

`examples/c/profiles/` holds one small, self-contained program for each of the nine `mimhid_profile_kind` values, plus `keyboard_battery.c` for the battery report any generated profile can carry. Each file starts from its profile's `*_init` function and sets only what it changes, so it can be copied into a product and edited directly. None of them opens a board, so they run anywhere the library links. With both `MIMHID_BUILD_EXAMPLES` and `MIMHID_BUILD_TESTS` on, CTest runs each one as `mimhid.example-profile-<name>`.

| # | Profile kind | Example file | Factory |
|---:|---|---|---|
| 1 | `MIMHID_PROFILE_KEYBOARD` | `keyboard.c` | `mimhid_spec_create_keyboard` |
| 2 | `MIMHID_PROFILE_MOUSE` | `mouse.c` | `mimhid_spec_create_mouse` |
| 3 | `MIMHID_PROFILE_TOUCHSCREEN` | `touchscreen.c` | `mimhid_spec_create_touchscreen` |
| 4 | `MIMHID_PROFILE_TOUCHPAD` | `touchpad.c` | `mimhid_spec_create_touchpad` |
| 5 | `MIMHID_PROFILE_PEN` | `pen.c` | `mimhid_spec_create_pen` |
| 6 | `MIMHID_PROFILE_DIGITIZER` | `digitizer.c` | `mimhid_spec_create_digitizer` |
| 7 | `MIMHID_PROFILE_GAMEPAD` | `gamepad.c` | `mimhid_spec_create_gamepad` |
| 8 | `MIMHID_PROFILE_TOGGLE` | `toggle.c` | `mimhid_spec_create_toggle` (Consumer, System, Camera or Telephony keys, selected by `application_page`, `application_usage` and `field_page`) |
| 9 | `MIMHID_PROFILE_RAW` | `raw.c` | `mimhid_spec_create_raw` |
| - | `MIMHID_PROFILE_KEYBOARD` with `enable_battery` | `keyboard_battery.c` | `mimhid_spec_create_keyboard` (the same two option fields exist on every generated profile) |

`profile_example.h` is shared by all ten. It holds only the reporting helper and no product values. The helper prints the descriptor size and report layout the library derived from the caller's options, then the call sequence that would send the profile through a real board: `mimhid_discover`, `mimhid_device_open`, `mimhid_node_open`, `mimhid_device_wait_ready`, the profile's own mutation calls, and `mimhid_device_close`. Each example supplies its own mutation steps. The sequence is printed, not executed, because these programs open no board.

## Programs to watch on a real target

`examples/c/verify/` holds programs for a person to run with a board attached and to watch on the target machine. Each prints what to prepare, waits a few seconds, then does one clearly described thing. They are built with the other examples as `mimhid_verify_<name>` and are never registered as CTest cases. [QUICKSTART.md](QUICKSTART.md) has the walkthrough.

| Program | What it does |
|---|---|
| `verify_keyboard.c` | Types a short message three times. |
| `verify_mouse.c` | Moves the cursor in circles. |
| `verify_touch.c` | Drags one finger across the screen five times. |
| `verify_toggle.c` | Taps Consumer Control Usages (Play/Pause, Volume, Mute, AC New); System Control is left out on purpose, since Power Down and Sleep cannot be undone from here. |
| `verify_all.c` | Touch, keyboard and mouse, one at a time for two rounds, then interleaved from one thread. Opens its three Nodes in one batch, so the board enumerates once. |

`verify_common.h` finds the first board and opens a Node. Its USB identity is the pid.codes test pair, for testing in-house only.

The commands to compile all of them against a release archive are in `examples/README.md`.

## Other languages

`examples/python/enumerate.py`, `examples/csharp/Program.cs` and `bindings/rust/mimhid-sys/examples/enumerate.rs` list the boards `mimhid_discover` finds. They are deliberately short: the life cycle with Nodes, a batch and the event sink is shown once, in C. `mimhid_discover` messages every serial port of the host, so run them only when that is acceptable. See the Language bindings section of `README.md` for the bindings.

## Patterns

**The waits**

Every call that can wait takes its time; the library keeps none (see [API.md](API.md#timeouts)). The snippets below use the constants of `mimhid.h`, values that work for a board on a USB serial adapter:

```c
mimhid_commit_timeouts commit_timeouts;
mimhid_commit_timeouts_init(&commit_timeouts); /* MIMHID_CONTROL_TIMEOUT_MS, MIMHID_RECONNECT_TIMEOUT_MS */
```

**Open several nodes with one re-enumeration**

```c
mimhid_node_config kbd_node;
mimhid_node_config_init(&kbd_node, 0);   /* interface 0, IN endpoint 0x81 */
mimhid_node_config mouse_node;
mimhid_node_config_init(&mouse_node, 1); /* interface 1, IN endpoint 0x82 */

mimhid_device_batch_begin(device);
mimhid_node_open(device, keyboard_spec, &kbd_node, &commit_timeouts, &keyboard);
mimhid_node_open(device, mouse_spec, &mouse_node, &commit_timeouts, &mouse);
mimhid_device_batch_end(device, &commit_timeouts);   /* one re-enumeration for both nodes */
mimhid_device_wait_ready(device, 5000);
```

**Share one interface between two nodes**

Both need Report IDs, all different, and the same `mimhid_node_config`:

```c
keyboard_options.report_id.enabled = 1;
keyboard_options.report_id.value = 1;
mouse_options.report_id.enabled = 1;
mouse_options.report_id.value = 2;
/* ...create the Specs, then open both with the same kbd_node */
```

**Set properties of the USB interface a node uses**

```c
mimhid_node_config node_config;
mimhid_node_config_init(&node_config, 0);
node_config.iface.name = "Keys";
node_config.ep_in.poll_interval_ms = 4;
node_config.ep_out.address = 0x01;       /* an OUT endpoint: the target's Output reports reach event_sink */
node_config.ep_out.max_packet_size = 64;
node_config.ep_out.poll_interval_ms = 1;
```

**Open a raw HID report descriptor directly**

`mimhid_spec_create_raw` builds a Spec from one instead, with a report table the library checks reports against.

```c
mimhid_node_config raw_config;
mimhid_node_config_init(&raw_config, 2);
mimhid_node* raw_node = NULL;
mimhid_node_open_raw(device, raw_desc, sizeof(raw_desc), &raw_config, &commit_timeouts, &raw_node);
mimhid_report_send(raw_node, report, sizeof(report), MIMHID_SEND_TIMEOUT_MS); /* Report ID included */
```

**Command physical connection and disconnection**

```c
/* Off the bus: D+ at 0 V */
mimhid_device_disconnect(device, MIMHID_CONTROL_TIMEOUT_MS);

/* Back on the bus: D+ pulled up to 3.3 V, then the target enumerates the device */
mimhid_device_connect(device, &commit_timeouts);
```

**React to keyboard LEDs** (the target writes an Output report)

```c
static void on_event(void* user, const mimhid_event* event) {
    if (event->kind == MIMHID_EVENT_OUTPUT_REPORT && event->length > 0)
        leds = event->data[0];            /* copy; event->data is valid only during the call */
}
...
config.transport.event_sink = on_event;
```

**Report the target's control transfers, and answer one**

```c
mimhid_control_rule rule = {0};
rule.request_type_mask = 0xFF; rule.request_type_value = 0xC0; /* vendor, device to host */
rule.request_mask = 0xFF;      rule.request_value = 0x42;
rule.timeout_ms = 100;                        /* required: no default */
rule.when_late = MIMHID_CONTROL_ACTION_STALL; /* required: what happens if no answer comes */
mimhid_device_set_control_routing(device, 1 /* report every request */, &rule, 1,
                                  MIMHID_CONTROL_TIMEOUT_MS);

/* in event_sink, for MIMHID_EVENT_CONTROL_SETUP with MIMHID_CONTROL_FLAG_HELD: */
mimhid_device_control_reply(device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER, data, length,
                            MIMHID_SEND_TIMEOUT_MS);
```

See [PROTOCOL.md](PROTOCOL.md#control-transfers) for what is held, the verdict of a host-to-device transfer, and the flags.

**Keep the board on the bus after the program exits**

```c
config.transport.keep_attached = 1;
```
