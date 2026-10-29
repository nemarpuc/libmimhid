# Examples

The programs find a board with `mimhid_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMHID_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimhid_discover`, `mimhid_device_open` with the initial values and a USB identity, a keyboard and a mouse Spec on interfaces 0 and 1 opened in one batch, a key press, mouse motion and a click. |
| [`examples/c/control_transfers.c`](../examples/c/control_transfers.c) | Control transfers of the target: report every request, hold one by a rule, and answer it from the event sink. Runs for 10 s and types nothing. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimhid::` forwarders. |

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
