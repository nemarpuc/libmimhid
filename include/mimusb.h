// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_H
#define MIMUSB_H

/*
 * libmimusb: a microcontroller board, driven over a serial link, presents a USB device (HID
 * profiles, interfaces of any class, or descriptors given as bytes) to the computer its USB port is
 * plugged into (the "target"). This header is the whole stable C ABI. It
 * exposes no serial handle and no C++ type.
 *
 * Objects
 *   Device    one board, opened on a serial port. It presents one USB device to the target, whose
 *             identity (descriptor, strings) the Device sets.
 *   Spec      an immutable HID report descriptor and report layout, built by a factory
 *             (mimusb_spec_create_*). It needs no board and is reference counted.
 *   Node      the interfaces one function presents to the target. mimusb_node_open opens any
 *             class from its interfaces, endpoints and class descriptors, and the caller reads
 *             and writes its endpoints (mimusb_node_endpoint_write, MIMUSB_EVENT_ENDPOINT_DATA).
 *             The board runs every interface the same way, whatever its class, and what a class
 *             does on endpoint 0 (descriptors, GET_REPORT, SET_IDLE, ...) is a list of control
 *             rules the Node brings (mimusb_control_rule). mimusb_node_open_profile opens a Spec
 *             as one HID collection: the library writes the HID rules and the state its input
 *             calls change (mimusb_keyboard_key, ...); it sends a report when it is submitted.
 *             A HID interface of the caller's own is a mimusb_node_open with class 0x03, its
 *             HID descriptor, mimusb_hid_descriptor_rule and the rules it needs.
 *             A Device can instead be given whole as bytes, with no Node
 *             (mimusb_device_set_descriptors), to present a copy of a real device.
 *   Discovery a snapshot of the boards found on the serial ports.
 *
 * Opening or closing a Node changes the USB device the board presents, so the board drops it from
 * the target and enumerates it again (a "commit"); mimusb_device_batch_begin and _end gather
 * several changes into one.
 *
 * Rules every function follows
 *   Results    Every call records the details of its result for the calling thread, read with
 *              mimusb_last_error(). Results that come from the serial link (the "link errors") are
 *              MIMUSB_ERR_TIMEOUT, _NO_DEVICE, _SHORT_TRANSFER and _IO; error_detail says the frame
 *              and holds the errno or Win32 error, or the board's refusal. Once the port fails or
 *              is unplugged, calls on the Device return MIMUSB_ERR_NO_DEVICE until it is closed.
 *              Any call can also return MIMUSB_ERR_INTERNAL (an allocation failed or an internal
 *              invariant broke). The Returns lines below list what is particular to each function.
 *   Outputs    A function that creates an object returns it through an out_* pointer, which is
 *              set to null first and stays null when the call fails.
 *   Closing    The *_close, *_destroy and *_release functions follow one rule:
 *                MIMUSB_ERR_PARAM or MIMUSB_ERR_UNSET_FIELD  nothing was done; the handle is still
 *                                                           the caller's.
 *                any other result                           the object is gone and its handle must
 *                                                           not be used again. An error says what
 *                                                           failed on the way out.
 *              Writes to the serial port finish within the call, so nothing is ever left pending.
 *   Threads    A Device and its Nodes form one domain: the caller makes sure no two calls in a
 *              domain run at the same time, unless a function says otherwise. Specs and Discovery
 *              snapshots are outside any domain. The event and log sinks run on the library's
 *              reader thread or on the calling thread; they must not call back into the library,
 *              except mimusb_device_control_reply from the event sink.
 *   Timeouts   The library keeps no timeout: every call that can wait takes its timeouts in
 *              milliseconds as arguments (mimusb_commit_timeouts for a commit). A timeout of zero
 *              is refused with MIMUSB_ERR_UNSET_FIELD before anything is sent.
 *   Structures Each options structure starts with struct_size (its sizeof) and reserved (zero).
 *              Its *_init function sets struct_size and zeroes the rest; it fills in no value.
 *              A required field that is zero is refused, and every enumeration starts at 1, so a
 * field left zero never selects something by accident. A flag (enable_*, down, ...) is exactly 0
 * or 1.
 *
 * Specifications cited: USB 2.0, HID 1.11 and HID Usage Tables 1.7 (HUT), all from usb.org.
 * docs/API.md is the guide to this header; docs/PROTOCOL.md describes the serial link.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#if defined(MIMUSB_BUILDING_LIBRARY)
#define MIMUSB_API __declspec(dllexport)
#elif defined(MIMUSB_SHARED)
#define MIMUSB_API __declspec(dllimport)
#else
#define MIMUSB_API
#endif
#define MIMUSB_CALL __cdecl
#else
#define MIMUSB_API __attribute__((visibility("default")))
#define MIMUSB_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MIMUSB_VERSION_MAJOR 0
#define MIMUSB_VERSION_MINOR 4
#define MIMUSB_VERSION_PATCH 0
/* The largest report the library builds, whatever a board holds. */
#define MIMUSB_HOST_MAX_REPORT_BYTES 1024

typedef struct mimusb_discovery mimusb_discovery;
typedef struct mimusb_device mimusb_device;
typedef struct mimusb_spec mimusb_spec;
typedef struct mimusb_node mimusb_node;

/* ---- Constants --------------------------------------------------------------------------
 * Every enumeration is an int32_t typedef with anonymous constants, so structure layout does not
 * depend on how a compiler sizes an enum. */

/* A result libaoahid also has keeps the same number there. 5, 9 and 15 are not used. */
typedef int32_t mimusb_result;
enum {
    MIMUSB_OK = 0,
    MIMUSB_ERR_PARAM = 1,       /* a null pointer, a bad value or a handle in the wrong state */
    MIMUSB_ERR_UNSET_FIELD = 2, /* a required field or timeout is zero */
    MIMUSB_ERR_UNSUPPORTED = 3, /* not possible with these arguments or this board */
    MIMUSB_ERR_PROTOCOL = 4,    /* the board's firmware has another major or minor version */
    MIMUSB_ERR_ACCESS = 6,      /* no permission to open the serial port */
    MIMUSB_ERR_BUSY = 7,        /* in use: the port, a batch, an unsent edge, ... */
    MIMUSB_ERR_NO_DEVICE = 8,   /* the port failed or was unplugged; this stays until close */
    MIMUSB_ERR_TIMEOUT = 10,
    MIMUSB_ERR_SHORT_TRANSFER = 11,      /* a write moved fewer bytes than asked */
    MIMUSB_ERR_DESCRIPTOR_REJECTED = 12, /* the board refused the configuration */
    MIMUSB_ERR_IO = 13,
    MIMUSB_ERR_OVERFLOW = 14, /* a size, count or limit was exceeded */
    MIMUSB_ERR_INTERNAL = 16
};

typedef int32_t mimusb_log_level;
enum { MIMUSB_LOG_DISABLED = 1, MIMUSB_LOG_ERROR = 2, MIMUSB_LOG_INFO = 3, MIMUSB_LOG_TRACE = 4 };

/* The Application Collection of each profile is in docs/PROFILES.md. */
typedef int32_t mimusb_profile_kind;
enum {
    MIMUSB_PROFILE_KEYBOARD = 1,
    MIMUSB_PROFILE_MOUSE = 2,
    MIMUSB_PROFILE_TOUCHSCREEN = 3,
    MIMUSB_PROFILE_TOUCHPAD = 4,
    MIMUSB_PROFILE_PEN = 5,
    MIMUSB_PROFILE_DIGITIZER = 6,
    MIMUSB_PROFILE_GAMEPAD = 7,
    MIMUSB_PROFILE_TOGGLE = 8, /* one-bit controls on a page of the caller's choice */
    MIMUSB_PROFILE_RAW = 9
};

typedef int32_t mimusb_axis_role;
enum {
    MIMUSB_AXIS_X = 1,
    MIMUSB_AXIS_Y = 2,
    MIMUSB_AXIS_Z = 3,
    MIMUSB_AXIS_RX = 4,
    MIMUSB_AXIS_RY = 5,
    MIMUSB_AXIS_RZ = 6,
    MIMUSB_AXIS_SLIDER = 7,
    MIMUSB_AXIS_SIMULATION_ACCELERATOR = 8,
    MIMUSB_AXIS_SIMULATION_BRAKE = 9,
    MIMUSB_AXIS_SIMULATION_STEERING = 10,
    MIMUSB_AXIS_DIAL = 11,
    MIMUSB_AXIS_WHEEL = 12,
    MIMUSB_AXIS_SIMULATION_RUDDER = 13,
    MIMUSB_AXIS_SIMULATION_THROTTLE = 14
};

typedef int32_t mimusb_dpad_representation;
enum {
    MIMUSB_DPAD_NONE = 1,
    MIMUSB_DPAD_HAT = 2,    /* one eight-way Hat Switch */
    MIMUSB_DPAD_BUTTONS = 3 /* the four Generic Desktop D-pad Usages, one bit each */
};

/* The HUT control type of one Toggle Usage; it decides the descriptor encoding. Only the first six
 * can be pressed and released: a factory refuses the others with MIMUSB_ERR_UNSUPPORTED. */
typedef int32_t mimusb_usage_semantic;
enum {
    MIMUSB_USAGE_SELECTOR_BITMAP = 1,
    MIMUSB_USAGE_ON_OFF_TOGGLE = 2,
    MIMUSB_USAGE_ON_OFF_MAINTAINED = 3,
    MIMUSB_USAGE_MOMENTARY = 4,
    MIMUSB_USAGE_ONE_SHOT = 5,
    MIMUSB_USAGE_RETRIGGER = 6,
    MIMUSB_USAGE_ON_OFF_PAIR = 7,
    MIMUSB_USAGE_LINEAR = 8,
    MIMUSB_USAGE_DYNAMIC_VALUE = 9,
    MIMUSB_USAGE_NAMED_ARRAY = 10
};

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* Called with one formatted line per event (see "Threads" above). */
typedef void(MIMUSB_CALL* mimusb_log_sink)(void* user, mimusb_log_level level, const char* message);

/* The details of the calling thread's last result. Fields that do not apply are zero or null. */
typedef struct mimusb_error_detail {
    mimusb_result code;
    const char* field;  /* the argument, option or step concerned, e.g. "node.close" */
    const char* reason; /* one English sentence */
    /* errno or the Win32 error of a failed serial call; or, when the board refused a frame, its
     * code: 1 bad value, 2 no room, 3 wrong state, 4 descriptor refused, 5 unsupported, 6 busy */
    int32_t transport_status;
    int32_t frame_type; /* the serial frame involved; mimusb_frame_type_name names it */
    uint16_t node_id;   /* the node id of the Node involved (mimusb_node_id) */
    uint16_t report_id;
    uint32_t offset;
    uint32_t length;
} mimusb_error_detail;

/* ---- Events -----------------------------------------------------------------------------
 * What the board reports about the target. Delivered to transport.event_sink (see "Threads"
 * above); data is valid only during the call, so copy what you keep. */

typedef int32_t mimusb_event_kind;
enum {
    MIMUSB_EVENT_MOUNTED = 1, /* the target configured the board's USB device */
    MIMUSB_EVENT_UNMOUNTED = 2,
    MIMUSB_EVENT_SUSPEND = 3, /* value: 1 when the target enabled remote wakeup */
    MIMUSB_EVENT_RESUME = 4,
    /* The target sent an Output report (a keyboard's LEDs, for example) or a Feature report to a
     * profile Node (mimusb_node_open_profile), on its OUT endpoint or with SET_REPORT; and set the
     * protocol or the idle rate of its interface. The library makes these events from the
     * control rules and the packets it wrote for the Node (they are not CONTROL_SETUP events). */
    MIMUSB_EVENT_OUTPUT_REPORT = 5,
    MIMUSB_EVENT_FEATURE_REPORT = 6,
    MIMUSB_EVENT_SET_PROTOCOL = 7, /* value: 0 boot protocol, 1 report protocol */
    MIMUSB_EVENT_SET_IDLE = 8,     /* value: the idle rate in 4 ms units */
    /* A control transfer of the target on endpoint 0 that a rule of the caller reports (an ASK or
     * an ACCEPT rule, or every transfer: see mimusb_device_set_control_rules).
     * data: the 8 setup bytes (bmRequestType, bRequest, wValue, wIndex, wLength; little endian). */
    MIMUSB_EVENT_CONTROL_SETUP = 9,
    /* The data stage of a held host-to-device transfer, after the answer accepted it. The board
     * waits for a second answer, the verdict, with the same control_token. */
    MIMUSB_EVENT_CONTROL_DATA = 10,
    /* The board's queue of an IN endpoint was full and it dropped transfers: value is how many
     * since the last event of this kind, at most 65535; endpoint_address names the endpoint. On a
     * HID Node a key-up may be among them: send the current state again. With
     * MIMUSB_FULL_DROP_OLDEST the dropped ones are the oldest. Only after
     * mimusb_device_set_loss_reporting. */
    MIMUSB_EVENT_TRANSFER_LOST = 11,
    /* The target sent a packet on an OUT endpoint of a Node opened with mimusb_node_open, or of a
     * Device given as bytes (node_id 0; mimusb_device_set_descriptors): data and length are the
     * packet as it came (a zero-length packet has length 0); endpoint_address names the endpoint
     * and interface_number its interface. */
    MIMUSB_EVENT_ENDPOINT_DATA = 12,
    /* The target set (value 1) or cleared (value 0) the Halt feature of an endpoint of a Node
     * opened with mimusb_node_open (USB 2.0 9.4.1, 9.4.5); endpoint_address names it. */
    MIMUSB_EVENT_ENDPOINT_HALT = 13,
    /* The target selected an alternate setting of an interface of a Device given as bytes (USB 2.0
     * 9.4.10): interface_number is the interface and value the setting. The board has already
     * switched the interface's isochronous endpoints: those of the setting that was left stopped
     * (what waited for them is dropped) and those of the new one are on the bus. Whether to send
     * on them, and what, is the program's: the board adds nothing to a stream and sends no packet
     * of its own. Sent for a setting that was already selected as well. */
    MIMUSB_EVENT_INTERFACE_ALTERNATE = 14
};

/* mimusb_event.control_flags */
enum {
    MIMUSB_CONTROL_FLAG_HELD = 0x01,     /* the board waits for mimusb_device_control_reply */
    MIMUSB_CONTROL_FLAG_TOO_LONG = 0x02, /* a data stage longer than the board takes; not held */
    MIMUSB_CONTROL_FLAG_FIXED = 0x04,    /* SET_ADDRESS or SET_CONFIGURATION, which the board's own
                                            USB state needs; not held */
    MIMUSB_CONTROL_FLAG_LOST = 0x08, /* transfers before this one were not reported: queue full */
    MIMUSB_CONTROL_FLAG_ACCEPTED = 0x10 /* an ACCEPT rule took it; nothing is awaited */
};

typedef struct mimusb_event {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_event_kind kind;
    uint16_t node_id; /* the Node the event belongs to, or 0 for the whole device */
    uint16_t report_id;
    uint32_t value;
    const uint8_t* data; /* OUTPUT_REPORT and FEATURE_REPORT: the report without its Report ID */
    size_t length;
    uint32_t control_token;    /* CONTROL_SETUP and CONTROL_DATA: 1 to 255 */
    uint32_t control_flags;    /* MIMUSB_CONTROL_FLAG_* */
    uint32_t endpoint_address; /* TRANSFER_LOST, ENDPOINT_DATA, ENDPOINT_HALT: 0x81 to 0x8F or 0x01
                                  to 0x0F; otherwise 0 */
    uint32_t interface_number; /* ENDPOINT_DATA, INTERFACE_ALTERNATE and the HID events: the
                                  bInterfaceNumber the event belongs to; otherwise 0 */
} mimusb_event;

typedef void(MIMUSB_CALL* mimusb_event_sink)(void* user, const mimusb_event* event);

/* ---- Boards -----------------------------------------------------------------------------*/

/* The rate a board starts at, and returns to on a UART BREAK: the one rate two parties that have
 * never spoken share. Any other rate from here to the board's max_baud can be agreed; there is
 * no table of them. */
#define MIMUSB_BOOT_BAUD 115200u
/* transport.baud_rate: measure the fastest rate this port, adapter and board carry now (what
 * mimusb_discover does), and use it. The rate is in mimusb_device_info.baud_rate afterwards. */
#define MIMUSB_BAUD_FASTEST 4294967295u

/* mimusb_device_info.uart_flags */
#define MIMUSB_UART_FLAG_BREAK 0x01u /* the board returns to the boot rate on a BREAK */

/* mimusb_device_info.chip */
#define MIMUSB_CHIP_UNKNOWN 0
#define MIMUSB_CHIP_RP2040 1
#define MIMUSB_CHIP_RP2350 2
#define MIMUSB_CHIP_ESP32S3 3
#define MIMUSB_CHIP_NRF52840 4
#define MIMUSB_CHIP_STM32F4 5

/* A board found by mimusb_discover, or the one a Device has open. */
typedef struct mimusb_device_info {
    char port_name[64];    /* "/dev/ttyUSB0", "/dev/cu.usbserial-0001", "COM3" */
    char description[64];  /* the serial adapter's USB product string, where the OS gives one */
    uint8_t version_major; /* firmware version */
    uint8_t version_minor;
    uint8_t version_patch;
    uint8_t is_ready; /* 1 when the target has mounted the board's USB device */
    uint8_t
        firmware_compatible;  /* 1 when the firmware's major and minor version are this library's */
    uint8_t max_interfaces;   /* interfaces the board provides, of any class
                                 (mimusb_board_limits.max_interfaces_any) */
    uint8_t max_report_bytes; /* at most 255 here; mimusb_device_limits has the exact figure */
    uint16_t max_descriptor_bytes; /* the board's answer memory: the bytes of every ANSWER rule
                                      together (mimusb_board_limits.max_answer_bytes) */
    uint8_t has_unique_id;         /* 1 when the chip has a serial number of its own */
    uint8_t max_endpoint_number;   /* the highest endpoint number the board's USB controller has */
    /* discover: the fastest rate that carried a burst of test frames without loss on this port
     * (the board is left on MIMUSB_BOOT_BAUD); a Device: the rate the link runs at */
    uint32_t baud_rate;
    uint32_t max_baud;        /* the fastest rate the board's UART can make */
    uint32_t uart_flags;      /* MIMUSB_UART_FLAG_* */
    uint8_t chip;             /* MIMUSB_CHIP_* */
    uint8_t usb_high_speed;   /* 1 when the board's USB device runs at high speed */
    uint8_t unique_id_length; /* bytes of unique_id in use */
    uint8_t unique_id[16];
    uint8_t reserved_end[3];
} mimusb_device_info;

/* What the board holds, in bytes: 64-byte endpoint packets on a full-speed chip, 1024 on a
 * high-speed one. The data stage of a control transfer may be longer than a packet. */
typedef struct mimusb_board_limits {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t max_report_bytes;       /* largest report, Report ID included */
    uint32_t max_feature_bytes;      /* the data stage of a control transfer, as
                                        max_control_data_bytes: a Feature report answer fits */
    uint32_t max_endpoint_bytes;     /* largest ep_in and ep_out max_packet_size */
    uint32_t max_frame_payload;      /* largest serial frame payload the board takes */
    uint32_t max_control_data_bytes; /* largest data stage the host may read or answer */
    uint32_t max_control_rules;      /* control rules the board holds: those of every Node and of
                                        mimusb_device_set_control_rules together */
    uint32_t max_endpoint_number;    /* highest endpoint number of an endpoint's address */
    uint32_t max_interfaces_any;     /* interfaces of every class together, HID ones included */
    uint32_t max_answer_bytes;       /* the bytes of every ANSWER control rule together */
    uint32_t max_config_descriptor_bytes; /* the whole Configuration descriptor */
    uint32_t max_strings;                 /* string descriptors, the four of the Device included */
    uint32_t max_transfer_bytes;   /* largest payload of one frame to an endpoint of a Node opened
                                      with mimusb_node_open; a longer write is split */
    uint32_t endpoint_queue_slots; /* slots the queues of those endpoints are cut from: a queue
                                      of depth d takes d + 1 (MIMUSB_FULL_DROP_NEWEST and
                                      MIMUSB_FULL_WAIT) or 2 * d + 1 (MIMUSB_FULL_DROP_OLDEST) */
    uint32_t transfer_types;       /* bit n set: the board has endpoints of transfer type n
                                      (MIMUSB_TRANSFER_ISOCHRONOUS, MIMUSB_TRANSFER_BULK,
                                      MIMUSB_TRANSFER_INTERRUPT). A board with isochronous
                                      endpoints takes packets of them up to max_transfer_bytes
                                      (1023 at full speed, 1024 at high speed at most) */
    uint32_t max_bos_bytes;        /* the largest BOS descriptor, its header included */
} mimusb_board_limits;

/* What the board counted on its serial line since the last reset, each saturating at 65535. A
 * counter that only latches in the hardware counts once per look, so a count is a lower bound and
 * zero is exact. */
typedef struct mimusb_link_counters {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t bad_crc;      /* frames with a wrong CRC */
    uint32_t framing;      /* UART framing errors (not BREAKs) */
    uint32_t uart_overrun; /* bytes the UART lost because nobody read it in time */
    uint32_t ring_overrun; /* reads that found the receive ring three quarters full or more */
    uint32_t probes_good;  /* link-test frames received intact (only the search sends them) */
} mimusb_link_counters;

/* ---- USB identity and Device options ----------------------------------------------------
 * The fields are those of the USB 2.0 standard descriptors (section 9.6) and the HID class
 * descriptor (HID 1.11 section 6.2.1). A string is UTF-8 text without control characters (C0, DEL,
 * C1) that makes at most 126 UTF-16 code units (USB 2.0 9.6.7: a string descriptor is UTF-16LE and
 * 254 bytes long at most; U+10000 and above take two), or null to leave it out. language_id says
 * which language the strings are in; nothing is inferred from the text. */

/* The Device descriptor and its strings. bcd_usb, ep0_max_packet_size and language_id are
 * required (zero is refused), and so is vendor_id: zero is taken for a field that was never set (an
 * identity is the caller's, and mimusb_device_config_init fills nothing in). product_id may be
 * any value, zero included. A host asks for the BOS descriptor only when bcd_usb is 0x0201 or more
 * (USB 2.0 Link Power Management ECN; USB 3.2 9.6.2): give one with mimusb_device_set_bos or
 * mimusb_device_set_descriptors, or the request is stalled.
 * While the Device is given as bytes (mimusb_device_set_descriptors) this structure, usb_config
 * and the four strings it points to are not used: the bytes are the Device descriptor. */
typedef struct mimusb_usb_device {
    uint16_t bcd_usb;        /* e.g. 0x0200; 0x0201 or more: a BOS descriptor */
    uint8_t device_class;    /* 0: each interface says its own class */
    uint8_t device_subclass; /* 0 when device_class is 0 (USB 2.0 9.6.1) */
    uint8_t device_protocol;
    uint8_t ep0_max_packet_size; /* 8, 16, 32 or 64; 64 on a high-speed board (USB 2.0 9.6.1) */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint16_t language_id; /* the LANGID of the strings, e.g. 0x0409 (English, United States) */
    const char* manufacturer;
    const char* product;
    const char* serial;
    uint32_t serial_from_chip_id; /* 1 with a null serial: the chip's unique id */
} mimusb_usb_device;

/* The Configuration descriptor. Not used while the Device is given as bytes
 * (mimusb_device_set_descriptors). */
typedef struct mimusb_usb_config {
    uint8_t configuration_value; /* 1: the board has one configuration, selected by that value */
    uint8_t bm_attributes;       /* bit 7 set; bit 6 self-powered; bit 5 remote wakeup; bits 4..0
                                    zero (USB 2.0 9.6.3) */
    uint8_t bmax_power;          /* 0 to 250, in 2 mA units */
    uint8_t reserved8;
    const char* string;
} mimusb_usb_config;

/* The Linux HID parser limits of the target (docs/LIMITS.md). A Spec that exceeds one is refused by
 * mimusb_node_open with MIMUSB_ERR_OVERFLOW. All are required. */
typedef struct mimusb_linux_hid_policy {
    uint32_t descriptor_bytes;   /* largest report descriptor */
    uint32_t fields_per_report;  /* fields in one report */
    uint32_t global_stack_depth; /* Push items in effect at once */
    uint32_t report_size_bits;   /* largest Report Size item */
    uint64_t usages;             /* Usages in one descriptor */
    uint64_t report_data_bits;   /* largest report, in bits */
} mimusb_linux_hid_policy;

/* The serial link and what the library does on it. */
typedef struct mimusb_transport_options {
    /* The line rate: MIMUSB_BOOT_BAUD, any rate up to the board's max_baud (a rate
     * mimusb_discover reported is known to work), or MIMUSB_BAUD_FASTEST. */
    uint32_t baud_rate;
    /* Each wait for an answer while mimusb_device_open looks for the board: a few frames at the
     * boot rate plus the latency timer of a USB serial adapter (16 ms on FTDI chips). */
    uint32_t handshake_timeout_ms;
    /* The largest report this Device sends, Report ID included: at most the board's
     * max_report_bytes and MIMUSB_HOST_MAX_REPORT_BYTES. */
    uint32_t maximum_report_bytes;
    mimusb_linux_hid_policy linux_hid;
    uint32_t validate_reports; /* 1 checks every built report against its Spec before sending */
    /* 1 leaves the board's USB device on the target after mimusb_device_close */
    uint32_t keep_attached;
    mimusb_event_sink event_sink; /* may be null */
    void* event_user;
    mimusb_log_level log_level; /* anything but MIMUSB_LOG_DISABLED needs log_sink */
    mimusb_log_sink log_sink;
    void* log_user;
} mimusb_transport_options;

typedef struct mimusb_device_config {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_transport_options transport;
    mimusb_usb_device usb_device;
    mimusb_usb_config usb_config;
} mimusb_device_config;

/* The waits of a call that configures the board and enumerates its USB device again (a commit). */
typedef struct mimusb_commit_timeouts {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t control_timeout_ms;   /* each configuration frame waits this long for the board's
                                      answer, and is sent up to three times */
    uint32_t reconnect_timeout_ms; /* the board's new enumeration */
} mimusb_commit_timeouts;

/* ---- Node options ----------------------------------------------------------------------- */

/* Transfer types of an endpoint: the values of bmAttributes bits 1..0 (USB 2.0 9.6.6). The board
 * has no control endpoints other than endpoint 0. Isochronous endpoints exist only on a board
 * whose mimusb_board_limits.transfer_types has bit MIMUSB_TRANSFER_ISOCHRONOUS, and only in a
 * Device given as bytes (mimusb_device_set_descriptors); mimusb_node_open refuses one with
 * MIMUSB_ERR_UNSUPPORTED. */
enum { MIMUSB_TRANSFER_ISOCHRONOUS = 1, MIMUSB_TRANSFER_BULK = 2, MIMUSB_TRANSFER_INTERRUPT = 3 };

/* What an IN endpoint does when `depth` transfers wait on the board for the target to read them.
 * Both fields are required on every IN endpoint (MIMUSB_ERR_UNSET_FIELD when 0) and zero on an OUT
 * endpoint (MIMUSB_ERR_PARAM otherwise). */
enum {
    MIMUSB_FULL_DROP_NEWEST = 1, /* the transfer that arrives is dropped: for state reports, where
                                    late is useless */
    MIMUSB_FULL_DROP_OLDEST = 2, /* the oldest transfer that is not being sent is dropped */
    MIMUSB_FULL_WAIT = 3         /* nothing is dropped: mimusb_node_endpoint_write blocks while
                                    `depth` transfers are unfinished */
};

typedef struct mimusb_endpoint_policy {
    uint8_t on_full; /* MIMUSB_FULL_* */
    uint8_t depth;   /* transfers that may wait, 1 to 255; the board's pool bounds it
                        (mimusb_board_limits.endpoint_queue_slots) */
} mimusb_endpoint_policy;

typedef struct mimusb_usb_endpoint {
    uint8_t address; /* IN: 0x81 to 0x8F; OUT: 0x01 to 0x0F: bit 7 the direction, bits 6..4 zero,
                        endpoint number 1 to 15 (USB 2.0 9.6.6); 0 for "no endpoint" where an
                        endpoint is optional */
    uint8_t transfer_type; /* MIMUSB_TRANSFER_BULK or MIMUSB_TRANSFER_INTERRUPT */
    uint8_t interval;      /* bInterval as it goes on the bus (USB 2.0 9.6.6): interrupt endpoints
                              1 to 255 (frames) at full speed, 1 to 16 (an exponent) at high
                              speed; for bulk it is sent as given */
    uint8_t reserved8;     /* zero */
    uint16_t max_packet_size; /* 1 to the board's max_endpoint_bytes; bulk: 8, 16, 32 or 64 at full
                                 speed, 512 at high speed (USB 2.0 5.8.3) */
    uint16_t reserved16;      /* zero */
    mimusb_endpoint_policy policy; /* IN endpoints; all zero on an OUT endpoint */
} mimusb_usb_endpoint;

/* What a CONTROL_REPLY says (mimusb_device_control_reply) and what an ASK rule does when no reply
 * came in time (STALL or DROP). */
typedef int32_t mimusb_control_action;
enum {
    MIMUSB_CONTROL_ACTION_LEAVE = 1,  /* the board's own handling, as if it had not asked */
    MIMUSB_CONTROL_ACTION_ANSWER = 2, /* data (device to host) or an acknowledgement */
    MIMUSB_CONTROL_ACTION_STALL = 3,
    MIMUSB_CONTROL_ACTION_DROP = 4 /* no answer and no STALL, until the target gives up */
};

#define MIMUSB_CONTROL_MAX_TIMEOUT_MS 5000 /* USB 2.0 9.2.6.1: 5 seconds for any command */

/* What a rule does with the request it matches. */
typedef int32_t mimusb_control_rule_action;
enum {
    /* Returns `data` (cut to wLength), straight from the board's memory. Device to host requests
     * only. mimusb_node_set_rule_data / mimusb_device_set_rule_data change the bytes later, to
     * at most the length given when the rules were set. */
    MIMUSB_CONTROL_ANSWER = 1,
    /* Returns the last transfer the board started on IN endpoint endpoint_address (with
     * first_byte_filter 1: the last one whose first byte is first_byte), cut to wLength; stalls
     * when none was started since the configuration was committed. The transfer is kept by the
     * board as it sends it, so nothing is copied on the way to the target. Device to host only. */
    MIMUSB_CONTROL_ANSWER_LAST_SENT = 2,
    /* Takes the data stage (up to max_control_data_bytes) and completes the transfer; the program
     * hears of it as MIMUSB_EVENT_CONTROL_SETUP (flag ACCEPTED) and, when there was a data stage,
     * MIMUSB_EVENT_CONTROL_DATA. Nothing is awaited. */
    MIMUSB_CONTROL_ACCEPT = 3,
    /* Holds the transfer and asks the program (CONTROL_SETUP with flag HELD), which answers with
     * mimusb_device_control_reply within timeout_ms; after it the board does when_late. */
    MIMUSB_CONTROL_ASK = 4,
    MIMUSB_CONTROL_STALL = 5
};

/* A request matches when (field & mask) == value for each of the four fields; a mask of 0 matches
 * anything. Rules are tried in order and the first match decides. */
typedef struct mimusb_control_rule {
    uint8_t request_type_mask; /* bmRequestType: direction, type and recipient */
    uint8_t request_type_value;
    uint8_t request_mask; /* bRequest */
    uint8_t request_value;
    uint16_t value_mask; /* wValue */
    uint16_t value_value;
    uint16_t index_mask; /* wIndex */
    uint16_t index_value;
    mimusb_control_rule_action action; /* required */
    /* ANSWER: the bytes (copied when the rule is given; at most the board's max_answer_bytes for
     * all rules together) */
    const uint8_t* data;
    size_t length;
    /* ANSWER_LAST_SENT: an IN endpoint address (0x81 to 0x8F), and the filter on the first byte
     * (0 or 1; a Report ID is the first byte of a report on an interface that has them) */
    uint8_t endpoint_address;
    uint8_t first_byte_filter;
    uint8_t first_byte;
    uint8_t reserved8;
    /* ASK: 1 to MIMUSB_CONTROL_MAX_TIMEOUT_MS: how long the board waits for each answer. USB 2.0
     * expects the first data packet of a standard request within 500 ms (9.2.6.4) and a whole
     * control transfer within 5 s (9.2.6.1); hosts may give up sooner, so keep it short. */
    uint32_t timeout_ms;
    mimusb_control_action when_late; /* ASK: MIMUSB_CONTROL_ACTION_STALL or _DROP */
} mimusb_control_rule;

/* One interface of a Node, as the target sees it (USB 2.0 9.6.5). The library puts class_code,
 * subclass and protocol in the Interface descriptor and does not read them, with two exceptions
 * mimusb_node_open makes: 0 is MIMUSB_ERR_UNSET_FIELD. HID (0x03) is accepted: the board has no
 * HID driver, and the HID descriptor goes in class_descriptors as given. `number` is the
 * bInterfaceNumber, never renumbered: the numbers in use at a commit, over all Nodes, are 0, 1,
 * 2, ... without a gap (MIMUSB_ERR_PARAM otherwise, with the missing number in
 * mimusb_error_detail.offset). class_descriptors are bytes placed after the Interface descriptor
 * exactly as given (a CDC functional descriptor, for instance); each descriptor's bLength must be
 * at least 2 and stay inside the bytes. A string index inside them comes from
 * mimusb_device_add_string. name, if not null, becomes the interface's string. */
typedef struct mimusb_usb_interface {
    uint8_t number;
    uint8_t class_code; /* required, never 0 */
    uint8_t subclass;
    uint8_t protocol;
    const char* name;
    const uint8_t* class_descriptors;
    size_t class_descriptors_length;
    const mimusb_usb_endpoint* endpoints;
    size_t endpoint_count;
} mimusb_usb_interface;

/* The interfaces of one Node. An interface association descriptor (USB-IF Interface Association
 * Descriptor ECN) is given whole: 8 bytes, type 0x0B, whose bFirstInterface and bInterfaceCount
 * name this Node's interfaces, which must then have consecutive numbers. It goes before the first
 * of them. Endpoint addresses are unique across the device. */
typedef struct mimusb_node_config {
    uint32_t struct_size;
    uint32_t reserved;
    const mimusb_usb_interface* interfaces;
    size_t interface_count;
    const uint8_t* association; /* null for none */
    size_t association_length;
    /* What the board does with the Node's control requests, as mimusb_control_rule says; copied.
     * They are tried before those of mimusb_device_set_control_rules, in the order of the Nodes
     * and then of this array. */
    const struct mimusb_control_rule* rules;
    size_t rule_count;
} mimusb_node_config;

/* The interface and endpoints of a HID profile Node (mimusb_node_open_profile).
 * Nodes with the same interface_number share one interface, and must then
 * all have Report IDs, all different, and the same interface and endpoint settings. Both endpoints
 * are interrupt endpoints. The IN endpoint's policy is any of the three; its depth is how many
 * reports may wait on the board, at most what the board's queues hold (MIMUSB_ERR_OVERFLOW). */
typedef struct mimusb_hid_node_config {
    uint32_t struct_size;
    uint32_t reserved;
    uint8_t interface_number; /* as mimusb_usb_interface.number */
    uint8_t subclass;         /* 0 none, 1 boot interface */
    uint8_t protocol;         /* 0 none, 1 keyboard, 2 mouse (HID 1.11 4.3) */
    uint8_t country_code;     /* 0 when the hardware is not localized, up to 35 */
    uint16_t bcd_hid;         /* nonzero, e.g. 0x0111 */
    uint16_t reserved16;      /* zero */
    const char* name;
    mimusb_usb_endpoint ep_in;  /* required */
    mimusb_usb_endpoint ep_out; /* all zero for none */
} mimusb_hid_node_config;

/* A whole USB device as the bytes the target reads (mimusb_device_set_descriptors), to present a
 * copy of a real device. Nothing is changed in them and nothing is filled in. */
typedef struct mimusb_device_descriptors {
    uint32_t struct_size;
    uint32_t reserved;
    const uint8_t* device; /* the 18-byte device descriptor (USB 2.0 9.6.1) */
    size_t device_length;
    const uint8_t* configuration; /* the whole configuration descriptor (USB 2.0 9.6.3) */
    size_t configuration_length;
    const uint8_t* bos; /* the whole BOS descriptor (USB 3.2 9.6.2), or null for none */
    size_t bos_length;
} mimusb_device_descriptors;

/* ---- Profile options --------------------------------------------------------------------
 * docs/PROFILES.md describes each profile's descriptor and the limits of every field. A value
 * that may be zero (a logical minimum, a neutral value, a count of optional parts) is never
 * taken for "unset": what is optional has its own enable_* flag or count. */

/* The four HID 1.11 physical Global items for one field. They are emitted only when enabled is 1
 * and are reset right after the field, so they never apply to the next one. */
typedef struct mimusb_physical_properties {
    uint32_t enabled;
    int32_t minimum;
    int32_t maximum;
    int32_t unit_exponent;
    uint32_t unit;
} mimusb_physical_properties;

/* One report field. The logical range must fit bit_width (1 to 32 bits); a bit_width of zero is
 * an unset field. */
typedef struct mimusb_integer_field {
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t bit_width;
    mimusb_physical_properties physical;
} mimusb_integer_field;

/* With enabled 1, every report starts with value (1 to 255). */
typedef struct mimusb_report_id_option {
    uint32_t enabled;
    uint8_t value;
    uint8_t reserved8[3];
} mimusb_report_id_option;

/* Battery Strength (Generic Device Controls 0x06/0x20) as a second Input report in the profile's
 * Application Collection. Both this report_id and the profile's must be enabled and differ.
 * Start the range at 1: some Linux releases ignore a value of 0 (docs/PROFILES.md). */
typedef struct mimusb_battery_report {
    mimusb_report_id_option report_id;
    mimusb_integer_field strength;
} mimusb_battery_report;

/* Full N-key rollover: one bit per Usage from usage_minimum (at least 0x04) to usage_maximum, plus
 * the eight modifiers 0xE0 to 0xE7, which the range must not include. */
typedef struct mimusb_keyboard_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    uint16_t usage_minimum;
    uint16_t usage_maximum;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_keyboard_options;

/* X, Y, wheel and pan are relative; their ranges must hold negative and positive values. */
typedef struct mimusb_mouse_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    uint32_t button_count; /* 1 to 65535 */
    mimusb_integer_field x;
    mimusb_integer_field y;
    uint32_t enable_wheel;
    mimusb_integer_field wheel;
    uint32_t enable_pan;
    mimusb_integer_field pan;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_mouse_options;

/* One-bit controls on any HUT page: Consumer, System Control, Camera Control, Telephony, ...
 * allowed_usages, usage_semantics, expected_linux_event_types and expected_linux_codes are
 * parallel arrays of allowed_usage_count entries, all required. The expected Linux strings (such
 * as "EV_KEY" and "KEY_PLAYPAUSE") are kept with the Spec as a record; the library does not read
 * them. On the Camera Control page (0x90) only Auto-focus (0x20) and Shutter (0x21) exist, each
 * as MIMUSB_USAGE_ONE_SHOT. */
typedef struct mimusb_toggle_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    uint16_t application_page; /* the Application Collection, e.g. 0x0C/0x01 Consumer Control */
    uint16_t application_usage;
    uint16_t field_page; /* the Usage Page of every allowed Usage */
    uint16_t reserved16;
    const uint16_t* allowed_usages;
    size_t allowed_usage_count;
    const mimusb_usage_semantic* usage_semantics;
    const char* const* expected_linux_event_types;
    const char* const* expected_linux_codes;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_toggle_options;

/* One axis. usage_page and usage must be the ones of role (docs/PROFILES.md); neutral_value is the
 * value sent at rest and on close, and may be any value in the range, zero included. The expected
 * string (such as "ABS_X") is required and kept with the Spec as a record; the library does not
 * read it. */
typedef struct mimusb_gamepad_axis_option {
    mimusb_axis_role role;
    uint16_t usage_page;
    uint16_t usage;
    mimusb_integer_field value;
    int32_t neutral_value;
    const char* expected_linux_code;
} mimusb_gamepad_axis_option;

/* At least two axes, one of them MIMUSB_AXIS_X and one MIMUSB_AXIS_Y, each role once. A Hat must
 * be logical 0 to 7 in 4 bits; outside MIMUSB_DPAD_HAT the three hat_* fields are zero. */
typedef struct mimusb_gamepad_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    const mimusb_gamepad_axis_option* axes;
    size_t axis_count;
    uint32_t button_count;         /* at least 1 */
    uint16_t button_usage_minimum; /* the Button page Usage of the first button, nonzero */
    mimusb_dpad_representation dpad_representation;
    int32_t hat_logical_minimum;
    int32_t hat_logical_maximum;
    uint32_t hat_bit_width;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_gamepad_options;

/* Fixed-slot Multi-Touch under the Digitizers / Touch Screen Application Collection. */
typedef struct mimusb_touchscreen_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    uint32_t maximum_contacts;          /* 1 to 16 */
    uint32_t contacts_per_report;       /* maximum_contacts, or fewer with multi-packet frames */
    mimusb_integer_field contact_id;    /* holds 0 and at least maximum_contacts values */
    mimusb_integer_field x;             /* holds 0 */
    mimusb_integer_field y;             /* holds 0 */
    mimusb_integer_field contact_count; /* holds 0 to maximum_contacts */
    uint32_t enable_pressure;
    mimusb_integer_field pressure;
    uint32_t enable_width;
    mimusb_integer_field width;
    uint32_t enable_height;
    mimusb_integer_field height;
    uint32_t enable_azimuth;
    mimusb_integer_field azimuth; /* counter-clockwise, minimum 0 (docs/LIMITS.md) */
    uint32_t enable_scan_time;
    mimusb_integer_field scan_time;
    uint32_t scan_time_unit_100us; /* must be 1 with enable_scan_time */
    /* 1 adds Contact Count Maximum as a Feature report, which the board answers itself. */
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_touchscreen_options;

/* The touchscreen report under the Digitizers / Touch Pad Application Collection, plus
 * button_count click buttons (0 for a buttonless clickpad). */
typedef struct mimusb_touchpad_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    uint32_t maximum_contacts;
    uint32_t contacts_per_report;
    mimusb_integer_field contact_id;
    mimusb_integer_field x;
    mimusb_integer_field y;
    mimusb_integer_field contact_count;
    uint32_t enable_pressure;
    mimusb_integer_field pressure;
    uint32_t enable_width;
    mimusb_integer_field width;
    uint32_t enable_height;
    mimusb_integer_field height;
    uint32_t enable_azimuth;
    mimusb_integer_field azimuth;
    uint32_t enable_scan_time;
    mimusb_integer_field scan_time;
    uint32_t scan_time_unit_100us;
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t button_count;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_touchpad_options;

/* A stylus under the Digitizers / Pen Application Collection (a direct, on-screen pen). */
typedef struct mimusb_pen_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    mimusb_integer_field x;
    mimusb_integer_field y;
    uint32_t enable_pressure;
    mimusb_integer_field pressure;
    uint32_t enable_tilt; /* X Tilt and Y Tilt together */
    mimusb_integer_field tilt_x;
    mimusb_integer_field tilt_y;
    uint32_t enable_twist_target_specific; /* how a target uses Twist varies */
    mimusb_integer_field twist;
    /* Up to 32 unique Digitizers-page Usages, each Barrel Switch (0x44) or Secondary Barrel
     * Switch (0x5A); bit i of a sample's barrel_buttons is barrel_usages[i]. A count of 0 means
     * no barrel button. */
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    /* 1 adds HUT Invert (0x3C), which selects the eraser end; not the Eraser Usage (0x45). */
    uint32_t enable_eraser;
    uint32_t enable_hover; /* 1 lets a sample be in range without touching */
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_pen_options;

/* The pen report under the Digitizers / Digitizer Application Collection (an indirect tablet). */
typedef struct mimusb_digitizer_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimusb_report_id_option report_id;
    mimusb_integer_field x;
    mimusb_integer_field y;
    uint32_t enable_pressure;
    mimusb_integer_field pressure;
    uint32_t enable_tilt;
    mimusb_integer_field tilt_x;
    mimusb_integer_field tilt_y;
    uint32_t enable_twist_target_specific;
    mimusb_integer_field twist;
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    uint32_t enable_eraser;
    uint32_t enable_hover;
    uint32_t enable_battery;
    mimusb_battery_report battery;
} mimusb_digitizer_options;

/* An Input report a Raw Node may send: its Report ID and its exact length on the wire. */
typedef struct mimusb_raw_report {
    mimusb_report_id_option report_id;
    uint32_t wire_length;
} mimusb_raw_report;

/* A descriptor of the caller's own. A descriptor with Output items needs requires_output 1 (they
 * arrive as MIMUSB_EVENT_OUTPUT_REPORT), one with Feature items requires_feature_response 1 (the
 * board answers with mimusb_node_set_feature_response). */
typedef struct mimusb_raw_options {
    uint32_t struct_size;
    uint32_t reserved;
    const uint8_t* descriptor;
    size_t descriptor_length;
    const mimusb_raw_report* reports;
    size_t report_count;
    uint32_t requires_output;
    uint32_t requires_feature_response;
} mimusb_raw_options;

typedef struct mimusb_report_capability {
    mimusb_report_id_option report_id;
    uint32_t wire_length;
} mimusb_report_capability;

/* What a Spec sends. Filled in by mimusb_spec_manifest and mimusb_node_manifest. */
typedef struct mimusb_capability_manifest {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t input_supported;
    uint32_t output_supported;            /* always 1: the board passes Output reports on */
    uint32_t feature_transport_supported; /* always 1: the board answers Feature reads */
    mimusb_profile_kind profile_kind;
    const mimusb_report_capability* reports; /* valid while the Spec is */
    size_t report_count;
    size_t descriptor_length;
} mimusb_capability_manifest;

/* ---- Input samples ---------------------------------------------------------------------- */

/* Optional values of one touch contact. A field the Spec did not enable must be 0. */
typedef struct mimusb_touchscreen_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimusb_touchscreen_extra;

typedef struct mimusb_touchpad_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimusb_touchpad_extra;

/* One pen state. tip 1 needs in_range 1; without enable_hover, in_range must equal tip. A field the
 * Spec did not enable must be 0. */
typedef struct mimusb_pen_sample {
    uint32_t in_range;
    uint32_t tip;
    uint32_t eraser;         /* 1 for the eraser end; tip may be 1 too */
    uint32_t barrel_buttons; /* bit i is barrel_usages[i] */
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} mimusb_pen_sample;

typedef struct mimusb_digitizer_sample {
    uint32_t in_range;
    uint32_t tip;
    uint32_t eraser;
    uint32_t barrel_buttons;
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} mimusb_digitizer_sample;

/* ---- Initializers -----------------------------------------------------------------------
 * Each *_init function zeroes a structure and sets struct_size, and nothing else: no value is
 * filled in for the caller. The USB identity, the strings, the timeouts, the baud rate, the
 * endpoints, the Linux HID limits and every profile's fields are the caller's to set; one left at
 * zero where zero is not valid is refused with MIMUSB_ERR_UNSET_FIELD (or MIMUSB_ERR_PARAM). A
 * value the caller sets is used as given, zero included where zero is valid. */
static inline void mimusb_struct_init(void* value, size_t size) {
    if (value != NULL) {
        const uint32_t struct_size = (uint32_t)size;
        memset(value, 0, size);
        memcpy(value, &struct_size, sizeof(struct_size));
    }
}

static inline mimusb_integer_field
mimusb_integer_field_make(int32_t logical_minimum, // NOLINT(bugprone-easily-swappable-parameters)
                          int32_t logical_maximum, uint32_t bit_width) {
    mimusb_integer_field field;
    memset(&field, 0, sizeof(field));
    field.logical_minimum = logical_minimum;
    field.logical_maximum = logical_maximum;
    field.bit_width = bit_width;
    return field;
}

static inline void mimusb_device_config_init(mimusb_device_config* config) {
    mimusb_struct_init(config, sizeof(*config));
}

static inline void mimusb_commit_timeouts_init(mimusb_commit_timeouts* timeouts) {
    mimusb_struct_init(timeouts, sizeof(*timeouts));
}

static inline void mimusb_node_config_init(mimusb_node_config* config) {
    mimusb_struct_init(config, sizeof(*config));
}

static inline void mimusb_hid_node_config_init(mimusb_hid_node_config* config) {
    mimusb_struct_init(config, sizeof(*config));
}

static inline void mimusb_board_limits_init(mimusb_board_limits* limits) {
    mimusb_struct_init(limits, sizeof(*limits));
}

static inline void mimusb_link_counters_init(mimusb_link_counters* counters) {
    mimusb_struct_init(counters, sizeof(*counters));
}

static inline void mimusb_keyboard_options_init(mimusb_keyboard_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_mouse_options_init(mimusb_mouse_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_toggle_options_init(mimusb_toggle_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_gamepad_options_init(mimusb_gamepad_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_touchscreen_options_init(mimusb_touchscreen_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_touchpad_options_init(mimusb_touchpad_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_pen_options_init(mimusb_pen_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_digitizer_options_init(mimusb_digitizer_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_raw_options_init(mimusb_raw_options* options) {
    mimusb_struct_init(options, sizeof(*options));
}

static inline void mimusb_capability_manifest_init(mimusb_capability_manifest* manifest) {
    mimusb_struct_init(manifest, sizeof(*manifest));
}

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* mimusb_last_error
 * Ownership: Returns the calling thread's record, valid until that thread's next call.
 * Blocking: None.
 * Synchronization: The record belongs to the calling thread; do not pass it to another.
 * Returns: Never null. */
MIMUSB_API const mimusb_error_detail* MIMUSB_CALL mimusb_last_error(void);

/* mimusb_result_name
 * Ownership: The string is static.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: The constant's name, such as "MIMUSB_ERR_TIMEOUT", with MIMUSB_OK recorded; for a value
 * that is not a result, "MIMUSB_RESULT_UNKNOWN" with MIMUSB_ERR_PARAM recorded. */
MIMUSB_API const char* MIMUSB_CALL mimusb_result_name(mimusb_result result);

/* mimusb_frame_type_name
 * Ownership: The string is static.
 * Blocking: None.
 * Synchronization: Any thread.
 * Names the serial frame type of mimusb_error_detail.frame_type (docs/PROTOCOL.md).
 * Returns: The frame's name, such as "COMMIT" or "EP_IN", with MIMUSB_OK recorded; for a value
 * that is not a frame type, "UNKNOWN" with MIMUSB_ERR_PARAM recorded. */
MIMUSB_API const char* MIMUSB_CALL mimusb_frame_type_name(int32_t frame_type);

/* mimusb_version
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: (major << 24) | (minor << 16) | patch of the library that is running. */
MIMUSB_API uint32_t MIMUSB_CALL mimusb_version(void);

/* ---- Discovery -------------------------------------------------------------------------- */

/* mimusb_discover
 * Ownership: The caller destroys the snapshot with mimusb_discovery_destroy.
 * Blocking: Searches every USB serial port at once, one thread each, waiting up to timeout_ms for
 * each answer; a port another program has open is skipped. On each port it pings at the boot
 * rate and, when nothing answers, sends a BREAK (which returns a board a program left on a fast
 * rate to the boot rate) and pings again, so a serial device that is not a board receives a few
 * pings and a BREAK. On a board it measures the fastest rate the link carries: from the board's
 * max_baud downward, it sends bursts of test frames and trusts a rate only when the board's own
 * error counters stay at zero (baud_rate), and it leaves the board on the boot rate. The search
 * takes a few seconds at most. A board whose firmware has another major or minor version is
 * listed with only its version and firmware_compatible 0, so that it can be flashed again.
 * Synchronization: Any thread.
 * Returns: MIMUSB_OK, also when no board was found; MIMUSB_ERR_PARAM for a null out_discovery;
 * MIMUSB_ERR_UNSET_FIELD for a zero timeout. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_discover(uint32_t timeout_ms,
                                                     mimusb_discovery** out_discovery);

/* mimusb_discovery_count
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread, but not during mimusb_discovery_destroy.
 * Returns: The number of boards with MIMUSB_OK recorded, or 0 with MIMUSB_ERR_PARAM recorded for
 * a null snapshot. */
MIMUSB_API size_t MIMUSB_CALL mimusb_discovery_count(const mimusb_discovery* discovery);

/* mimusb_discovery_get
 * Ownership: The entry belongs to the snapshot.
 * Blocking: None.
 * Synchronization: As mimusb_discovery_count.
 * Returns: The entry with MIMUSB_OK recorded, or null with MIMUSB_ERR_PARAM recorded for a null
 * snapshot or an index out of range. */
MIMUSB_API const mimusb_device_info* MIMUSB_CALL
mimusb_discovery_get(const mimusb_discovery* discovery, size_t index);

/* mimusb_discovery_destroy
 * Ownership: Frees the snapshot and every entry of it.
 * Blocking: None.
 * Synchronization: No other call may use the snapshot meanwhile.
 * Returns: Nothing. MIMUSB_OK is recorded, or MIMUSB_ERR_PARAM for null (nothing is done). */
MIMUSB_API void MIMUSB_CALL mimusb_discovery_destroy(mimusb_discovery* discovery);

/* ---- Device ----------------------------------------------------------------------------- */

/* mimusb_device_open
 * Ownership: Copies config; the caller closes the Device with mimusb_device_close.
 * Blocking: Opens port_path (such as "/dev/ttyACM0" or "COM3") for this process alone, looks for
 * the board at the boot rate (and after a BREAK, for one left on a fast rate; waiting
 * transport.handshake_timeout_ms for each answer), reads its limits, moves the link to
 * transport.baud_rate and checks it with a short burst of test frames (MIMUSB_BAUD_FASTEST
 * measures instead, which takes up to a few seconds), and starts the reader thread. It sends no
 * configuration: the board's USB device stays off the target until a Node is opened.
 * Synchronization: Any thread, one call per port.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, a wrong struct_size or a bad value;
 * MIMUSB_ERR_UNSET_FIELD for a required field left zero; MIMUSB_ERR_UNSUPPORTED for a rate the
 * board cannot make, or one the adapter or wiring did not carry;
 * MIMUSB_ERR_OVERFLOW for a maximum_report_bytes the board does not take; MIMUSB_ERR_TIMEOUT when
 * no board answered; MIMUSB_ERR_PROTOCOL for firmware of another major or minor version;
 * MIMUSB_ERR_ACCESS, MIMUSB_ERR_BUSY or MIMUSB_ERR_IO for a port that cannot be opened (another
 * program may have it) or set up. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_open(const char* port_path,
                                                        const mimusb_device_config* config,
                                                        mimusb_device** out_device);

/* mimusb_device_close
 * Ownership: Follows the "Closing" rule; the Device's Nodes go with it.
 * Blocking: Sends the neutral report of every open Node (keys and buttons up, contacts lifted,
 * axes at rest) within drain_timeout_ms, takes the board's USB device off the target unless
 * transport.keep_attached is 1, removes the control routing and loss reporting, puts the board
 * back on the boot rate and closes the port. Each configuration frame waits up to
 * control_timeout_ms. A failed step does not stop the next one.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null or closing Device; MIMUSB_ERR_UNSET_FIELD for a
 * zero timeout; MIMUSB_ERR_NO_DEVICE when the link was already gone, so the board was not reset;
 * otherwise the first link error on the way out. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_close(mimusb_device* device,
                                                         uint32_t control_timeout_ms,
                                                         uint32_t drain_timeout_ms);

/* mimusb_device_set_usb_identity
 * Ownership: Copies usb_device and usb_config.
 * Blocking: Commits (see the top of this header), waiting as timeouts says; inside a batch it only
 * stages the change.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, a wrong struct_size or a bad value;
 * MIMUSB_ERR_UNSET_FIELD for a required field or timeout left zero; the results of
 * mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_usb_identity(
    mimusb_device* device, const mimusb_usb_device* usb_device, const mimusb_usb_config* usb_config,
    const mimusb_commit_timeouts* timeouts);

/* mimusb_device_latched_error
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Reads the error the reader thread recorded for the link, and forgets it unless it is
 * MIMUSB_ERR_NO_DEVICE, which stays.
 * Returns: MIMUSB_OK when there is none; MIMUSB_ERR_PARAM for an invalid Device;
 * MIMUSB_ERR_NO_DEVICE after the port failed or was unplugged; MIMUSB_ERR_IO for another link
 * error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_latched_error(mimusb_device* device);

/* mimusb_device_is_ready
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread while the Device is open.
 * Returns: 1 while the target has the board's USB device mounted and takes reports, else 0. */
MIMUSB_API int32_t MIMUSB_CALL mimusb_device_is_ready(const mimusb_device* device);

/* mimusb_device_wait_ready
 * Ownership: Nothing.
 * Blocking: Until the target mounts the board's USB device, or timeout_ms.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK once mounted; MIMUSB_ERR_PARAM for an invalid Device; MIMUSB_ERR_TIMEOUT;
 * MIMUSB_ERR_NO_DEVICE when the link fails meanwhile. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_wait_ready(mimusb_device* device,
                                                              uint32_t timeout_ms);

/* mimusb_device_batch_begin
 * Ownership: Nothing.
 * Blocking: None; nothing is sent. Until mimusb_device_batch_end, mimusb_node_open,
 * mimusb_node_close and mimusb_device_set_usb_identity only stage their change, and a Node opened
 * in the batch refuses reports with MIMUSB_ERR_BUSY.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device; MIMUSB_ERR_BUSY inside a batch. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_batch_begin(mimusb_device* device);

/* mimusb_device_batch_end
 * Ownership: Nothing.
 * Blocking: Commits every change staged since mimusb_device_batch_begin at once, waiting as
 * timeouts says.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null timeouts or a wrong
 * struct_size; MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_BUSY outside a batch; the
 * results of mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_device_batch_end(mimusb_device* device, const mimusb_commit_timeouts* timeouts);

/* mimusb_device_commit
 * Ownership: Nothing.
 * Blocking: Sends the configuration of every open Node and commits, waiting as timeouts says.
 * mimusb_node_open, mimusb_node_close and mimusb_device_batch_end do this already; call it to
 * retry after a failed commit, or for reconnect_delay_ms: with a nonzero delay on an attached
 * device the board keeps its USB device off the target (D+ at 0 V) for that many milliseconds.
 * Inside a batch it commits the staged changes and ends the batch.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or timeouts; MIMUSB_ERR_UNSET_FIELD
 * for a zero timeout; MIMUSB_ERR_OVERFLOW when the Nodes do not fit the board;
 * MIMUSB_ERR_DESCRIPTOR_REJECTED when the board refuses the configuration; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_commit(mimusb_device* device,
                                                          uint32_t reconnect_delay_ms,
                                                          const mimusb_commit_timeouts* timeouts);

/* mimusb_device_connect
 * Ownership: Nothing.
 * Blocking: A Device that is not on the target (not configured yet, or after
 * mimusb_device_disconnect) commits instead, which puts the board's USB device on the target (D+
 * pull-up on) and waits for the board's answer. A Device that is on the target already sends
 * nothing and returns MIMUSB_OK: the board refuses a commit that attaches while it is attached.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or timeouts; MIMUSB_ERR_UNSET_FIELD
 * for a zero timeout; a link error; the results of mimusb_device_commit when it commits. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_connect(mimusb_device* device,
                                                           const mimusb_commit_timeouts* timeouts);

/* mimusb_device_disconnect
 * Ownership: Nothing.
 * Blocking: Takes the board's USB device off the target (D+ at 0 V, as if unplugged), waiting up
 * to control_timeout_ms for the board's answer. The next connect or commit configures it again.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_disconnect(mimusb_device* device,
                                                              uint32_t control_timeout_ms);

/* mimusb_device_remote_wakeup
 * Ownership: Nothing.
 * Blocking: Waits up to control_timeout_ms for the board's answer.
 * Synchronization: Device domain.
 * Wakes a target that suspended the board's USB device; the target must have enabled remote
 * wakeup (usb_config.bm_attributes bit 5 and MIMUSB_EVENT_SUSPEND with value 1).
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; MIMUSB_ERR_BUSY when the device is not suspended; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_remote_wakeup(mimusb_device* device,
                                                                 uint32_t control_timeout_ms);

/* mimusb_device_enter_bootloader
 * Ownership: Nothing.
 * Blocking: Waits up to control_timeout_ms for the board's answer.
 * Synchronization: Device domain.
 * Restarts the board into its chip's own bootloader, where new firmware is written (the RPI-RP2
 * drive on the target-facing USB port of an RP2040 or RP2350, USB DFU or a USART on an STM32F4,
 * the ROM's download mode on an ESP32-S3, the bootloader in flash on an nRF52840 board that has
 * one).
 * The board first takes its USB device off the target. After MIMUSB_OK the board no longer
 * answers: the Device is good only for mimusb_device_close. Close does not know the board
 * restarted: it returns MIMUSB_ERR_NO_DEVICE only when the serial link itself failed. A Device
 * that was never committed has nothing to detach, so close returns MIMUSB_OK (seen on an RP2040);
 * for a committed one the detach COMMIT close sends gets no answer, and close returns the link
 * error (MIMUSB_ERR_TIMEOUT; from reading the code, not run).
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; MIMUSB_ERR_UNSUPPORTED on a board whose chip has no bootloader the firmware can enter;
 * a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_enter_bootloader(mimusb_device* device,
                                                                    uint32_t control_timeout_ms);

/* mimusb_device_board_info
 * Ownership: Fills the caller's out_info.
 * Blocking: None; a copy of what the board reported at open.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or a null out_info. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_board_info(const mimusb_device* device,
                                                              mimusb_device_info* out_info);

/* mimusb_device_limits
 * Ownership: Fills the caller's out_limits (set its struct_size; mimusb_board_limits_init does).
 * Blocking: None; a copy of what the board reported at open.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null out_limits or a wrong
 * struct_size. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_limits(const mimusb_device* device,
                                                          mimusb_board_limits* out_limits);

/* mimusb_device_link_counters
 * Ownership: Fills the caller's out_counters (set its struct_size; mimusb_link_counters_init
 * does).
 * Blocking: One command to the board, waiting up to control_timeout_ms for its answer.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null out_counters or a wrong
 * struct_size; MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_TIMEOUT or MIMUSB_ERR_IO
 * when the board did not answer. With reset
 * nonzero the board's counters are zeroed after being read: a program that wants to see a link
 * degrade reads them now and then. Any lost byte shows there, not in the report stream. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_device_link_counters(mimusb_device* device, uint32_t reset, uint32_t control_timeout_ms,
                            mimusb_link_counters* out_counters);

/* ---- Control transfers of the target ----------------------------------------------------
 * Besides the reports, the target talks to the board on endpoint 0: descriptors, class and vendor
 * requests, GET_REPORT, SET_REPORT. The board answers none of them on its own beyond what USB
 * itself fixes (SET_ADDRESS, SET_CONFIGURATION, GET_DESCRIPTOR of the device, configuration and
 * strings, the standard requests its USB stack serves, and an interface's GET_STATUS). Everything
 * else is a list of rules, which each Node brings (mimusb_node_config.rules; the profile Nodes
 * write theirs) and the Device adds to (mimusb_device_set_control_rules). The first rule whose
 * four masked fields match the setup packet decides; a request no rule matches is stalled. */

/* mimusb_device_set_control_rules
 * Ownership: Copies rules and the bytes they point to.
 * Blocking: Sends the whole table of the Device's Nodes and these rules again, with their answer
 * bytes, and waits up to control_timeout_ms for the board's answer to each frame. Inside a batch,
 * or before the first commit, nothing is sent: the rules go with the commit.
 * Synchronization: Device domain.
 * Replaces the Device's own rules, which come after those of the Nodes. report_all 1 reports every
 * control transfer (MIMUSB_EVENT_CONTROL_SETUP), whatever its rule; with no rules and report_all 0
 * the Device adds nothing. A held request delays only the target's control transfer, never the
 * reports, but while the board waits for the verdict of a host-to-device transfer its USB task
 * does nothing else. SET_ADDRESS and SET_CONFIGURATION are never held, nor a host-to-device data
 * stage longer than max_control_data_bytes. The rules stay until replaced or
 * mimusb_device_close.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, report_all, count or rule (a value
 * outside its mask, an unknown action, an ANSWER with null data and a length, an ANSWER or
 * ANSWER_LAST_SENT of a host-to-device request, an endpoint that is not an IN address, a
 * timeout over MIMUSB_CONTROL_MAX_TIMEOUT_MS, a when_late other than STALL or DROP);
 * MIMUSB_ERR_UNSET_FIELD for a zero control_timeout_ms, an action left 0, an ASK without
 * timeout_ms or when_late; MIMUSB_ERR_OVERFLOW for more rules or answer bytes than the board
 * holds with those of the Nodes; a link error. A rule naming an endpoint that is not an IN
 * endpoint of the committed configuration is refused by the board (the details name its NAK). */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_control_rules(
    mimusb_device* device, uint32_t report_all, const mimusb_control_rule* rules, size_t rule_count,
    uint32_t control_timeout_ms);

/* mimusb_device_set_rule_data
 * Ownership: Copies data.
 * Blocking: One command to the board, waiting up to control_timeout_ms; it is retried while the
 * board is answering from that rule.
 * Synchronization: Device domain.
 * Changes the bytes an ANSWER rule of mimusb_device_set_control_rules returns, without a new
 * table: the rule is number rule_index in that array. Any number of bytes up to the rule's
 * capacity, the length of its data when the rule was given (the rule then answers with that many,
 * and may be given more again later, up to the capacity). Inside a batch, or before the first
 * commit, it is kept for the commit.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a rule that is not an ANSWER rule, a
 * null data with a length or more bytes than the rule's capacity; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; MIMUSB_ERR_BUSY when the board kept answering from the rule; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_rule_data(mimusb_device* device,
                                                                 size_t rule_index,
                                                                 const uint8_t* data, size_t length,
                                                                 uint32_t control_timeout_ms);

/* mimusb_node_set_rule_data
 * Ownership: Copies data.
 * Blocking: One command to the board, waiting up to control_timeout_ms; it is retried while the
 * board is answering from that rule.
 * Synchronization: Device domain.
 * As mimusb_device_set_rule_data for rule number rule_index of the Node's own
 * mimusb_node_config.rules. A profile Node has none of its own (the library writes its rules).
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node, a rule that is not an ANSWER rule, a
 * null data with a length or more bytes than the rule's capacity; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; MIMUSB_ERR_BUSY when the board kept answering from the rule; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_set_rule_data(mimusb_node* node, size_t rule_index,
                                                               const uint8_t* data, size_t length,
                                                               uint32_t control_timeout_ms);

/* mimusb_hid_descriptor_bytes
 * Ownership: Nothing; writes out.
 * Blocking: None.
 * Synchronization: None.
 * The 9 bytes of a HID descriptor with one subordinate descriptor, the report descriptor of
 * report_descriptor_length bytes (HID 1.11 6.2.1), for mimusb_usb_interface.class_descriptors.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null out, a length of 0 or above 65535;
 * MIMUSB_ERR_UNSET_FIELD for a zero bcd_hid. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_hid_descriptor_bytes(uint16_t bcd_hid,
                                                                 uint8_t country_code,
                                                                 size_t report_descriptor_length,
                                                                 uint8_t out[9]);

/* mimusb_hid_descriptor_rule
 * Ownership: out_rule->data points at `bytes`, which the caller keeps until the rule is given to
 * the library (it copies them then).
 * Blocking: None.
 * Synchronization: None.
 * Fills an ANSWER rule for GET_DESCRIPTOR (USB 2.0 9.4.3; HID 1.11 7.1.1) on one interface, of
 * descriptor_type 0x22 (the report descriptor) or 0x21 (the HID descriptor), any index.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, an empty or longer-than-65535 byte
 * string or another descriptor type. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_hid_descriptor_rule(uint8_t interface_number,
                                                                uint8_t descriptor_type,
                                                                const uint8_t* bytes, size_t length,
                                                                mimusb_control_rule* out_rule);

/* mimusb_device_control_reply
 * Ownership: Copies data.
 * Blocking: Writes one frame, for up to send_timeout_ms without progress; it does not wait for the
 * board.
 * Synchronization: Device domain, and also from the event sink, which is the way to answer in
 * time.
 * Answers the held transfer of token (from MIMUSB_EVENT_CONTROL_SETUP with the HELD flag, or from
 * MIMUSB_EVENT_CONTROL_DATA for the verdict of a host-to-device transfer):
 *  - ANSWER to a device-to-host request: data is the data stage, sent whole whatever wLength says.
 *    More than wLength makes the host report an overrun; data with wLength 0 still starts a data
 *    stage, which is how a device that does so is copied. Empty data is an empty data stage.
 *  - ANSWER to any other request: data must be empty. With a host-to-device data stage the board
 *    takes the data, reports it as MIMUSB_EVENT_CONTROL_DATA and waits for the verdict: ANSWER
 *    (acknowledge), STALL, or the rule's when_late.
 *  - STALL, DROP: data must be empty.
 *  - LEAVE: the board's own handling; as a verdict it acknowledges.
 * A reply the board no longer waits for (too late, or the target gave up) is dropped.
 * Returns: MIMUSB_OK once the frame is written; MIMUSB_ERR_PARAM for an invalid Device, a token
 * of 0 or above 255, an unknown action, or data with an action that takes none;
 * MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_OVERFLOW for data longer than
 * max_control_data_bytes; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_control_reply(mimusb_device* device,
                                                                 uint32_t token,
                                                                 mimusb_control_action action,
                                                                 const uint8_t* data, size_t length,
                                                                 uint32_t send_timeout_ms);

/* The largest interval_ms of mimusb_device_set_loss_reporting, which is the least time between two
 * MIMUSB_EVENT_TRANSFER_LOST events. */
#define MIMUSB_LOSS_MAX_INTERVAL_MS 60000U

/* mimusb_device_set_loss_reporting
 * Ownership: Nothing.
 * Blocking: Waits up to control_timeout_ms for the board's answer.
 * Synchronization: Device domain.
 * enabled 1 makes the board report the reports it drops because its queue is full
 * (MIMUSB_EVENT_TRANSFER_LOST), at most one event per interval_ms (0 to
 * MIMUSB_LOSS_MAX_INTERVAL_MS; 0 reports each time at once). enabled 0, which is how the board
 * starts, needs interval_ms 0 and puts nothing extra on the link. The setting stays until replaced
 * or mimusb_device_close. Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, enabled or
 * interval_ms; MIMUSB_ERR_UNSET_FIELD for a zero timeout; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_loss_reporting(mimusb_device* device,
                                                                      uint32_t enabled,
                                                                      uint32_t interval_ms,
                                                                      uint32_t control_timeout_ms);

/* mimusb_device_add_string
 * Ownership: Copies utf8.
 * Blocking: None; the string is sent with the next commit.
 * Synchronization: Device domain.
 * Stores a string descriptor (in the Device's language_id) and returns its index, for the caller
 * to write into its own class descriptors. Indices 1 to 4 keep their meaning (manufacturer,
 * product, serial number, configuration); an interface's name takes an index from the same pool at
 * each commit, the lowest one nothing else has. The first index given is 5. The string stays
 * until mimusb_device_remove_string.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null argument or text that is not
 * UTF-8 without control characters of at most 126 UTF-16 code units; MIMUSB_ERR_OVERFLOW when the
 * board's max_strings or the index space is used up. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_add_string(mimusb_device* device,
                                                              const char* utf8, uint8_t* out_index);

/* mimusb_device_remove_string
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Forgets a string given by mimusb_device_add_string; the next commit no longer sends it.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or an index that was not given. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_remove_string(mimusb_device* device,
                                                                 uint8_t index);

/* mimusb_device_set_string
 * Ownership: Copies the units.
 * Blocking: None; the string is sent with the next commit.
 * Synchronization: Device domain.
 * Stores a string descriptor exactly as it goes on the bus: index 1 to 255, any language_id (a
 * nonzero LANGID, USB 2.0 9.6.7), and unit_count UTF-16 code units (1 to 126), whatever they are.
 * A string with the same index and language is replaced. These are the strings a Device given as
 * bytes (mimusb_device_set_descriptors) can be read, and the only ones: the strings of
 * usb_device, usb_config and mimusb_device_add_string are not sent then. They are sent
 * with every commit while the Device is given as bytes, and never otherwise.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null units, an index of 0, a
 * language_id of 0 or a unit_count of 0 or above 126; MIMUSB_ERR_OVERFLOW when the board's
 * max_strings is used up. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_string(mimusb_device* device, uint8_t index,
                                                              uint16_t language_id,
                                                              const uint16_t* units,
                                                              size_t unit_count);

/* mimusb_device_clear_string
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Forgets a string given by mimusb_device_set_string; the next commit no longer sends it.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or an index and language_id that were
 * not given. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_clear_string(mimusb_device* device,
                                                                uint8_t index,
                                                                uint16_t language_id);

/* mimusb_device_set_bos
 * Ownership: Copies the bytes.
 * Blocking: None; the BOS descriptor is sent with the next commit.
 * Synchronization: Device domain.
 * For a Device composed from Nodes: stores the Binary Object Store descriptor the target can read
 * (USB 3.2 9.6.2), for the next commit; length 0 removes it. It is checked without reading any
 * capability: a 5-byte header of type 0x0F whose wTotalLength is length and whose bNumDeviceCaps is
 * the number of capability descriptors that follow, each at least 3 bytes, of type 0x10, inside the
 * bytes; at most the board's max_bos_bytes. At the commit the Device's bcd_usb must be 0x0201 or
 * more: a host asks for a BOS descriptor only then (USB 2.0 Link Power Management ECN, USB 3.2
 * 9.6.2). A Device without one stalls the request, as before. A Device given as bytes carries its
 * BOS in mimusb_device_descriptors.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null bytes with a length, or a BOS
 * descriptor that does not hold together (mimusb_error_detail.offset is the offset of what is
 * wrong); MIMUSB_ERR_OVERFLOW for more than max_bos_bytes; MIMUSB_ERR_BUSY while the Device is
 * given as bytes. The commit refuses a bcd_usb below 0x0201 with MIMUSB_ERR_PARAM. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_set_bos(mimusb_device* device,
                                                           const uint8_t* bytes, size_t length);

/* ---- A Device given as bytes -------------------------------------------------------------
 * To present a copy of a real device, the descriptors can be given as the bytes the target reads
 * instead of being composed from Nodes. The two are exclusive on a Device: while descriptors are
 * set, mimusb_node_open and mimusb_node_open_profile return MIMUSB_ERR_BUSY, and
 * mimusb_device_set_descriptors with a Node open does. Its endpoints are then named by their
 * address on the Device (mimusb_device_endpoint_*). The OUT data of its endpoints arrives as
 * MIMUSB_EVENT_ENDPOINT_DATA with node_id 0 and the interface of the endpoint in
 * interface_number; endpoint 0 is the control rules of mimusb_device_set_control_rules, whose
 * events are those of a Device, which is how a class request of such a device is answered or
 * passed on.
 *
 * Such a Device may have alternate settings, and with them isochronous endpoints (a board whose
 * mimusb_board_limits.transfer_types has MIMUSB_TRANSFER_ISOCHRONOUS: audio and video classes
 * stream on them). The library and the board do the least that is possible with them:
 *  - SET_INTERFACE of the target switches the isochronous endpoints of the interface (those of the
 *    setting left stop, what waited for them is dropped; those of the setting selected start, at
 *    the packet size their descriptor gives) and arrives as MIMUSB_EVENT_INTERFACE_ALTERNATE. A
 *    bulk or interrupt endpoint is open from the configuration on, whichever setting names it.
 *    GET_INTERFACE answers the setting selected (0 at the start).
 *  - An isochronous IN transfer is one packet, at most the endpoint's packet size
 *    (mimusb_device_endpoint_write refuses a longer one with MIMUSB_ERR_PARAM), sent in the frame
 *    of the bus that next asks for it. The board makes no packet, fills no gap and keeps no
 *    rhythm: when nothing waits, that frame of the bus passes with no packet (USB 2.0 5.6), and
 *    when the target reads slower than the program writes, the policy of the endpoint decides
 *    what is dropped. When to write, how much, and what a missing frame means to the target is
 *    the program's, which is why the policy and its depth are the program's to choose.
 *  - An isochronous OUT packet is an MIMUSB_EVENT_ENDPOINT_DATA event like any other.
 * The class's own requests (audio sampling frequency, video probe and commit) are endpoint-0
 * control rules, as for any class. */

/* mimusb_device_set_descriptors
 * Ownership: Copies the bytes.
 * Blocking: Commits as a Node open does: sends the descriptors, the strings of
 * mimusb_device_set_string, the Device's control rules and the policy of every IN endpoint, then
 * the board leaves the target's bus and returns to it with the new device, waiting as timeouts
 * says. Inside a batch it only stages them; the commit that ends the batch sends them.
 * Synchronization: Device domain.
 * Nothing is changed in the bytes and nothing is filled in; usb_device, usb_config and their four
 * strings are not used. Checked, none of it reading a class: the device descriptor is 18 bytes of
 * type 1 with one configuration; wTotalLength of the configuration is its length; every bLength
 * is at least 2 and stays inside the bytes; the interface numbers are 0, 1, 2, ... in order and
 * alternate settings are 0, 1, 2, ... in order for each interface, with nothing between them;
 * every endpoint is interrupt or bulk, or isochronous on a board whose transfer_types has it, with
 * an address that is valid, unique within a setting and within the board's limits (it may come
 * again in another setting of the same interface: a bulk or interrupt endpoint then with the same
 * descriptor, an isochronous one with any packet size the board takes) and a packet size
 * mimusb_node_open would accept (an isochronous one: up to max_transfer_bytes, at most 1023 at
 * full speed and 1024 at high speed, with bInterval 1 at full speed); an endpoint descriptor is
 * 7 bytes, or 9 in the audio-class form;
 * the totals fit mimusb_board_limits; a BOS has the header and capability lengths of
 * mimusb_device_set_bos and needs bcdUSB 0x0201 or more. Every IN endpoint needs a policy given
 * with mimusb_device_endpoint_set_policy before this call. The details name the field
 * ("descriptors.device", ".configuration", ".bos") and the offset in the bytes.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null argument, a wrong struct_size,
 * a nonzero reserved or a descriptor that does not hold together; MIMUSB_ERR_UNSET_FIELD for a
 * zero timeout, a bcdUSB of 0 or an IN endpoint with no policy (the details name the first);
 * MIMUSB_ERR_UNSUPPORTED for an isochronous endpoint on a board that has none (the offset is that
 * of its bmAttributes); MIMUSB_ERR_OVERFLOW when the board's limits are exceeded, a configuration
 * of isochronous endpoints that needs more packet memory than the controller has included (the
 * board refuses the commit); MIMUSB_ERR_BUSY while a Node is open; the results of
 * mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_device_set_descriptors(mimusb_device* device, const mimusb_device_descriptors* descriptors,
                              const mimusb_commit_timeouts* timeouts);

/* mimusb_device_clear_descriptors
 * Ownership: Nothing.
 * Blocking: Takes the Device off the target (a commit with nothing to present) as
 * mimusb_node_close does for the last Node.
 * Synchronization: Device domain.
 * Forgets the descriptors, the policies of their endpoints and what the board held for them; Nodes
 * can be opened again. The strings of mimusb_device_set_string stay. Inside a batch it only
 * stages the removal.
 * Returns: MIMUSB_OK (also when no descriptors are set); MIMUSB_ERR_PARAM for an invalid Device or
 * timeouts; MIMUSB_ERR_UNSET_FIELD for a zero timeout; the results of mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_device_clear_descriptors(mimusb_device* device, const mimusb_commit_timeouts* timeouts);

/* mimusb_device_endpoint_set_policy
 * Ownership: Copies policy.
 * Blocking: Before the descriptors are committed: none; the policy is kept for the commit. After:
 * as mimusb_node_endpoint_set_policy (a command to the board, waiting up to control_timeout_ms).
 * Synchronization: Device domain.
 * The policy of an IN endpoint of a Device given as bytes, as mimusb_node_endpoint_set_policy has
 * it for a Node. It must be given for every IN endpoint before mimusb_device_set_descriptors, and
 * may be changed afterwards. A policy for an address the descriptors do not have is kept for the
 * next mimusb_device_set_descriptors. Policies stay until mimusb_device_clear_descriptors.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, a null policy, a bad on_full, an
 * address that is not an IN address (0x81 to 0x8F); MIMUSB_ERR_UNSET_FIELD for on_full or depth 0
 * or a zero timeout; MIMUSB_ERR_OVERFLOW; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_endpoint_set_policy(
    mimusb_device* device, uint8_t address, const mimusb_endpoint_policy* policy,
    uint32_t control_timeout_ms);

/* mimusb_device_endpoint_write
 * Ownership: Copies data.
 * Blocking: As mimusb_node_endpoint_write.
 * Synchronization: Device domain; writes to different endpoints may run at the same time.
 * Sends data as one USB transfer on an IN endpoint of a Device given as bytes, with everything
 * mimusb_node_endpoint_write says of splitting, zero-length packets and the wait policy.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device, an address that is not an IN endpoint
 * of the descriptors (none set included), null data with a nonzero length, or more than the
 * endpoint's packet size (the largest of its settings) to an isochronous endpoint: such a transfer
 * is one packet, and is not split;
 * MIMUSB_ERR_UNSET_FIELD, MIMUSB_ERR_TIMEOUT, MIMUSB_ERR_SHORT_TRANSFER, MIMUSB_ERR_OVERFLOW and a
 * link error as mimusb_node_endpoint_write; MIMUSB_ERR_BUSY as there, and while the descriptors
 * are staged in a batch that has not ended. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_endpoint_write(mimusb_device* device,
                                                                  uint8_t address,
                                                                  const uint8_t* data,
                                                                  size_t length,
                                                                  uint32_t timeout_ms);

/* mimusb_device_endpoint_stall
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * As mimusb_node_endpoint_stall for an endpoint of a Device given as bytes.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or an address that is not an endpoint
 * of the descriptors (none set included); MIMUSB_ERR_UNSET_FIELD for a zero timeout;
 * MIMUSB_ERR_BUSY when the board does not run the configuration; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_endpoint_stall(mimusb_device* device,
                                                                  uint8_t address,
                                                                  uint32_t control_timeout_ms);

/* mimusb_device_endpoint_clear_stall
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * As mimusb_node_endpoint_clear_stall for an endpoint of a Device given as bytes.
 * Returns: MIMUSB_OK; the errors of mimusb_device_endpoint_stall: MIMUSB_ERR_PARAM,
 * MIMUSB_ERR_UNSET_FIELD, MIMUSB_ERR_BUSY or a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_endpoint_clear_stall(
    mimusb_device* device, uint8_t address, uint32_t control_timeout_ms);

/* mimusb_device_endpoint_flush
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * As mimusb_node_endpoint_flush for an IN endpoint of a Device given as bytes.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Device or an address that is not an IN
 * endpoint of the descriptors (none set included); MIMUSB_ERR_UNSET_FIELD for a zero timeout;
 * MIMUSB_ERR_BUSY when the board does not run the configuration; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_device_endpoint_flush(mimusb_device* device,
                                                                  uint8_t address,
                                                                  uint32_t control_timeout_ms);

/* ---- Spec -------------------------------------------------------------------------------
 * The factories copy their options and every array and string the options point to. The Spec
 * holds one reference for the caller. All factories return:
 *   MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, a wrong struct_size or a value out of range;
 *   MIMUSB_ERR_UNSET_FIELD for a required field left zero; MIMUSB_ERR_OVERFLOW when the descriptor
 *   or report would be too large; the factory's own results listed with it. */

/* mimusb_spec_create_keyboard
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW; MIMUSB_ERR_PARAM also for a
 * range that includes 0x00 to 0x03 or the modifiers. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_spec_create_keyboard(const mimusb_keyboard_options* options, mimusb_spec** out_spec);

/* mimusb_spec_create_mouse
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_create_mouse(const mimusb_mouse_options* options,
                                                              mimusb_spec** out_spec);

/* mimusb_spec_create_toggle
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW; MIMUSB_ERR_UNSUPPORTED for a
 * semantic that press and release cannot express, or a Camera Control Usage that is not
 * MIMUSB_USAGE_ONE_SHOT. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_create_toggle(const mimusb_toggle_options* options,
                                                               mimusb_spec** out_spec);

/* mimusb_spec_create_gamepad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_spec_create_gamepad(const mimusb_gamepad_options* options, mimusb_spec** out_spec);

/* mimusb_spec_create_touchscreen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_spec_create_touchscreen(const mimusb_touchscreen_options* options, mimusb_spec** out_spec);

/* mimusb_spec_create_touchpad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_spec_create_touchpad(const mimusb_touchpad_options* options, mimusb_spec** out_spec);

/* mimusb_spec_create_pen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_create_pen(const mimusb_pen_options* options,
                                                            mimusb_spec** out_spec);

/* mimusb_spec_create_digitizer
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_spec_create_digitizer(const mimusb_digitizer_options* options, mimusb_spec** out_spec);

/* mimusb_spec_create_raw
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMUSB_OK through MIMUSB_ERR_OVERFLOW; MIMUSB_ERR_PARAM also for a
 * descriptor that does not parse or does not match the report table; MIMUSB_ERR_UNSUPPORTED for
 * Output or Feature items without the matching requires_* flag, a long item, or a construct the
 * parser does not handle. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_create_raw(const mimusb_raw_options* options,
                                                            mimusb_spec** out_spec);

/* mimusb_spec_retain
 * Ownership: Adds a reference for the caller, who must already hold one.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: Nothing. MIMUSB_OK is recorded; MIMUSB_ERR_PARAM for null, or MIMUSB_ERR_OVERFLOW when
 * the count is full (no reference is added). */
MIMUSB_API void MIMUSB_CALL mimusb_spec_retain(mimusb_spec* spec);

/* mimusb_spec_release
 * Ownership: Gives up one of the caller's references; the last one frees the Spec. A Node keeps
 * its own reference until it closes.
 * Blocking: None.
 * Synchronization: Any thread, as long as the last release does not race any other use.
 * Returns: Nothing. MIMUSB_OK is recorded, or MIMUSB_ERR_PARAM for null (nothing is done). */
MIMUSB_API void MIMUSB_CALL mimusb_spec_release(mimusb_spec* spec);

/* mimusb_spec_descriptor
 * Ownership: The bytes belong to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_descriptor(const mimusb_spec* spec,
                                                            const uint8_t** bytes, size_t* length);

/* mimusb_spec_manifest
 * Ownership: Fills the caller's manifest; manifest->reports belongs to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_spec_manifest(const mimusb_spec* spec,
                                                          mimusb_capability_manifest* manifest);

/* ---- Node ------------------------------------------------------------------------------- */

/* mimusb_node_open
 * Ownership: Copies config and everything it points to.
 * Blocking: As mimusb_device_commit: the whole configuration of the Device is sent and the board
 * enumerates again. Inside a batch the Node is only staged and committed with the batch.
 * Synchronization: Device domain.
 * Opens a Node of any class made of the interfaces, endpoints, class descriptors, optional
 * association and control rules of config. Nothing is read from class_code except that 0 is
 * unset. What the class does on endpoint 0 (the HID report descriptor, GET_REPORT, SET_IDLE, ...)
 * is in config.rules: a request that no rule of any Node or of the Device matches is stalled.
 * Checked without knowing the class: interface numbers (0, 1, 2, ... over all Nodes, no gap);
 * every endpoint address valid (USB 2.0 9.6.6), unique across the device, with a number the board's
 * controller has; transfer_type one the board has; max_packet_size within the board's
 * max_endpoint_bytes and, for bulk, one USB allows (8, 16, 32 or 64 at full speed, 512 at high
 * speed; USB 2.0 5.8.3); interval not 0 for an interrupt endpoint; a policy on every IN endpoint
 * and none on an OUT endpoint; the bLength of each class descriptor; the association; and the
 * totals against mimusb_board_limits. The details name the field and the offending number.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null argument, a wrong struct_size, a bad value, a
 * gap in the interface numbers (mimusb_error_detail.offset is the missing number), a repeated
 * endpoint address or interface number, an association that does not match the interfaces;
 * MIMUSB_ERR_UNSET_FIELD for a required field left zero (class_code, transfer_type,
 * max_packet_size, interval of an interrupt endpoint, a policy of an IN endpoint, the action of a
 * rule) or a zero timeout; MIMUSB_ERR_UNSUPPORTED for a transfer type the board does not have;
 * MIMUSB_ERR_OVERFLOW when a size, count or limit of the board is exceeded; the results of
 * mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_open(mimusb_device* device,
                                                      const mimusb_node_config* config,
                                                      const mimusb_commit_timeouts* timeouts,
                                                      mimusb_node** out_node);

/* mimusb_node_open_profile
 * Ownership: Copies config, retains spec.
 * Blocking: As mimusb_node_open.
 * Synchronization: Device domain.
 * Opens a HID Node whose report descriptor is the Spec's: interface class 0x03, the HID
 * descriptor, one interrupt IN endpoint and at most one interrupt OUT endpoint. The library writes
 * the control rules that make it behave as HID 1.11 asks (GET_DESCRIPTOR of the HID and report
 * descriptors, GET_REPORT from the last report sent and from mimusb_node_set_feature_response,
 * SET_REPORT, GET/SET_IDLE and GET/SET_PROTOCOL) and the events that follow from them
 * (MIMUSB_EVENT_OUTPUT_REPORT, _FEATURE_REPORT, _SET_IDLE, _SET_PROTOCOL). The profile calls
 * (mimusb_keyboard_key, ...) and mimusb_node_submit work on it. The rules for Nodes sharing an
 * interface (all with Report IDs, all different, the same interface and endpoint settings) are of
 * this call. Both endpoints are interrupt endpoints.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null argument, a wrong struct_size, a bad value,
 * Nodes sharing an interface that do not follow the rules above, or interface numbers that leave
 * a gap (mimusb_error_detail.offset is the missing number; nothing is sent);
 * MIMUSB_ERR_UNSET_FIELD for a required field or timeout left zero; MIMUSB_ERR_UNSUPPORTED for a
 * transfer type other than interrupt;
 * MIMUSB_ERR_OVERFLOW when the board has no room for the Node, its interface or its descriptor, or
 * the Spec exceeds maximum_report_bytes or a linux_hid limit; the results of
 * mimusb_device_commit. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_open_profile(
    mimusb_device* device, mimusb_spec* spec, const mimusb_hid_node_config* config,
    const mimusb_commit_timeouts* timeouts, mimusb_node** out_node);

/* mimusb_node_close
 * Ownership: Follows the "Closing" rule.
 * Blocking: Finishes a touch frame that has started and sends the neutral report (keys and
 * buttons up, contacts lifted, axes at rest) within drain_timeout_ms, then commits the remaining
 * Nodes as mimusb_node_open does; closing the last Node takes the USB device off the target.
 * Inside a batch it only stages the removal. A failed step does not stop the next one; after a
 * failed commit the target may still see the Node until a commit succeeds (mimusb_device_commit).
 * Closing a Node whose interface number is not the highest in use would leave a gap in the numbers:
 * the Node is gone, the commit fails with MIMUSB_ERR_PARAM, and a later commit succeeds once the
 * gap is closed (close the highest number first, or in a batch).
 * Synchronization: Device domain; not from the event sink.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null or closed Node, a closing Device, or a wrong
 * timeouts struct_size; MIMUSB_ERR_UNSET_FIELD for a zero timeout; otherwise the first error on
 * the way out. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_close(mimusb_node* node,
                                                       const mimusb_commit_timeouts* timeouts,
                                                       uint32_t drain_timeout_ms);

/* mimusb_node_id
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Returns: The number the board knows the Node by, plus one (so it is nonzero), with MIMUSB_OK
 * recorded; or 0 with MIMUSB_ERR_PARAM recorded for a null or closed Node. */
MIMUSB_API uint16_t MIMUSB_CALL mimusb_node_id(const mimusb_node* node);

/* mimusb_node_set_feature_response
 * Ownership: Copies data.
 * Blocking: Waits up to control_timeout_ms for the board's answer; inside a batch, or for a Node
 * not yet committed, it is sent with the commit instead.
 * Synchronization: Device domain; profile Nodes only.
 * Sets what the board answers (the bytes after the Report ID, at most the board's
 * max_feature_bytes; mimusb_device_limits) when the target reads Feature report report_id of this
 * Node: an ANSWER rule the library writes, whose bytes it changes in place when the length stays
 * and otherwise sends the table again. A touch Spec with
 * enable_contact_count_maximum_feature_declaration has its answer set already.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node, null data with a nonzero length, or
 * more than max_feature_bytes, or a Node opened with mimusb_node_open;
 * MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_OVERFLOW when the board has no room for
 * the rule or its bytes; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_set_feature_response(mimusb_node* node,
                                                                      uint8_t report_id,
                                                                      const uint8_t* data,
                                                                      size_t length,
                                                                      uint32_t control_timeout_ms);

/* mimusb_node_endpoint_set_policy
 * Ownership: Copies policy.
 * Blocking: A command to the board, waiting up to control_timeout_ms for its answer; none inside a
 * batch or for a Node not yet committed: the policy is then sent with the commit.
 * Synchronization: Device domain.
 * Changes what an IN endpoint of the Node does when its queue is full, and how many transfers may
 * wait. The board cut the queue at the last commit: a change takes effect at once only if the
 * queue holds it (a queue of depth d needs d + 1 slots, or 2 * d + 1 for MIMUSB_FULL_DROP_OLDEST,
 * of the slots it was given), otherwise the board refuses it (MIMUSB_ERR_OVERFLOW) and nothing
 * changes; a commit cuts the queues anew. Nodes that share an interface share the least depth any
 * of them set. Changing a policy resets the wait
 * count of the endpoint (see mimusb_node_endpoint_write).
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node, a null policy, an address that is not
 * an IN endpoint of the Node or a bad on_full; MIMUSB_ERR_UNSET_FIELD for on_full or depth 0 or a
 * zero timeout; MIMUSB_ERR_OVERFLOW; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_node_endpoint_set_policy(mimusb_node* node, uint8_t address,
                                const mimusb_endpoint_policy* policy, uint32_t control_timeout_ms);

/* mimusb_node_endpoint_write
 * Ownership: Copies data.
 * Blocking: With MIMUSB_FULL_WAIT, until the board has room for the transfer or timeout_ms has
 * passed; no lock of the Device is held while it waits, so the event sink and other calls run.
 * With the drop policies only until the serial driver has the frames (timeout_ms then bounds a
 * write that makes no progress, and 0 is accepted).
 * Synchronization: Device domain; writes to different endpoints may run at the same time.
 * Sends data as one USB transfer on an IN endpoint of a Node (a profile Node's own sends go through
 * the same path, so the wait policy applies to them too). A length
 * of 0 sends a zero-length packet (USB 2.0 5.8.3). A length above the board's max_transfer_bytes is
 * sent as several frames, each a multiple of the endpoint's max_packet_size except the last, so
 * the target sees the same transfer; the library never adds a zero-length packet: a caller whose
 * class ends a transfer of an exact multiple of the packet size with one sends it with a second
 * call of length 0. Writes to one endpoint are in order.
 * The wait policy: the library counts, per endpoint, the frames it has written minus those the
 * board reported finished, and a write blocks while that number equals the policy's depth. A
 * flush, a stall or clear of the endpoint, a change of its policy, an unmount (the target left)
 * and a commit set the count to zero and wake writers: a waiting write returns MIMUSB_ERR_BUSY
 * (MIMUSB_ERR_SHORT_TRANSFER when frames of a split write had gone). The count never goes below
 * zero. timeout_ms is required with the wait policy (MIMUSB_ERR_UNSET_FIELD when 0).
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node, an address that is not an IN endpoint
 * of the Node, or null data with a nonzero length; MIMUSB_ERR_UNSET_FIELD as above;
 * MIMUSB_ERR_TIMEOUT when timeout_ms passed with nothing of the write sent;
 * MIMUSB_ERR_SHORT_TRANSFER when it passed after some frames of a split write were sent;
 * MIMUSB_ERR_BUSY as above, also for a Node staged in a batch that has not ended or while the board
 * enumerates; MIMUSB_ERR_OVERFLOW for a transfer of more than 65535 frames; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_endpoint_write(mimusb_node* node, uint8_t address,
                                                                const uint8_t* data, size_t length,
                                                                uint32_t timeout_ms);

/* mimusb_node_endpoint_stall
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * Sets the Halt feature of an endpoint of a Node (USB 2.0 9.4.5): the
 * endpoint answers STALL until it is cleared by mimusb_node_endpoint_clear_stall or by the target.
 * On an IN endpoint what waits on the board and the transfer being sent are dropped. This resets
 * the wait count.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node or an address that is not an endpoint
 * of the Node; MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_BUSY when the board does not
 * run the configuration (the Node is staged, or the target has not configured the device); a link
 * error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_endpoint_stall(mimusb_node* node, uint8_t address,
                                                                uint32_t control_timeout_ms);

/* mimusb_node_endpoint_clear_stall
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * As mimusb_node_endpoint_stall, clearing the Halt feature: an IN endpoint goes on with what
 * waits, an OUT endpoint accepts packets again. This resets the wait count.
 * Returns: MIMUSB_OK; the errors of mimusb_node_endpoint_stall: MIMUSB_ERR_PARAM,
 * MIMUSB_ERR_UNSET_FIELD, MIMUSB_ERR_BUSY or a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_endpoint_clear_stall(mimusb_node* node,
                                                                      uint8_t address,
                                                                      uint32_t control_timeout_ms);

/* mimusb_node_endpoint_flush
 * Ownership: Nothing.
 * Blocking: A command to the board, waiting up to control_timeout_ms.
 * Synchronization: Device domain.
 * Drops what waits on an IN endpoint of the board; a transfer the board's controller already has
 * completes. This resets the wait count.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for an invalid Node, an address that is not an IN endpoint
 * of the Node (an OUT endpoint too); MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_BUSY
 * when the board does not run the configuration; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_endpoint_flush(mimusb_node* node, uint8_t address,
                                                                uint32_t control_timeout_ms);

/* mimusb_node_manifest
 * Ownership: As mimusb_spec_manifest; manifest->reports is valid while the Node is open.
 * Blocking: None.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_manifest(const mimusb_node* node,
                                                          mimusb_capability_manifest* manifest);

/* mimusb_node_submit
 * Ownership: The report is built in the library's own buffer.
 * Blocking: Writes one report of the Node's current state, if it changed, to the serial port and
 * returns once the driver has it (microseconds on an idle port). It never waits for the target;
 * send_timeout_ms only bounds a write that makes no progress.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK, also when there was nothing to send; MIMUSB_ERR_PARAM for an invalid Node;
 * MIMUSB_ERR_UNSET_FIELD for a zero timeout; MIMUSB_ERR_UNSUPPORTED for a Raw Node (use
 * mimusb_raw_submit); MIMUSB_ERR_BUSY for a Node opened in a batch that has not ended, while the
 * board enumerates, or while a report of the Node is being sent; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_submit(mimusb_node* node,
                                                        uint32_t send_timeout_ms);

/* mimusb_node_submit_blocking
 * Ownership: As mimusb_node_submit.
 * Blocking: Writes reports until the Node's state has been sent, a touch frame of several reports
 * and a mouse's pending movement included, or timeout_ms has passed; each write is bounded by what
 * is left of it.
 * Synchronization: Device domain.
 * Returns: MIMUSB_OK; MIMUSB_ERR_TIMEOUT when timeout_ms passed first; the results of
 * mimusb_node_submit. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_node_submit_blocking(mimusb_node* node,
                                                                 uint32_t timeout_ms);

/* ---- Input ------------------------------------------------------------------------------
 * These calls change a Node's state; mimusb_node_submit sends it. None of them blocks, allocates
 * or does I/O. Each belongs to the Device domain and works only on a Node of its own profile.
 * A press and the release after it are never merged: until the report carrying the press has been
 * sent, the release is refused with MIMUSB_ERR_BUSY, so a quick tap is not lost. Every input call
 * returns:
 *   MIMUSB_OK; MIMUSB_ERR_PARAM for a Node of another profile or a value out of range;
 *   MIMUSB_ERR_BUSY while a report of the Node is being sent, an opposite edge is unsent, the
 *   Node was opened in a batch that has not ended, or the board enumerates;
 *   MIMUSB_ERR_NO_DEVICE once the link is gone; the call's own results listed with it. */

/* mimusb_keyboard_key
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) one Keyboard page Usage, a modifier or a key of the Spec's
 * range. Closing the Node releases every key still down.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_keyboard_key(mimusb_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimusb_mouse_move
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Adds to the pending X and Y movement. A report carries as much as the fields hold and keeps the
 * rest for the next one.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE; MIMUSB_ERR_OVERFLOW when
 * the 64-bit pending total overflows. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_mouse_move(mimusb_node* node, int32_t dx, int32_t dy);

/* mimusb_mouse_scroll
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Adds to the pending wheel and pan movement, as mimusb_mouse_move does.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE; MIMUSB_ERR_UNSUPPORTED
 * for a nonzero value on a field the Spec lacks; MIMUSB_ERR_OVERFLOW when the 64-bit pending total
 * overflows. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_mouse_scroll(mimusb_node* node, int32_t wheel,
                                                         int32_t pan);

/* mimusb_mouse_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_mouse_button(mimusb_node* node, uint32_t button,
                                                         uint32_t down);

/* mimusb_toggle_usage
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) one Usage of the Spec's allow-list; any number may be
 * pressed at once. Closing the Node releases every Usage still down.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_toggle_usage(mimusb_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimusb_gamepad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_gamepad_button(mimusb_node* node, uint32_t button,
                                                           uint32_t down);

/* mimusb_gamepad_axis
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets axes[axis_index] of the Spec to value, which must be in its logical range.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_gamepad_axis(mimusb_node* node, size_t axis_index,
                                                         int32_t value);

/* mimusb_gamepad_dpad
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the D-pad from four 0/1 directions. With a Hat, up and down (or left and right) together
 * are refused and no direction sends the Null state; with four buttons the bits go out as given.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE; MIMUSB_ERR_PARAM also
 * when the Spec has no D-pad. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_gamepad_dpad(mimusb_node* node, uint32_t up,
                                                         uint32_t down, uint32_t right,
                                                         uint32_t left);

/* mimusb_touchscreen_contact
 * Ownership: Copies extra, which may be null (every optional field 0).
 * Blocking: None.
 * Synchronization: Device domain.
 * down 1 places contact_id in a free slot, or moves it if it is already down; down 0 lifts it at
 * x, y. A lifted contact_id can be placed again once its lift has been sent. While a frame split
 * over several reports is being sent, every call is refused with MIMUSB_ERR_BUSY.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE; MIMUSB_ERR_PARAM also for
 * lifting a contact_id that is not down; MIMUSB_ERR_BUSY also for placing one whose lift is
 * unsent; MIMUSB_ERR_OVERFLOW when every slot is taken. */
MIMUSB_API mimusb_result MIMUSB_CALL
mimusb_touchscreen_contact(mimusb_node* node, uint32_t contact_id, uint32_t down, int32_t x,
                           int32_t y, const mimusb_touchscreen_extra* extra);

/* mimusb_touchpad_contact
 * Ownership: Copies extra, which may be null.
 * Blocking: None.
 * Synchronization: Device domain.
 * As mimusb_touchscreen_contact, for a Touchpad Node.
 * Returns: As mimusb_touchscreen_contact, MIMUSB_OK through MIMUSB_ERR_OVERFLOW. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_touchpad_contact(mimusb_node* node, uint32_t contact_id,
                                                             uint32_t down, int32_t x, int32_t y,
                                                             const mimusb_touchpad_extra* extra);

/* mimusb_touchpad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) click button 1 to button_count.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_touchpad_button(mimusb_node* node, uint32_t button,
                                                            uint32_t down);

/* mimusb_pen_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the whole pen state. in_range 0 takes the pen away: In Range, Tip, pressure, Invert and the
 * barrel bits go out as 0 with the sample's own X, Y, tilt and twist, so repeat the last position
 * to leave the pointer where it was. Switching between tip and eraser while in range first sends
 * an out-of-range report.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_pen_update(mimusb_node* node,
                                                       const mimusb_pen_sample* sample);

/* mimusb_digitizer_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Device domain.
 * As mimusb_pen_update, for a Digitizer Node.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_digitizer_update(mimusb_node* node,
                                                             const mimusb_digitizer_sample* sample);

/* mimusb_battery_update
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the battery level of a Node whose Spec has enable_battery. It goes out with the next
 * mimusb_node_submit that has no input report to send, and only when it changed. A level not yet
 * sent is dropped on close.
 * Returns: As every input call, MIMUSB_OK through MIMUSB_ERR_NO_DEVICE; MIMUSB_ERR_PARAM also for
 * a Node without a battery report. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_battery_update(mimusb_node* node, int32_t strength);

/* ---- Reports of the caller's own -------------------------------------------------------- */

/* mimusb_raw_submit
 * Ownership: Copies report.
 * Blocking: As mimusb_node_submit.
 * Synchronization: Device domain.
 * Sends one report of a Raw Spec's Node after checking it against the Spec's report table.
 * Returns: MIMUSB_OK; MIMUSB_ERR_PARAM for a Node that is not Raw, a null report, a Report ID or
 * length the Spec does not accept, or nonzero padding bits; MIMUSB_ERR_UNSET_FIELD for a zero
 * timeout; MIMUSB_ERR_BUSY as for mimusb_node_submit; MIMUSB_ERR_OVERFLOW for a report longer than
 * maximum_report_bytes; a link error. */
MIMUSB_API mimusb_result MIMUSB_CALL mimusb_raw_submit(mimusb_node* node, const uint8_t* report,
                                                       size_t length, uint32_t send_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
