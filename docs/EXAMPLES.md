# Examples

The programs find a board with `mimhid_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMHID_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimhid_device_open` with port path and USB identity, a keyboard and a mouse Spec, explicit interface assignments, both nodes opened in one batch, commit, a key press, mouse motion, and a click. |
| [`examples/c/control_transfers.c`](../examples/c/control_transfers.c) | Control transfers of the target: report every request, hold one by a rule, and answer it from the event sink. Runs for 10 s and types nothing. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimhid::` forwarders. |

## Patterns

**The waits**

Every call that can wait takes its time; the library has none of its own (see [API.md](API.md#timeouts)). The snippets below use these, which are the program's choice:

```c
#define CONTROL_TIMEOUT_MS 500
#define SEND_TIMEOUT_MS 500
#define RECONNECT_TIMEOUT_MS 3000
#define DRAIN_TIMEOUT_MS 1000

mimhid_commit_timeouts commit_timeouts;
mimhid_commit_timeouts_init(&commit_timeouts);
commit_timeouts.control_timeout_ms = CONTROL_TIMEOUT_MS;
commit_timeouts.reconnect_timeout_ms = RECONNECT_TIMEOUT_MS;
```

**Open several nodes and commit in one re-enumeration**

```c
mimhid_node_config kbd_node;
mimhid_node_config_init(&kbd_node);
kbd_node.iface.number = 0;
kbd_node.iface.class_code = 3;
kbd_node.iface.bcd_hid = 0x0111;
kbd_node.ep_in.address = 0x81;
kbd_node.ep_in.poll_interval_ms = 1;
kbd_node.ep_in.max_packet_size = 64;

mimhid_node_config mouse_node = kbd_node;
mouse_node.iface.number = 1;
mouse_node.ep_in.address = 0x82;

mimhid_device_batch_begin(device);
mimhid_node_open(device, keyboard_spec, &kbd_node, &commit_timeouts, &keyboard);
mimhid_node_open(device, mouse_spec, &mouse_node, &commit_timeouts, &mouse);
mimhid_device_batch_end(device, &commit_timeouts);   /* one re-enumeration for both nodes */
mimhid_device_wait_ready(device, 5000);
```

**Set properties of the USB interface a node uses**

```c
mimhid_node_options node_options = {0};
node_options.struct_size = sizeof(node_options);
node_options.interface_number = 0; /* explicit interface index */
node_options.poll_interval_ms = 4;
node_options.ep_in_size = 64;
node_options.ep_out_size = 0;      /* set 1 to 64 if OUT endpoint needed */
node_options.interface_name = "Keys";
node_options.bcd_hid = 0x0111;
```

**Open a raw HID report descriptor directly**

```c
mimhid_node* raw_node = NULL;
mimhid_node_open_raw(device, raw_desc, sizeof(raw_desc), &node_options, &raw_node);
```

**Command physical connection and disconnection**

```c
/* Disconnect physically from host (D+=0V stealth mode) */
mimhid_device_disconnect(device, CONTROL_TIMEOUT_MS);

/* Reconnect physically to host (D+=3.3V) */
mimhid_device_connect(device, &commit_timeouts);
```

**React to keyboard LEDs** (the target writes an Output report)

```c
static void on_event(void* user, const mimhid_event* event) {
    if (event->kind == MIMHID_EVENT_OUTPUT_REPORT && event->length > 0)
        leds = event->data[0];            /* copy; event->data is valid only during the call */
}
...
options.event_sink = on_event;
```

**Report the target's control transfers, and answer one**

```c
mimhid_control_rule rule = {0};
rule.request_type_mask = 0xFF; rule.request_type_value = 0xC0; /* vendor, device to host */
rule.request_mask = 0xFF;      rule.request_value = 0x42;
rule.timeout_ms = 100;                        /* required: no default */
rule.when_late = MIMHID_CONTROL_ACTION_STALL; /* required: what happens if no answer comes */
mimhid_device_set_control_routing(device, 1 /* report every request */, &rule, 1, CONTROL_TIMEOUT_MS);

/* in event_sink, for MIMHID_EVENT_CONTROL_SETUP with MIMHID_CONTROL_FLAG_HELD: */
mimhid_device_control_reply(device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER, data, length,
                            SEND_TIMEOUT_MS);
```

See [PROTOCOL.md](PROTOCOL.md#control-transfers) for what is held, the verdict of a host-to-device transfer, and the flags.

**Keep the board on the bus after the program exits**

```c
options.keep_attached = 1;
```
