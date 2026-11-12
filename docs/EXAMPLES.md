# Examples

The programs find a board with `mimusb_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMUSB_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimusb_discover`, `mimusb_device_open` with every value set in the program (the initializers fill none in) and a USB identity, a keyboard and a mouse Spec on interfaces 0 and 1 opened in one batch, a key press, mouse motion, and a click. |
| [`examples/c/control_transfers.c`](../examples/c/control_transfers.c) | Control transfers of the target: report every request, hold one by an `ASK` rule, and answer it from the event sink. Runs for 10 s and types nothing. |
| [`examples/c/hid_interface.c`](../examples/c/hid_interface.c) | A boot keyboard whose HID interface the program writes itself with `mimusb_node_open` (class 0x03): the HID descriptor (`mimusb_hid_descriptor_bytes`), the report descriptor and HID descriptor answers (`mimusb_hid_descriptor_rule`), `GET_REPORT` from the last report sent (`MIMUSB_CONTROL_ANSWER_LAST_SENT`), `SET_REPORT`, `SET_IDLE` and `SET_PROTOCOL` taken (`MIMUSB_CONTROL_ACCEPT`), and reports sent with `mimusb_node_endpoint_write`. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimusb::` forwarders. |

`libaoahid` has one large program, `multi_profile.c`, that CI runs against a fake transport that finds no device. `libmimusb` has no such transport for its examples: `mimusb_discover` messages every serial port of the host, so a CI run would touch real ports. The same ground is covered by `basic_keyboard_mouse.c` (open, Specs, batch, submit, close), `control_transfers.c` (the event sink and control routing), and `examples/c/verify/verify_all.c` (three profiles interleaved against one real target); the build of each is a CTest case only for the per-profile programs, which need no board.

## One example per profile kind

`examples/c/profiles/` holds one small, self-contained program for each of the nine `mimusb_profile_kind` values, plus `keyboard_battery.c` for the battery report any generated profile can carry. Each file zeroes its profile's options with the `*_init` function and then sets every field the profile needs (the initializer fills in no value), so it can be copied into a product and edited directly. None of them opens a board, so they run anywhere the library links. With both `MIMUSB_BUILD_EXAMPLES` and `MIMUSB_BUILD_TESTS` on, CTest runs each one as `mimusb.example-profile-<name>`.

| # | Profile kind | Example file | Factory |
|---:|---|---|---|
| 1 | `MIMUSB_PROFILE_KEYBOARD` | [`examples/c/profiles/keyboard.c`](../examples/c/profiles/keyboard.c) | `mimusb_spec_create_keyboard` |
| 2 | `MIMUSB_PROFILE_MOUSE` | [`examples/c/profiles/mouse.c`](../examples/c/profiles/mouse.c) | `mimusb_spec_create_mouse` |
| 3 | `MIMUSB_PROFILE_TOUCHSCREEN` | [`examples/c/profiles/touchscreen.c`](../examples/c/profiles/touchscreen.c) | `mimusb_spec_create_touchscreen` |
| 4 | `MIMUSB_PROFILE_TOUCHPAD` | [`examples/c/profiles/touchpad.c`](../examples/c/profiles/touchpad.c) | `mimusb_spec_create_touchpad` |
| 5 | `MIMUSB_PROFILE_PEN` | [`examples/c/profiles/pen.c`](../examples/c/profiles/pen.c) | `mimusb_spec_create_pen` |
| 6 | `MIMUSB_PROFILE_DIGITIZER` | [`examples/c/profiles/digitizer.c`](../examples/c/profiles/digitizer.c) | `mimusb_spec_create_digitizer` |
| 7 | `MIMUSB_PROFILE_GAMEPAD` | [`examples/c/profiles/gamepad.c`](../examples/c/profiles/gamepad.c) | `mimusb_spec_create_gamepad` |
| 8 | `MIMUSB_PROFILE_TOGGLE` | [`examples/c/profiles/toggle.c`](../examples/c/profiles/toggle.c) | `mimusb_spec_create_toggle` (Consumer, System, Camera, or Telephony keys, selected by `application_page`, `application_usage`, and `field_page`) |
| 9 | `MIMUSB_PROFILE_RAW` | [`examples/c/profiles/raw.c`](../examples/c/profiles/raw.c) | `mimusb_spec_create_raw` |
| - | `MIMUSB_PROFILE_KEYBOARD` with `enable_battery` | [`examples/c/profiles/keyboard_battery.c`](../examples/c/profiles/keyboard_battery.c) | `mimusb_spec_create_keyboard` (the same two option fields exist on every generated profile) |

`profile_example.h` is shared by all ten. It holds only the reporting helper and no product values. The helper prints the descriptor size and report layout the library derived from the caller's options, then the call sequence that would send the profile through a real board: `mimusb_discover`, `mimusb_device_open`, `mimusb_node_open`, `mimusb_device_wait_ready`, the profile's own mutation calls, and `mimusb_device_close`. Each example supplies its own mutation steps. The sequence is printed, not executed, because these programs open no board.

## Programs to watch on a real target

`examples/c/verify/` holds programs for a person to run with a board attached and to watch on the target machine. Each prints what to prepare, waits a few seconds, then does one clearly described thing. They are built with the other examples as `mimusb_verify_<name>` and are never registered as CTest cases. [QUICKSTART.md](QUICKSTART.md) has the walkthrough.

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

[`examples/python/enumerate.py`](../examples/python/enumerate.py), [`examples/csharp/Program.cs`](../examples/csharp/Program.cs), and [`bindings/rust/mimusb-sys/examples/enumerate.rs`](../bindings/rust/mimusb-sys/examples/enumerate.rs) list the boards `mimusb_discover` finds. They are deliberately short: the life cycle with Nodes, a batch, and the event sink is shown once, in C. `mimusb_discover` messages every serial port of the host, so run them only when that is acceptable. See [BUILDING.md](BUILDING.md#language-bindings) for the language bindings.

## Patterns

### The waits and interface helper

Every call that can wait takes its time as an explicit parameter, and the library keeps none and defines no constant for one (see [API.md](API.md#rules-every-function-follows)). The snippets below use these values, chosen by the examples for a board on a USB serial adapter; the same goes for the node settings, which the initializer leaves zero:

```c
#include <mimusb.h>

enum { CONTROL_MS = 500, SEND_MS = 500, DRAIN_MS = 1000, RECONNECT_MS = 3000 };

static void fill_timeouts(mimusb_commit_timeouts* commit_timeouts) {
    mimusb_commit_timeouts_init(commit_timeouts); /* zero: refused until you set them */
    commit_timeouts->control_timeout_ms = CONTROL_MS;
    commit_timeouts->reconnect_timeout_ms = RECONNECT_MS;
}

/* A HID interface with bInterfaceNumber n, polled every 1 ms through IN endpoint 0x81 + n with 64-byte packets,
 * and no OUT endpoint: every field is set here, none by mimusb_hid_node_config_init. */
static void fill_node(mimusb_hid_node_config* config, uint8_t n) {
    mimusb_hid_node_config_init(config);
    config->interface_number = n;
    config->bcd_hid = 0x0111;
    config->ep_in.address = (uint8_t)(0x81 + n);
    config->ep_in.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    config->ep_in.interval = 1;
    config->ep_in.max_packet_size = 64;
    config->ep_in.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    config->ep_in.policy.depth = 7;
}
```

### Open several nodes with one re-enumeration

```c
#include <mimusb.h>

static void open_two_nodes_in_batch(mimusb_device* device,
                                    mimusb_spec* keyboard_spec,
                                    mimusb_spec* mouse_spec,
                                    const mimusb_commit_timeouts* commit_timeouts,
                                    mimusb_node** keyboard,
                                    mimusb_node** mouse) {
    mimusb_hid_node_config kbd_node;
    mimusb_hid_node_config_init(&kbd_node);
    kbd_node.interface_number = 0;
    kbd_node.bcd_hid = 0x0111;
    kbd_node.ep_in.address = 0x81;
    kbd_node.ep_in.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    kbd_node.ep_in.interval = 1;
    kbd_node.ep_in.max_packet_size = 64;
    kbd_node.ep_in.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    kbd_node.ep_in.policy.depth = 7;

    mimusb_hid_node_config mouse_node = kbd_node;
    mouse_node.interface_number = 1;
    mouse_node.ep_in.address = 0x82;

    mimusb_device_batch_begin(device);
    mimusb_node_open_profile(device, keyboard_spec, &kbd_node, commit_timeouts, keyboard);
    mimusb_node_open_profile(device, mouse_spec, &mouse_node, commit_timeouts, mouse);
    mimusb_device_batch_end(device, commit_timeouts); /* one re-enumeration for both nodes */
    mimusb_device_wait_ready(device, 5000);
}
```

### Share one interface between two nodes

Both need Report IDs, all different, and the same `mimusb_hid_node_config`:

```c
#include <mimusb.h>

static void enable_distinct_report_ids(mimusb_keyboard_options* keyboard_options,
                                       mimusb_mouse_options* mouse_options) {
    keyboard_options->report_id.enabled = 1;
    keyboard_options->report_id.value = 1;
    mouse_options->report_id.enabled = 1;
    mouse_options->report_id.value = 2;
    /* Create the Specs, then open both with the same mimusb_hid_node_config. */
}
```

### Set properties of the USB interface a node uses

```c
#include <mimusb.h>

static void configure_interface_with_out_endpoint(mimusb_hid_node_config* node_config) {
    mimusb_hid_node_config_init(node_config);
    node_config->interface_number = 0;
    node_config->bcd_hid = 0x0111;
    node_config->name = "Keys";
    node_config->ep_in.address = 0x81;
    node_config->ep_in.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    node_config->ep_in.interval = 4;
    node_config->ep_in.max_packet_size = 64;
    node_config->ep_in.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    node_config->ep_in.policy.depth = 7;
    node_config->ep_out.address = 0x01;
    node_config->ep_out.transfer_type = MIMUSB_TRANSFER_INTERRUPT;       /* an OUT endpoint: the target's Output reports reach event_sink */
    node_config->ep_out.max_packet_size = 64;
    node_config->ep_out.interval = 1;
}
```

### Send reports of a raw Spec

`mimusb_spec_create_raw` builds a Spec from a caller's report descriptor, with a report table the library checks reports against; it is opened with `mimusb_node_open_profile` like any other Spec.

```c
#include <mimusb.h>

static mimusb_result send_raw_report(mimusb_device* device,
                                     mimusb_spec* raw_spec,
                                     const mimusb_hid_node_config* raw_config,
                                     const mimusb_commit_timeouts* commit_timeouts,
                                     const uint8_t* report,
                                     size_t report_len) {
    mimusb_node* raw_node = NULL;
    mimusb_result r = mimusb_node_open_profile(device, raw_spec, raw_config, commit_timeouts,
                                               &raw_node);
    if (r != MIMUSB_OK) return r;
    return mimusb_raw_submit(raw_node, report, report_len, 500); /* Report ID included, checked */
}
```

For a HID interface with no Spec at all (a descriptor copied from a real device, nothing checked), see `examples/c/hid_interface.c`: the interface is a `mimusb_node_open` Node of class 0x03 with its own control rules, and its reports go out with `mimusb_node_endpoint_write`.

### Present a vendor-class bulk device

`mimusb_node_open` takes interfaces of any class, HID included. The library places the class bytes it is given and reads none of them. `examples/c/vendor_bulk.c` is the whole program: one interface of class `0xFF`, a bulk IN endpoint that makes `mimusb_node_endpoint_write` wait while four transfers are unfinished, and a bulk OUT endpoint whose packets arrive as `MIMUSB_EVENT_ENDPOINT_DATA` events. It builds with the other examples and needs a board and a target, so it is not a ctest case.

```c
mimusb_usb_endpoint in = {0};
in.address = 0x81; in.transfer_type = MIMUSB_TRANSFER_BULK; in.max_packet_size = 64;
in.policy.on_full = MIMUSB_FULL_WAIT; in.policy.depth = 4;
mimusb_usb_endpoint out = {0};
out.address = 0x01; out.transfer_type = MIMUSB_TRANSFER_BULK; out.max_packet_size = 64;
mimusb_usb_item items[2] = {mimusb_usb_item_make_endpoint(&in),
                            mimusb_usb_item_make_endpoint(&out)};
mimusb_usb_setting setting = {items, 2};
/* interface.class_code = 0xFF; interface.settings = &setting; interface.setting_count = 1; */
/* mimusb_node_open(device, &node_config, &timeouts, &node);
   mimusb_node_endpoint_write(node, 0x81, data, length, 1000); */
```

### Present a device given as bytes

To present a copy of a real device, give the bytes the target reads instead of composing them from Nodes. `examples/c/device_from_descriptors.c` is the whole program: an 18-byte device descriptor, a 32-byte configuration with one vendor interface (bulk IN `0x81`, bulk OUT `0x01`) and two strings (`mimusb_device_set_string`). The policy of every IN endpoint is given first (`mimusb_device_endpoint_set_policy`); the library checks the bytes (never a class) and names the field and the byte offset of whatever does not hold together. A Device given as bytes has no Node: writes, Halt and flushes name the endpoint by address (`mimusb_device_endpoint_write`, `_stall`, `_clear_stall`, `_flush`), and the packets of an OUT endpoint arrive as `MIMUSB_EVENT_ENDPOINT_DATA` with `node_id` 0. While descriptors are set, `mimusb_node_open` returns `MIMUSB_ERR_BUSY`, and so does `mimusb_device_set_descriptors` while a Node is open; `mimusb_device_clear_descriptors` ends it. `usb_device`, `usb_config` and their four strings are not used then. It needs a board and a target, so it is not a ctest case.

### Present a WinUSB device (BOS descriptor)

`examples/c/winusb_bulk.c` adds a BOS descriptor (USB 3.2 9.6.2) carrying the Microsoft OS 2.0 platform capability, and a control rule of the Device that answers the vendor request the capability names with the descriptor set, so that Windows binds WinUSB without an INF file. The layouts follow Microsoft's "Microsoft OS 2.0 Descriptors Specification". A host asks for a BOS descriptor only when `bcdUSB` is 0x0201 or more (the commit refuses a BOS below that). **This example has not been run on Windows**: the BOS descriptor and the answer were read back from the simulated board, and no Windows machine has bound WinUSB to the device it presents. A Device composed from Nodes takes its BOS with `mimusb_device_set_bos`.

### Command physical connection and disconnection

```c
#include <mimusb.h>

static void toggle_bus_attachment(mimusb_device* device,
                                  const mimusb_commit_timeouts* commit_timeouts) {
    /* Off the bus: D+ at 0 V */
    mimusb_device_disconnect(device, 500);

    /* Back on the bus: D+ pulled up to 3.3 V, then the target enumerates the device */
    mimusb_device_connect(device, commit_timeouts);
}
```

### React to keyboard LEDs (the target writes an Output report)

```c
#include <mimusb.h>

static void on_led_event(void* user, const mimusb_event* event) {
    uint8_t* leds = (uint8_t*)user;
    if (event->kind == MIMUSB_EVENT_OUTPUT_REPORT && event->length > 0) {
        *leds = event->data[0]; /* copy; event->data is valid only during the call */
    }
}
```

### Report the target's control transfers, and answer one

```c
#include <mimusb.h>

static mimusb_result setup_vendor_control_rule(mimusb_device* device) {
    mimusb_control_rule rule = {0};
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0;               /* vendor, device to host */
    rule.request_mask = 0xFF;
    rule.request_value = 0x42;
    rule.action = MIMUSB_CONTROL_ASK;             /* hold it and ask the program */
    rule.timeout_ms = 100;                        /* required: no default */
    rule.when_late = MIMUSB_CONTROL_ACTION_STALL; /* required: STALL or DROP if no answer comes */
    return mimusb_device_set_control_rules(device, 1 /* report every request */, &rule, 1, 500);
}

static void answer_held_control_setup(mimusb_device* device,
                                      const mimusb_event* event,
                                      const uint8_t* data,
                                      size_t length) {
    if (event->kind == MIMUSB_EVENT_CONTROL_SETUP &&
        (event->control_flags & MIMUSB_CONTROL_FLAG_HELD) != 0) {
        mimusb_device_control_reply(
            device, event->control_token, MIMUSB_CONTROL_ACTION_ANSWER, data, length, 500);
    }
}
```

See [PROTOCOL.md](PROTOCOL.md#control-rules) for the other actions (`ANSWER`, `ANSWER_LAST_SENT`, `ACCEPT`, `STALL`), what is held, the verdict of a host-to-device transfer, and the flags.

### Keep the board on the bus after the program exits

```c
#include <mimusb.h>

static void enable_keep_attached(mimusb_device_config* config) {
    config->transport.keep_attached = 1;
}
```
