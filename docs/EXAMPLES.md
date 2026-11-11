# Examples

The programs find a board with `mimhid_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMHID_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimhid_discover`, `mimhid_device_open` with every value set in the program (the initializers fill none in) and a USB identity, a keyboard and a mouse Spec on interfaces 0 and 1 opened in one batch, a key press, mouse motion, and a click. |
| [`examples/c/control_transfers.c`](../examples/c/control_transfers.c) | Control transfers of the target: report every request, hold one by a rule, and answer it from the event sink. Runs for 10 s and types nothing. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimhid::` forwarders. |

`libaoahid` has one large program, `multi_profile.c`, that CI runs against a fake transport that finds no device. `libmimhid` has no such transport for its examples: `mimhid_discover` messages every serial port of the host, so a CI run would touch real ports. The same ground is covered by `basic_keyboard_mouse.c` (open, Specs, batch, submit, close), `control_transfers.c` (the event sink and control routing), and `examples/c/verify/verify_all.c` (three profiles interleaved against one real target); the build of each is a CTest case only for the per-profile programs, which need no board.

## One example per profile kind

`examples/c/profiles/` holds one small, self-contained program for each of the nine `mimhid_profile_kind` values, plus `keyboard_battery.c` for the battery report any generated profile can carry. Each file zeroes its profile's options with the `*_init` function and then sets every field the profile needs (the initializer fills in no value), so it can be copied into a product and edited directly. None of them opens a board, so they run anywhere the library links. With both `MIMHID_BUILD_EXAMPLES` and `MIMHID_BUILD_TESTS` on, CTest runs each one as `mimhid.example-profile-<name>`.

| # | Profile kind | Example file | Factory |
|---:|---|---|---|
| 1 | `MIMHID_PROFILE_KEYBOARD` | [`examples/c/profiles/keyboard.c`](../examples/c/profiles/keyboard.c) | `mimhid_spec_create_keyboard` |
| 2 | `MIMHID_PROFILE_MOUSE` | [`examples/c/profiles/mouse.c`](../examples/c/profiles/mouse.c) | `mimhid_spec_create_mouse` |
| 3 | `MIMHID_PROFILE_TOUCHSCREEN` | [`examples/c/profiles/touchscreen.c`](../examples/c/profiles/touchscreen.c) | `mimhid_spec_create_touchscreen` |
| 4 | `MIMHID_PROFILE_TOUCHPAD` | [`examples/c/profiles/touchpad.c`](../examples/c/profiles/touchpad.c) | `mimhid_spec_create_touchpad` |
| 5 | `MIMHID_PROFILE_PEN` | [`examples/c/profiles/pen.c`](../examples/c/profiles/pen.c) | `mimhid_spec_create_pen` |
| 6 | `MIMHID_PROFILE_DIGITIZER` | [`examples/c/profiles/digitizer.c`](../examples/c/profiles/digitizer.c) | `mimhid_spec_create_digitizer` |
| 7 | `MIMHID_PROFILE_GAMEPAD` | [`examples/c/profiles/gamepad.c`](../examples/c/profiles/gamepad.c) | `mimhid_spec_create_gamepad` |
| 8 | `MIMHID_PROFILE_TOGGLE` | [`examples/c/profiles/toggle.c`](../examples/c/profiles/toggle.c) | `mimhid_spec_create_toggle` (Consumer, System, Camera, or Telephony keys, selected by `application_page`, `application_usage`, and `field_page`) |
| 9 | `MIMHID_PROFILE_RAW` | [`examples/c/profiles/raw.c`](../examples/c/profiles/raw.c) | `mimhid_spec_create_raw` |
| - | `MIMHID_PROFILE_KEYBOARD` with `enable_battery` | [`examples/c/profiles/keyboard_battery.c`](../examples/c/profiles/keyboard_battery.c) | `mimhid_spec_create_keyboard` (the same two option fields exist on every generated profile) |

`profile_example.h` is shared by all ten. It holds only the reporting helper and no product values. The helper prints the descriptor size and report layout the library derived from the caller's options, then the call sequence that would send the profile through a real board: `mimhid_discover`, `mimhid_device_open`, `mimhid_node_open`, `mimhid_device_wait_ready`, the profile's own mutation calls, and `mimhid_device_close`. Each example supplies its own mutation steps. The sequence is printed, not executed, because these programs open no board.

## Programs to watch on a real target

`examples/c/verify/` holds programs for a person to run with a board attached and to watch on the target machine. Each prints what to prepare, waits a few seconds, then does one clearly described thing. They are built with the other examples as `mimhid_verify_<name>` and are never registered as CTest cases. [QUICKSTART.md](QUICKSTART.md) has the walkthrough.

| Program | What it does |
|---|---|
| [`examples/c/verify/verify_keyboard.c`](../examples/c/verify/verify_keyboard.c) | Types a short message three times. |
| [`examples/c/verify/verify_mouse.c`](../examples/c/verify/verify_mouse.c) | Moves the cursor in circles. |
| [`examples/c/verify/verify_touch.c`](../examples/c/verify/verify_touch.c) | Drags one finger across the screen five times. |
| [`examples/c/verify/verify_toggle.c`](../examples/c/verify/verify_toggle.c) | Taps Consumer Control Usages (Play/Pause, Volume, Mute, AC New); System Control is left out on purpose, since Power Down and Sleep cannot be undone from here. |
| [`examples/c/verify/verify_all.c`](../examples/c/verify/verify_all.c) | Touch, keyboard, and mouse, one at a time for two rounds, then interleaved from one thread. Opens its three Nodes in one batch, so the board enumerates once. |

`verify_common.h` finds the first board and opens a Node. It sets every value itself, and its USB identity is the pid.codes test pair, for testing in-house only.

The commands to compile all of them against a release archive are in [`examples/README.md`](../examples/README.md#compiling-these-examples).

## Other languages

[`examples/python/enumerate.py`](../examples/python/enumerate.py), [`examples/csharp/Program.cs`](../examples/csharp/Program.cs), and [`bindings/rust/mimhid-sys/examples/enumerate.rs`](../bindings/rust/mimhid-sys/examples/enumerate.rs) list the boards `mimhid_discover` finds. They are deliberately short: the life cycle with Nodes, a batch, and the event sink is shown once, in C. `mimhid_discover` messages every serial port of the host, so run them only when that is acceptable. See [BUILDING.md](BUILDING.md#language-bindings) for the language bindings.

## Patterns

### The waits and interface helper

Every call that can wait takes its time as an explicit parameter, and the library keeps none and defines no constant for one (see [API.md](API.md#rules-every-function-follows)). The snippets below use these values, chosen by the examples for a board on a USB serial adapter; the same goes for the node settings, which the initializer leaves zero:

```c
#include <mimhid.h>

enum { CONTROL_MS = 500, SEND_MS = 500, DRAIN_MS = 1000, RECONNECT_MS = 3000 };

static void fill_timeouts(mimhid_commit_timeouts* commit_timeouts) {
    mimhid_commit_timeouts_init(commit_timeouts); /* zero: refused until you set them */
    commit_timeouts->control_timeout_ms = CONTROL_MS;
    commit_timeouts->reconnect_timeout_ms = RECONNECT_MS;
}

/* A HID interface with bInterfaceNumber n, polled every 1 ms through IN endpoint 0x81 + n with 64-byte packets,
 * and no OUT endpoint: every field is set here, none by mimhid_hid_node_config_init. */
static void fill_node(mimhid_hid_node_config* config, uint8_t n) {
    mimhid_hid_node_config_init(config);
    config->interface_number = n;
    config->bcd_hid = 0x0111;
    config->ep_in.address = (uint8_t)(0x81 + n);
    config->ep_in.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    config->ep_in.interval = 1;
    config->ep_in.max_packet_size = 64;
    config->ep_in.policy.on_full = MIMHID_FULL_DROP_NEWEST;
    config->ep_in.policy.depth = 7;
}
```

### Open several nodes with one re-enumeration

```c
#include <mimhid.h>

static void open_two_nodes_in_batch(mimhid_device* device,
                                    mimhid_spec* keyboard_spec,
                                    mimhid_spec* mouse_spec,
                                    const mimhid_commit_timeouts* commit_timeouts,
                                    mimhid_node** keyboard,
                                    mimhid_node** mouse) {
    mimhid_hid_node_config kbd_node;
    mimhid_hid_node_config_init(&kbd_node);
    kbd_node.interface_number = 0;
    kbd_node.bcd_hid = 0x0111;
    kbd_node.ep_in.address = 0x81;
    kbd_node.ep_in.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    kbd_node.ep_in.interval = 1;
    kbd_node.ep_in.max_packet_size = 64;
    kbd_node.ep_in.policy.on_full = MIMHID_FULL_DROP_NEWEST;
    kbd_node.ep_in.policy.depth = 7;

    mimhid_hid_node_config mouse_node = kbd_node;
    mouse_node.interface_number = 1;
    mouse_node.ep_in.address = 0x82;

    mimhid_device_batch_begin(device);
    mimhid_node_open_profile(device, keyboard_spec, &kbd_node, commit_timeouts, keyboard);
    mimhid_node_open_profile(device, mouse_spec, &mouse_node, commit_timeouts, mouse);
    mimhid_device_batch_end(device, commit_timeouts); /* one re-enumeration for both nodes */
    mimhid_device_wait_ready(device, 5000);
}
```

### Share one interface between two nodes

Both need Report IDs, all different, and the same `mimhid_hid_node_config`:

```c
#include <mimhid.h>

static void enable_distinct_report_ids(mimhid_keyboard_options* keyboard_options,
                                       mimhid_mouse_options* mouse_options) {
    keyboard_options->report_id.enabled = 1;
    keyboard_options->report_id.value = 1;
    mouse_options->report_id.enabled = 1;
    mouse_options->report_id.value = 2;
    /* Create the Specs, then open both with the same mimhid_hid_node_config. */
}
```

### Set properties of the USB interface a node uses

```c
#include <mimhid.h>

static void configure_interface_with_out_endpoint(mimhid_hid_node_config* node_config) {
    mimhid_hid_node_config_init(node_config);
    node_config->interface_number = 0;
    node_config->bcd_hid = 0x0111;
    node_config->name = "Keys";
    node_config->ep_in.address = 0x81;
    node_config->ep_in.transfer_type = MIMHID_TRANSFER_INTERRUPT;
    node_config->ep_in.interval = 4;
    node_config->ep_in.max_packet_size = 64;
    node_config->ep_in.policy.on_full = MIMHID_FULL_DROP_NEWEST;
    node_config->ep_in.policy.depth = 7;
    node_config->ep_out.address = 0x01;
    node_config->ep_out.transfer_type = MIMHID_TRANSFER_INTERRUPT;       /* an OUT endpoint: the target's Output reports reach event_sink */
    node_config->ep_out.max_packet_size = 64;
    node_config->ep_out.interval = 1;
}
```

### Open a raw HID report descriptor directly

`mimhid_spec_create_raw` builds a Spec from one instead, with a report table the library checks reports against.

```c
#include <mimhid.h>

static mimhid_result send_raw_report(mimhid_device* device,
                                     const uint8_t* raw_desc,
                                     size_t raw_desc_len,
                                     const mimhid_hid_node_config* raw_config,
                                     const mimhid_commit_timeouts* commit_timeouts,
                                     const uint8_t* report,
                                     size_t report_len) {
    mimhid_node* raw_node = NULL;
    mimhid_result r = mimhid_node_open_raw(
        device, raw_desc, raw_desc_len, raw_config, commit_timeouts, &raw_node);
    if (r != MIMHID_OK) return r;
    return mimhid_report_send(raw_node, report, report_len, 500); /* Report ID included */
}
```

### Present a vendor-class bulk device

`mimhid_node_open` takes interfaces of any class but HID. The library places the class bytes it is given and reads none of them. `examples/c/vendor_bulk.c` is the whole program: one interface of class `0xFF`, a bulk IN endpoint that makes `mimhid_node_endpoint_write` wait while four transfers are unfinished, and a bulk OUT endpoint whose packets arrive as `MIMHID_EVENT_ENDPOINT_DATA` events. It builds with the other examples and needs a board and a target, so it is not a ctest case.

```c
mimhid_usb_endpoint endpoints[2] = {
    {0x81, MIMHID_TRANSFER_BULK, 0, 0, 64, 0, {MIMHID_FULL_WAIT, 4}},
    {0x01, MIMHID_TRANSFER_BULK, 0, 0, 64, 0, {0, 0}}};
/* interface_config.class_code = 0xFF; .endpoints = endpoints; .endpoint_count = 2; */
/* mimhid_node_open(device, &node_config, &timeouts, &node);
   mimhid_node_endpoint_write(node, 0x81, data, length, 1000); */
```

### Command physical connection and disconnection

```c
#include <mimhid.h>

static void toggle_bus_attachment(mimhid_device* device,
                                  const mimhid_commit_timeouts* commit_timeouts) {
    /* Off the bus: D+ at 0 V */
    mimhid_device_disconnect(device, 500);

    /* Back on the bus: D+ pulled up to 3.3 V, then the target enumerates the device */
    mimhid_device_connect(device, commit_timeouts);
}
```

### React to keyboard LEDs (the target writes an Output report)

```c
#include <mimhid.h>

static void on_led_event(void* user, const mimhid_event* event) {
    uint8_t* leds = (uint8_t*)user;
    if (event->kind == MIMHID_EVENT_OUTPUT_REPORT && event->length > 0) {
        *leds = event->data[0]; /* copy; event->data is valid only during the call */
    }
}
```

### Report the target's control transfers, and answer one

```c
#include <mimhid.h>

static mimhid_result setup_vendor_control_rule(mimhid_device* device) {
    mimhid_control_rule rule = {0};
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0;               /* vendor, device to host */
    rule.request_mask = 0xFF;
    rule.request_value = 0x42;
    rule.timeout_ms = 100;                        /* required: no default */
    rule.when_late = MIMHID_CONTROL_ACTION_STALL; /* required: what happens if no answer comes */
    return mimhid_device_set_control_routing(device, 1 /* report every request */, &rule, 1, 500);
}

static void answer_held_control_setup(mimhid_device* device,
                                      const mimhid_event* event,
                                      const uint8_t* data,
                                      size_t length) {
    if (event->kind == MIMHID_EVENT_CONTROL_SETUP &&
        (event->control_flags & MIMHID_CONTROL_FLAG_HELD) != 0) {
        mimhid_device_control_reply(
            device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER, data, length, 500);
    }
}
```

See [PROTOCOL.md](PROTOCOL.md#control-transfers) for what is held, the verdict of a host-to-device transfer, and the flags.

### Keep the board on the bus after the program exits

```c
#include <mimhid.h>

static void enable_keep_attached(mimhid_device_config* config) {
    config->transport.keep_attached = 1;
}
```
