# Examples

Both programs find a board with `mimhid_discover` (or connect directly via port path), open it, and build and run on Linux, macOS, and Windows (`-DMIMHID_BUILD_EXAMPLES=ON`).

| File | What it shows |
|---|---|
| [`examples/c/basic_keyboard_mouse.c`](../examples/c/basic_keyboard_mouse.c) | `mimhid_device_open` with port path and USB identity, a keyboard and a mouse Spec, explicit interface assignments, both nodes opened in one batch, commit, a key press, mouse motion, and a click. |
| [`examples/cpp/multitouch_gesture.cpp`](../examples/cpp/multitouch_gesture.cpp) | A two-finger touchscreen Spec and a pinch-out gesture through the `mimhid::` forwarders. |

## Patterns

**Open several nodes and commit in one re-enumeration**

```c
mimhid_node_options kbd_node = {0};
kbd_node.struct_size = sizeof(kbd_node);
kbd_node.interface_number = 0;
kbd_node.poll_interval_ms = 1;
kbd_node.ep_in_size = 64;
kbd_node.bcd_hid = 0x0111;

mimhid_node_options mouse_node = {0};
mouse_node.struct_size = sizeof(mouse_node);
mouse_node.interface_number = 1;
mouse_node.poll_interval_ms = 1;
mouse_node.ep_in_size = 64;
mouse_node.bcd_hid = 0x0111;

mimhid_device_batch_begin(device);
mimhid_node_open(device, keyboard_spec, &kbd_node, &keyboard);
mimhid_node_open(device, mouse_spec, &mouse_node, &mouse);
mimhid_device_batch_end(device);        /* one re-enumeration for both nodes */
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
mimhid_device_disconnect(device);

/* Reconnect physically to host (D+=3.3V) */
mimhid_device_connect(device);
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

**Keep the board on the bus after the program exits**

```c
options.keep_attached = 1;
```
