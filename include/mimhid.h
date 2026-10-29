// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_H
#define MIMHID_H

/*
 * libmimhid: a microcontroller board, driven over a serial link, presents USB HID devices to the
 * computer its USB port is plugged into (the "target"). This header is the whole stable C ABI. It
 * exposes no serial handle and no C++ type.
 *
 * Objects
 *   Device    one board, opened on a serial port. It presents one USB device to the target, whose
 *             identity (descriptor, strings) the Device sets.
 *   Spec      an immutable HID report descriptor and report layout, built by a factory
 *             (mimhid_spec_create_*). It needs no board and is reference counted.
 *   Node      a Spec opened on a Device: one HID collection on one of the board's USB interfaces,
 *             with the state its input calls change (mimhid_keyboard_key, ...). A Node sends a
 *             report when it is submitted. mimhid_node_open_raw opens a descriptor of the
 *             caller's own without a Spec.
 *   Discovery a snapshot of the boards found on the serial ports.
 *
 * Opening or closing a Node changes the USB device the board presents, so the board drops it from
 * the target and enumerates it again (a "commit"); mimhid_device_batch_begin and _end gather
 * several changes into one.
 *
 * Rules every function follows
 *   Results    Every call records the details of its result for the calling thread, read with
 *              mimhid_last_error(). Results that come from the serial link (the "link errors") are
 *              MIMHID_ERR_TIMEOUT, _NO_DEVICE, _SHORT_TRANSFER and _IO; error_detail says the frame
 *              and holds the errno or Win32 error, or the board's refusal. Once the port fails or
 *              is unplugged, calls on the Device return MIMHID_ERR_NO_DEVICE until it is closed.
 *              Any call can also return MIMHID_ERR_INTERNAL (an allocation failed or an internal
 *              invariant broke). The Returns lines below list what is particular to each function.
 *   Outputs    A function that creates an object returns it through an out_* pointer, which is
 *              set to null first and stays null when the call fails.
 *   Closing    The *_close, *_destroy and *_release functions follow one rule:
 *                MIMHID_ERR_PARAM or MIMHID_ERR_UNSET_FIELD  nothing was done; the handle is still
 *                                                           the caller's.
 *                any other result                           the object is gone and its handle must
 *                                                           not be used again. An error says what
 *                                                           failed on the way out.
 *              Writes to the serial port finish within the call, so nothing is ever left pending.
 *   Threads    A Device and its Nodes form one domain: the caller makes sure no two calls in a
 *              domain run at the same time, unless a function says otherwise. Specs and Discovery
 *              snapshots are outside any domain. The event and log sinks run on the library's
 *              reader thread or on the calling thread; they must not call back into the library,
 *              except mimhid_device_control_reply from the event sink.
 *   Timeouts   The library keeps no timeout: every call that can wait takes its timeouts in
 *              milliseconds as arguments (mimhid_commit_timeouts for a commit). A timeout of zero
 *              is refused with MIMHID_ERR_UNSET_FIELD before anything is sent.
 *              MIMHID_CONTROL_TIMEOUT_MS and the constants next to it are values that work.
 *   Structures Each options structure starts with struct_size (its sizeof) and reserved (zero).
 *              Its *_init function sets both and fills in working values. A required field that
 *              is zero is refused, and every enumeration starts at 1, so a field left zero never
 *              selects something by accident. A flag (enable_*, down, ...) is exactly 0 or 1.
 *
 * Specifications cited: USB 2.0, HID 1.11 and HID Usage Tables 1.7 (HUT), all from usb.org.
 * docs/API.md is the guide to this header; docs/PROTOCOL.md describes the serial link.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#if defined(MIMHID_BUILDING_LIBRARY)
#define MIMHID_API __declspec(dllexport)
#elif defined(MIMHID_SHARED)
#define MIMHID_API __declspec(dllimport)
#else
#define MIMHID_API
#endif
#define MIMHID_CALL __cdecl
#else
#define MIMHID_API __attribute__((visibility("default")))
#define MIMHID_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MIMHID_VERSION_MAJOR 2
#define MIMHID_VERSION_MINOR 0
#define MIMHID_VERSION_PATCH 1
/* The largest report the library builds, whatever a board holds. */
#define MIMHID_HOST_MAX_REPORT_BYTES 1024
/* The serial protocol this library and its firmware speak (mimhid_device_info.protocol_version). */
#define MIMHID_PROTOCOL_VERSION 6

typedef struct mimhid_discovery mimhid_discovery;
typedef struct mimhid_device mimhid_device;
typedef struct mimhid_spec mimhid_spec;
typedef struct mimhid_node mimhid_node;

/* ---- Constants --------------------------------------------------------------------------
 * Every enumeration is an int32_t typedef with anonymous constants, so structure layout does not
 * depend on how a compiler sizes an enum. */

/* A result libaoahid also has keeps the same number there. 5, 9 and 15 are not used. */
typedef int32_t mimhid_result;
enum {
    MIMHID_OK = 0,
    MIMHID_ERR_PARAM = 1,       /* a null pointer, a bad value or a handle in the wrong state */
    MIMHID_ERR_UNSET_FIELD = 2, /* a required field or timeout is zero */
    MIMHID_ERR_UNSUPPORTED = 3, /* not possible with these arguments or this board */
    MIMHID_ERR_PROTOCOL = 4,    /* the board's firmware speaks another protocol version */
    MIMHID_ERR_ACCESS = 6,      /* no permission to open the serial port */
    MIMHID_ERR_BUSY = 7,        /* in use: the port, a batch, an unsent edge, ... */
    MIMHID_ERR_NO_DEVICE = 8,   /* the port failed or was unplugged; this stays until close */
    MIMHID_ERR_TIMEOUT = 10,
    MIMHID_ERR_SHORT_TRANSFER = 11,      /* a write moved fewer bytes than asked */
    MIMHID_ERR_DESCRIPTOR_REJECTED = 12, /* the board refused the configuration */
    MIMHID_ERR_IO = 13,
    MIMHID_ERR_OVERFLOW = 14, /* a size, count or limit was exceeded */
    MIMHID_ERR_INTERNAL = 16
};

typedef int32_t mimhid_log_level;
enum { MIMHID_LOG_DISABLED = 1, MIMHID_LOG_ERROR = 2, MIMHID_LOG_INFO = 3, MIMHID_LOG_TRACE = 4 };

/* The Application Collection of each profile is in docs/PROFILES.md. */
typedef int32_t mimhid_profile_kind;
enum {
    MIMHID_PROFILE_KEYBOARD = 1,
    MIMHID_PROFILE_MOUSE = 2,
    MIMHID_PROFILE_TOUCHSCREEN = 3,
    MIMHID_PROFILE_TOUCHPAD = 4,
    MIMHID_PROFILE_PEN = 5,
    MIMHID_PROFILE_DIGITIZER = 6,
    MIMHID_PROFILE_GAMEPAD = 7,
    MIMHID_PROFILE_TOGGLE = 8, /* one-bit controls on a page of the caller's choice */
    MIMHID_PROFILE_RAW = 9
};

typedef int32_t mimhid_axis_role;
enum {
    MIMHID_AXIS_X = 1,
    MIMHID_AXIS_Y = 2,
    MIMHID_AXIS_Z = 3,
    MIMHID_AXIS_RX = 4,
    MIMHID_AXIS_RY = 5,
    MIMHID_AXIS_RZ = 6,
    MIMHID_AXIS_SLIDER = 7,
    MIMHID_AXIS_SIMULATION_ACCELERATOR = 8,
    MIMHID_AXIS_SIMULATION_BRAKE = 9,
    MIMHID_AXIS_SIMULATION_STEERING = 10,
    MIMHID_AXIS_DIAL = 11,
    MIMHID_AXIS_WHEEL = 12,
    MIMHID_AXIS_SIMULATION_RUDDER = 13,
    MIMHID_AXIS_SIMULATION_THROTTLE = 14
};

typedef int32_t mimhid_dpad_representation;
enum {
    MIMHID_DPAD_NONE = 1,
    MIMHID_DPAD_HAT = 2,    /* one eight-way Hat Switch */
    MIMHID_DPAD_BUTTONS = 3 /* the four Generic Desktop D-pad Usages, one bit each */
};

/* The HUT control type of one Toggle Usage; it decides the descriptor encoding. Only the first six
 * can be pressed and released: a factory refuses the others with MIMHID_ERR_UNSUPPORTED. */
typedef int32_t mimhid_usage_semantic;
enum {
    MIMHID_USAGE_SELECTOR_BITMAP = 1,
    MIMHID_USAGE_ON_OFF_TOGGLE = 2,
    MIMHID_USAGE_ON_OFF_MAINTAINED = 3,
    MIMHID_USAGE_MOMENTARY = 4,
    MIMHID_USAGE_ONE_SHOT = 5,
    MIMHID_USAGE_RETRIGGER = 6,
    MIMHID_USAGE_ON_OFF_PAIR = 7,
    MIMHID_USAGE_LINEAR = 8,
    MIMHID_USAGE_DYNAMIC_VALUE = 9,
    MIMHID_USAGE_NAMED_ARRAY = 10
};

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* Called with one formatted line per event (see "Threads" above). */
typedef void(MIMHID_CALL* mimhid_log_sink)(void* user, mimhid_log_level level, const char* message);

/* The details of the calling thread's last result. Fields that do not apply are zero or null. */
typedef struct mimhid_error_detail {
    mimhid_result code;
    const char* field;  /* the argument, option or step concerned, e.g. "node.close" */
    const char* reason; /* one English sentence */
    /* errno or the Win32 error of a failed serial call; or, when the board refused a frame, its
     * code: 1 bad value, 2 no room, 3 wrong state, 4 descriptor refused, 5 unsupported, 6 busy */
    int32_t transport_status;
    int32_t frame_type; /* the serial frame involved; mimhid_frame_type_name names it */
    uint16_t node_id;   /* the node id of the Node involved (mimhid_node_id) */
    uint16_t report_id;
    uint32_t offset;
    uint32_t length;
} mimhid_error_detail;

/* ---- Events -----------------------------------------------------------------------------
 * What the board reports about the target. Delivered to transport.event_sink (see "Threads"
 * above); data is valid only during the call, so copy what you keep. */

typedef int32_t mimhid_event_kind;
enum {
    MIMHID_EVENT_MOUNTED = 1, /* the target configured the board's USB device */
    MIMHID_EVENT_UNMOUNTED = 2,
    MIMHID_EVENT_SUSPEND = 3, /* value: 1 when the target enabled remote wakeup */
    MIMHID_EVENT_RESUME = 4,
    /* The target sent an Output report (a keyboard's LEDs, for example) or a Feature report. */
    MIMHID_EVENT_OUTPUT_REPORT = 5,
    MIMHID_EVENT_FEATURE_REPORT = 6,
    MIMHID_EVENT_SET_PROTOCOL = 7, /* value: 0 boot protocol, 1 report protocol */
    MIMHID_EVENT_SET_IDLE = 8,     /* value: the idle rate in 4 ms units */
    /* A control transfer of the target on endpoint 0 (see mimhid_device_set_control_routing).
     * data: the 8 setup bytes (bmRequestType, bRequest, wValue, wIndex, wLength; little endian). */
    MIMHID_EVENT_CONTROL_SETUP = 9,
    /* The data stage of a held host-to-device transfer, after the answer accepted it. The board
     * waits for a second answer, the verdict, with the same control_token. */
    MIMHID_EVENT_CONTROL_DATA = 10,
    /* The board's report queue of the Node's interface was full and it dropped the newest reports
     * (a key-up may be among them): value is how many since the last event of this kind, at most
     * 65535. Send the current state again. Only after mimhid_device_set_loss_reporting. */
    MIMHID_EVENT_REPORT_LOST = 11
};

/* mimhid_event.control_flags */
enum {
    MIMHID_CONTROL_FLAG_HELD = 0x01,     /* the board waits for mimhid_device_control_reply */
    MIMHID_CONTROL_FLAG_TOO_LONG = 0x02, /* a data stage longer than the board takes; not held */
    MIMHID_CONTROL_FLAG_FIXED = 0x04,    /* SET_ADDRESS or SET_CONFIGURATION, which the board's own
                                            USB state needs; not held */
    MIMHID_CONTROL_FLAG_LOST = 0x08 /* transfers before this one were not reported: queue full */
};

typedef struct mimhid_event {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_event_kind kind;
    uint16_t node_id; /* the Node the event belongs to, or 0 for the whole device */
    uint16_t report_id;
    uint32_t value;
    const uint8_t* data; /* OUTPUT_REPORT and FEATURE_REPORT: the report without its Report ID */
    size_t length;
    uint32_t control_token; /* CONTROL_SETUP and CONTROL_DATA: 1 to 255 */
    uint32_t control_flags; /* MIMHID_CONTROL_FLAG_* */
} mimhid_event;

typedef void(MIMHID_CALL* mimhid_event_sink)(void* user, const mimhid_event* event);

/* ---- Boards -----------------------------------------------------------------------------*/

/* The serial line rates a board and the host can agree on, slowest first. A board starts at the
 * first, 115200. */
#define MIMHID_BAUD_RATES                                                                          \
    { 115200u, 230400u, 460800u, 921600u, 1000000u, 1500000u, 2000000u, 3000000u, 4000000u }
#define MIMHID_BAUD_COUNT 9

/* mimhid_device_info.chip */
#define MIMHID_CHIP_UNKNOWN 0
#define MIMHID_CHIP_RP2040 1
#define MIMHID_CHIP_RP2350 2
#define MIMHID_CHIP_ESP32S3 3
#define MIMHID_CHIP_NRF52840 4
#define MIMHID_CHIP_STM32F4 5

/* A board found by mimhid_discover, or the one a Device has open. */
typedef struct mimhid_device_info {
    char port_name[64];    /* "/dev/ttyUSB0", "/dev/cu.usbserial-0001", "COM3" */
    char description[64];  /* the serial adapter's USB product string, where the OS gives one */
    uint8_t version_major; /* firmware version */
    uint8_t version_minor;
    uint8_t version_patch;
    uint8_t is_ready;         /* 1 when the target has mounted the board's USB device */
    uint8_t protocol_version; /* the serial protocol the firmware speaks */
    uint8_t max_nodes;
    uint8_t max_interfaces;        /* USB HID interfaces the board provides */
    uint8_t max_report_bytes;      /* at most 255 here; mimhid_device_limits has the exact figure */
    uint16_t max_descriptor_bytes; /* room for all report descriptors together */
    uint8_t has_unique_id;         /* 1 when the chip has a serial number of its own */
    uint8_t reserved8;
    /* discover: the fastest rate that carried pings both ways on this port (the board is left on
     * the first rate); a Device: the rate the link runs at */
    uint32_t baud_rate;
    uint16_t baud_mask;       /* bit i: the board can run at MIMHID_BAUD_RATES[i] */
    uint8_t chip;             /* MIMHID_CHIP_* */
    uint8_t usb_high_speed;   /* 1 when the board's USB device runs at high speed */
    uint8_t unique_id_length; /* bytes of unique_id in use */
    uint8_t unique_id[16];
    uint8_t reserved_end[3];
} mimhid_device_info;

/* What the board holds, in bytes: 64-byte endpoint packets on a full-speed chip, 1024 on a
 * high-speed one. A Feature report may be longer than a packet, since it goes over endpoint 0. */
typedef struct mimhid_board_limits {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t max_report_bytes;       /* largest report, Report ID included */
    uint32_t max_feature_bytes;      /* largest Feature report answer */
    uint32_t max_endpoint_bytes;     /* largest ep_in and ep_out max_packet_size */
    uint32_t max_frame_payload;      /* largest serial frame payload the board takes */
    uint32_t max_control_data_bytes; /* largest data stage the host may read or answer */
    uint32_t max_control_rules;      /* rules mimhid_device_set_control_routing takes */
} mimhid_board_limits;

/* ---- USB identity and Device options ----------------------------------------------------
 * The fields are those of the USB 2.0 standard descriptors (section 9.6) and the HID class
 * descriptor (HID 1.11 section 6.2.1). A string is printable ASCII of at most 126 characters, or
 * null to leave it out. */

/* The Device descriptor and its strings. vendor_id, bcd_usb, ep0_max_packet_size and language_id
 * are required. */
typedef struct mimhid_usb_device {
    uint16_t bcd_usb;     /* e.g. 0x0200 */
    uint8_t device_class; /* 0: each interface says its own class */
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t ep0_max_packet_size; /* 8, 16, 32 or 64 */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint16_t language_id; /* the LANGID of the strings, e.g. 0x0409 (English, United States) */
    const char* manufacturer;
    const char* product;
    const char* serial;
    uint32_t serial_from_chip_id; /* 1 with a null serial: the chip's unique id */
} mimhid_usb_device;

/* The Configuration descriptor. */
typedef struct mimhid_usb_config {
    uint8_t configuration_value; /* nonzero, e.g. 1 */
    uint8_t bm_attributes;       /* bit 7 set; bit 6 self-powered; bit 5 remote wakeup */
    uint8_t bmax_power;          /* 0 to 250, in 2 mA units */
    uint8_t reserved8;
    const char* string;
} mimhid_usb_config;

/* The Linux HID parser limits of the target (docs/LIMITS.md). A Spec that exceeds one is refused by
 * mimhid_node_open with MIMHID_ERR_OVERFLOW. All are required. */
typedef struct mimhid_linux_hid_policy {
    uint32_t descriptor_bytes;   /* largest report descriptor */
    uint32_t fields_per_report;  /* fields in one report */
    uint32_t global_stack_depth; /* Push items in effect at once */
    uint32_t report_size_bits;   /* largest Report Size item */
    uint64_t usages;             /* Usages in one descriptor */
    uint64_t report_data_bits;   /* largest report, in bits */
} mimhid_linux_hid_policy;

/* The serial link and what the library does on it. */
typedef struct mimhid_transport_options {
    uint32_t baud_rate; /* one of MIMHID_BAUD_RATES */
    /* Each wait for an answer while mimhid_device_open looks for the board: a few frames at the
     * boot rate plus the latency timer of a USB serial adapter (16 ms on FTDI chips). */
    uint32_t handshake_timeout_ms;
    /* The largest report this Device sends, Report ID included: at most the board's
     * max_report_bytes and MIMHID_HOST_MAX_REPORT_BYTES. */
    uint32_t maximum_report_bytes;
    mimhid_linux_hid_policy linux_hid;
    uint32_t validate_reports; /* 1 checks every built report against its Spec before sending */
    /* 1 leaves the board's USB device on the target after mimhid_device_close */
    uint32_t keep_attached;
    mimhid_event_sink event_sink; /* may be null */
    void* event_user;
    mimhid_log_level log_level; /* anything but MIMHID_LOG_DISABLED needs log_sink */
    mimhid_log_sink log_sink;
    void* log_user;
} mimhid_transport_options;

typedef struct mimhid_device_config {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_transport_options transport;
    mimhid_usb_device usb_device;
    mimhid_usb_config usb_config;
} mimhid_device_config;

/* The waits of a call that configures the board and enumerates its USB device again (a commit). */
typedef struct mimhid_commit_timeouts {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t control_timeout_ms;   /* each configuration frame waits this long for the board's
                                      answer, and is sent up to three times */
    uint32_t reconnect_timeout_ms; /* the board's new enumeration */
} mimhid_commit_timeouts;

/* ---- Node options ----------------------------------------------------------------------- */

typedef struct mimhid_usb_endpoint {
    uint8_t address;          /* IN: 0x81 to 0x8F; OUT: 0x01 to 0x0F, or 0 for none */
    uint8_t poll_interval_ms; /* bInterval, 1 to 255 */
    uint16_t max_packet_size; /* 1 to the board's max_endpoint_bytes; 0 with no OUT endpoint */
} mimhid_usb_endpoint;

/* The Interface descriptor and the HID descriptor. Nodes with the same number share one interface,
 * and must then all have Report IDs, all different, and the same interface and endpoint settings.
 * The numbers are only labels: the board numbers its interfaces 0, 1, 2, ... in their order. */
typedef struct mimhid_usb_interface {
    uint8_t number;
    uint8_t class_code;   /* nonzero: 0x03 for HID */
    uint8_t subclass;     /* 0 none, 1 boot interface */
    uint8_t protocol;     /* with subclass 1: 1 keyboard, 2 mouse */
    uint16_t bcd_hid;     /* nonzero, e.g. 0x0111 */
    uint8_t country_code; /* 0 when the hardware is not localized, up to 35 */
    uint8_t reserved8;
    const char* name;
} mimhid_usb_interface;

/* Interfaces that are not shared need different endpoint addresses. */
typedef struct mimhid_node_config {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_usb_interface iface;
    mimhid_usb_endpoint ep_in;  /* required */
    mimhid_usb_endpoint ep_out; /* all zero for none */
} mimhid_node_config;

/* ---- Profile options --------------------------------------------------------------------
 * docs/PROFILES.md describes each profile's descriptor and the limits of every field. A value
 * that may be zero (a logical minimum, a neutral value, a count of optional parts) is never
 * taken for "unset": what is optional has its own enable_* flag or count. */

/* The four HID 1.11 physical Global items for one field. They are emitted only when enabled is 1
 * and are reset right after the field, so they never apply to the next one. */
typedef struct mimhid_physical_properties {
    uint32_t enabled;
    int32_t minimum;
    int32_t maximum;
    int32_t unit_exponent;
    uint32_t unit;
} mimhid_physical_properties;

/* One report field. The logical range must fit bit_width (1 to 32 bits); a bit_width of zero is
 * an unset field. */
typedef struct mimhid_integer_field {
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t bit_width;
    mimhid_physical_properties physical;
} mimhid_integer_field;

/* With enabled 1, every report starts with value (1 to 255). */
typedef struct mimhid_report_id_option {
    uint32_t enabled;
    uint8_t value;
    uint8_t reserved8[3];
} mimhid_report_id_option;

/* Battery Strength (Generic Device Controls 0x06/0x20) as a second Input report in the profile's
 * Application Collection. Both this report_id and the profile's must be enabled and differ.
 * Start the range at 1: some Linux releases ignore a value of 0 (docs/PROFILES.md). */
typedef struct mimhid_battery_report {
    mimhid_report_id_option report_id;
    mimhid_integer_field strength;
} mimhid_battery_report;

/* Full N-key rollover: one bit per Usage from usage_minimum (at least 0x04) to usage_maximum, plus
 * the eight modifiers 0xE0 to 0xE7, which the range must not include. */
typedef struct mimhid_keyboard_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint16_t usage_minimum;
    uint16_t usage_maximum;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_keyboard_options;

/* X, Y, wheel and pan are relative; their ranges must hold negative and positive values. */
typedef struct mimhid_mouse_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint32_t button_count; /* 1 to 65535 */
    mimhid_integer_field x;
    mimhid_integer_field y;
    uint32_t enable_wheel;
    mimhid_integer_field wheel;
    uint32_t enable_pan;
    mimhid_integer_field pan;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_mouse_options;

/* One-bit controls on any HUT page: Consumer, System Control, Camera Control, Telephony, ...
 * allowed_usages, usage_semantics, expected_linux_event_types and expected_linux_codes are
 * parallel arrays of allowed_usage_count entries, all required. The expected Linux strings (such
 * as "EV_KEY" and "KEY_PLAYPAUSE") are kept with the Spec as a record; the library does not read
 * them. On the Camera Control page (0x90) only Auto-focus (0x20) and Shutter (0x21) exist, each
 * as MIMHID_USAGE_ONE_SHOT. */
typedef struct mimhid_toggle_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint16_t application_page; /* the Application Collection, e.g. 0x0C/0x01 Consumer Control */
    uint16_t application_usage;
    uint16_t field_page; /* the Usage Page of every allowed Usage */
    uint16_t reserved16;
    const uint16_t* allowed_usages;
    size_t allowed_usage_count;
    const mimhid_usage_semantic* usage_semantics;
    const char* const* expected_linux_event_types;
    const char* const* expected_linux_codes;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_toggle_options;

/* One axis. usage_page and usage must be the ones of role (docs/PROFILES.md); neutral_value is the
 * value sent at rest and on close, and may be any value in the range, zero included. The expected
 * string (such as "ABS_X") is required and kept with the Spec as a record; the library does not
 * read it. */
typedef struct mimhid_gamepad_axis_option {
    mimhid_axis_role role;
    uint16_t usage_page;
    uint16_t usage;
    mimhid_integer_field value;
    int32_t neutral_value;
    const char* expected_linux_code;
} mimhid_gamepad_axis_option;

/* At least two axes, one of them MIMHID_AXIS_X and one MIMHID_AXIS_Y, each role once. A Hat must
 * be logical 0 to 7 in 4 bits; outside MIMHID_DPAD_HAT the three hat_* fields are zero. */
typedef struct mimhid_gamepad_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    const mimhid_gamepad_axis_option* axes;
    size_t axis_count;
    uint32_t button_count;         /* at least 1 */
    uint16_t button_usage_minimum; /* the Button page Usage of the first button, nonzero */
    mimhid_dpad_representation dpad_representation;
    int32_t hat_logical_minimum;
    int32_t hat_logical_maximum;
    uint32_t hat_bit_width;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_gamepad_options;

/* Fixed-slot Multi-Touch under the Digitizers / Touch Screen Application Collection. */
typedef struct mimhid_touchscreen_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint32_t maximum_contacts;          /* 1 to 16 */
    uint32_t contacts_per_report;       /* maximum_contacts, or fewer with multi-packet frames */
    mimhid_integer_field contact_id;    /* holds 0 and at least maximum_contacts values */
    mimhid_integer_field x;             /* holds 0 */
    mimhid_integer_field y;             /* holds 0 */
    mimhid_integer_field contact_count; /* holds 0 to maximum_contacts */
    uint32_t enable_pressure;
    mimhid_integer_field pressure;
    uint32_t enable_width;
    mimhid_integer_field width;
    uint32_t enable_height;
    mimhid_integer_field height;
    uint32_t enable_azimuth;
    mimhid_integer_field azimuth; /* counter-clockwise, minimum 0 (docs/LIMITS.md) */
    uint32_t enable_scan_time;
    mimhid_integer_field scan_time;
    uint32_t scan_time_unit_100us; /* must be 1 with enable_scan_time */
    /* 1 adds Contact Count Maximum as a Feature report, which the board answers itself. */
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_touchscreen_options;

/* The touchscreen report under the Digitizers / Touch Pad Application Collection, plus
 * button_count click buttons (0 for a buttonless clickpad). */
typedef struct mimhid_touchpad_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint32_t maximum_contacts;
    uint32_t contacts_per_report;
    mimhid_integer_field contact_id;
    mimhid_integer_field x;
    mimhid_integer_field y;
    mimhid_integer_field contact_count;
    uint32_t enable_pressure;
    mimhid_integer_field pressure;
    uint32_t enable_width;
    mimhid_integer_field width;
    uint32_t enable_height;
    mimhid_integer_field height;
    uint32_t enable_azimuth;
    mimhid_integer_field azimuth;
    uint32_t enable_scan_time;
    mimhid_integer_field scan_time;
    uint32_t scan_time_unit_100us;
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t button_count;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_touchpad_options;

/* A stylus under the Digitizers / Pen Application Collection (a direct, on-screen pen). */
typedef struct mimhid_pen_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    mimhid_integer_field x;
    mimhid_integer_field y;
    uint32_t enable_pressure;
    mimhid_integer_field pressure;
    uint32_t enable_tilt; /* X Tilt and Y Tilt together */
    mimhid_integer_field tilt_x;
    mimhid_integer_field tilt_y;
    uint32_t enable_twist_target_specific; /* how a target uses Twist varies */
    mimhid_integer_field twist;
    /* Up to 32 unique Digitizers-page Usages, each Barrel Switch (0x44) or Secondary Barrel
     * Switch (0x5A); bit i of a sample's barrel_buttons is barrel_usages[i]. A count of 0 means
     * no barrel button. */
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    /* 1 adds HUT Invert (0x3C), which selects the eraser end; not the Eraser Usage (0x45). */
    uint32_t enable_eraser;
    uint32_t enable_hover; /* 1 lets a sample be in range without touching */
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_pen_options;

/* The pen report under the Digitizers / Digitizer Application Collection (an indirect tablet). */
typedef struct mimhid_digitizer_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    mimhid_integer_field x;
    mimhid_integer_field y;
    uint32_t enable_pressure;
    mimhid_integer_field pressure;
    uint32_t enable_tilt;
    mimhid_integer_field tilt_x;
    mimhid_integer_field tilt_y;
    uint32_t enable_twist_target_specific;
    mimhid_integer_field twist;
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    uint32_t enable_eraser;
    uint32_t enable_hover;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_digitizer_options;

/* An Input report a Raw Node may send: its Report ID and its exact length on the wire. */
typedef struct mimhid_raw_report {
    mimhid_report_id_option report_id;
    uint32_t wire_length;
} mimhid_raw_report;

/* A descriptor of the caller's own. A descriptor with Output items needs requires_output 1 (they
 * arrive as MIMHID_EVENT_OUTPUT_REPORT), one with Feature items requires_feature_response 1 (the
 * board answers with mimhid_node_set_feature_response). */
typedef struct mimhid_raw_options {
    uint32_t struct_size;
    uint32_t reserved;
    const uint8_t* descriptor;
    size_t descriptor_length;
    const mimhid_raw_report* reports;
    size_t report_count;
    uint32_t requires_output;
    uint32_t requires_feature_response;
} mimhid_raw_options;

typedef struct mimhid_report_capability {
    mimhid_report_id_option report_id;
    uint32_t wire_length;
} mimhid_report_capability;

/* What a Spec sends. Filled in by mimhid_spec_manifest and mimhid_node_manifest. */
typedef struct mimhid_capability_manifest {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t input_supported;
    uint32_t output_supported;            /* always 1: the board passes Output reports on */
    uint32_t feature_transport_supported; /* always 1: the board answers Feature reads */
    mimhid_profile_kind profile_kind;
    const mimhid_report_capability* reports; /* valid while the Spec is */
    size_t report_count;
    size_t descriptor_length;
} mimhid_capability_manifest;

/* ---- Input samples ---------------------------------------------------------------------- */

/* Optional values of one touch contact. A field the Spec did not enable must be 0. */
typedef struct mimhid_touchscreen_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimhid_touchscreen_extra;

typedef struct mimhid_touchpad_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimhid_touchpad_extra;

/* One pen state. tip 1 needs in_range 1; without enable_hover, in_range must equal tip. A field the
 * Spec did not enable must be 0. */
typedef struct mimhid_pen_sample {
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
} mimhid_pen_sample;

typedef struct mimhid_digitizer_sample {
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
} mimhid_digitizer_sample;

/* ---- Initializers -----------------------------------------------------------------------
 * Each *_init function zeroes a structure, sets struct_size, and fills in values that work with a
 * Linux target (Windows and macOS targets have not been tried; docs/TARGET_MATRIX.md), so a caller
 * changes only what it needs. Fields that only the caller can know (the USB vendor and product ID,
 * a Raw descriptor, the keys a Toggle Node may press) stay zero and are still required. A value
 * the caller sets afterwards is used as given, zero included where zero is valid.
 *
 * The timeouts are values that work for a board on a USB serial adapter; the library never uses
 * them itself. */
#define MIMHID_CONTROL_TIMEOUT_MS 500U    /* each configuration frame */
#define MIMHID_SEND_TIMEOUT_MS 500U       /* a serial write that makes no progress */
#define MIMHID_DRAIN_TIMEOUT_MS 1000U     /* a close: the neutral reports */
#define MIMHID_RECONNECT_TIMEOUT_MS 3000U /* the board's USB device enumerated again */

static inline void mimhid_struct_init(void* value, size_t size) {
    if (value != NULL) {
        const uint32_t struct_size = (uint32_t)size;
        memset(value, 0, size);
        memcpy(value, &struct_size, sizeof(struct_size));
    }
}

static inline mimhid_integer_field
mimhid_integer_field_make(int32_t logical_minimum, // NOLINT(bugprone-easily-swappable-parameters)
                          int32_t logical_maximum, uint32_t bit_width) {
    mimhid_integer_field field;
    memset(&field, 0, sizeof(field));
    field.logical_minimum = logical_minimum;
    field.logical_maximum = logical_maximum;
    field.bit_width = bit_width;
    return field;
}

/* The boot rate (mimhid_discover reports the fastest a port carries), 50 ms handshake waits,
 * 64-byte reports (one full-speed packet), no logging, and the Linux HID parser limits of the
 * kernel revision in docs/LIMITS.md. The USB identity is a bus-powered (100 mA) USB 2.0 device
 * with remote wakeup, English strings "libmimhid", and the chip's unique id as serial number.
 * usb_device.vendor_id and product_id stay zero: they are yours to set. */
static inline void mimhid_device_config_init(mimhid_device_config* config) {
    mimhid_struct_init(config, sizeof(*config));
    if (config != NULL) {
        config->transport.baud_rate = 115200U;
        config->transport.handshake_timeout_ms = 50U;
        config->transport.maximum_report_bytes = 64U;
        config->transport.linux_hid.descriptor_bytes = 4096U;
        config->transport.linux_hid.fields_per_report = 256U;
        config->transport.linux_hid.global_stack_depth = 4U;
        config->transport.linux_hid.report_size_bits = 256U;
        config->transport.linux_hid.usages = 12288U;
        config->transport.linux_hid.report_data_bits = 65528U;
        config->transport.log_level = MIMHID_LOG_DISABLED;
        config->usb_device.bcd_usb = 0x0200U;
        config->usb_device.ep0_max_packet_size = 64U;
        config->usb_device.bcd_device = 0x0100U;
        config->usb_device.language_id = 0x0409U;
        config->usb_device.manufacturer = "libmimhid";
        config->usb_device.product = "libmimhid";
        config->usb_device.serial_from_chip_id = 1U;
        config->usb_config.configuration_value = 1U;
        config->usb_config.bm_attributes = 0xA0U;
        config->usb_config.bmax_power = 50U;
    }
}

/* MIMHID_CONTROL_TIMEOUT_MS and MIMHID_RECONNECT_TIMEOUT_MS. */
static inline void mimhid_commit_timeouts_init(mimhid_commit_timeouts* timeouts) {
    mimhid_struct_init(timeouts, sizeof(*timeouts));
    if (timeouts != NULL) {
        timeouts->control_timeout_ms = MIMHID_CONTROL_TIMEOUT_MS;
        timeouts->reconnect_timeout_ms = MIMHID_RECONNECT_TIMEOUT_MS;
    }
}

/* A HID interface (HID 1.11, no boot protocol) labelled interface_number, polled every 1 ms through
 * IN endpoint 0x81 + interface_number with 64-byte packets, and no OUT endpoint. The number is an
 * argument because two Nodes with the same one share an interface: give each Node its own unless
 * you mean that. interface_number must be below 15. */
static inline void mimhid_node_config_init(mimhid_node_config* config, uint8_t interface_number) {
    mimhid_struct_init(config, sizeof(*config));
    if (config != NULL) {
        config->iface.number = interface_number;
        config->iface.class_code = 0x03U;
        config->iface.bcd_hid = 0x0111U;
        config->ep_in.address = (uint8_t)(0x81U + interface_number);
        config->ep_in.poll_interval_ms = 1U;
        config->ep_in.max_packet_size = 64U;
    }
}

static inline void mimhid_board_limits_init(mimhid_board_limits* limits) {
    mimhid_struct_init(limits, sizeof(*limits));
}

/* Every key from Keyboard a (0x04) to Keypad Hexadecimal (0xDD). The eight modifiers (0xE0 to
 * 0xE7) are always part of the report. */
static inline void mimhid_keyboard_options_init(mimhid_keyboard_options* options) {
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->usage_minimum = 0x04U;
        options->usage_maximum = 0xDDU;
    }
}

/* Five buttons, 16-bit X and Y, and an 8-bit wheel and pan. */
static inline void mimhid_mouse_options_init(mimhid_mouse_options* options) {
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->button_count = 5U;
        options->x = mimhid_integer_field_make(-32767, 32767, 16U);
        options->y = options->x;
        options->enable_wheel = 1U;
        options->wheel = mimhid_integer_field_make(-127, 127, 8U);
        options->enable_pan = 1U;
        options->pan = options->wheel;
    }
}

/* A Consumer Control (0x0C/0x01) collection of Consumer page keys. The keys themselves
 * (allowed_usages and the three arrays next to it) are the caller's to list. */
static inline void mimhid_toggle_options_init(mimhid_toggle_options* options) {
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->application_page = 0x0CU;
        options->application_usage = 0x01U;
        options->field_page = 0x0CU;
    }
}

/* Two sticks (X/Y and Z/Rz, 8 bits, at rest at 0), twelve buttons from Button 1, and an eight-way
 * Hat. */
static inline void mimhid_gamepad_options_init(mimhid_gamepad_options* options) {
    static const mimhid_gamepad_axis_option axes[4] = {
        {MIMHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_X"},
        {MIMHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Y"},
        {MIMHID_AXIS_Z, 0x01U, 0x32U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Z"},
        {MIMHID_AXIS_RZ, 0x01U, 0x35U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_RZ"}};
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->axes = axes;
        options->axis_count = 4U;
        options->button_count = 12U;
        options->button_usage_minimum = 1U;
        options->dpad_representation = MIMHID_DPAD_HAT;
        options->hat_logical_minimum = 0;
        options->hat_logical_maximum = 7;
        options->hat_bit_width = 4U;
    }
}

/* Ten contacts in one report, X and Y from 0 to 32767, and no optional field. */
static inline void mimhid_touchscreen_options_init(mimhid_touchscreen_options* options) {
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->maximum_contacts = 10U;
        options->contacts_per_report = 10U;
        options->contact_id = mimhid_integer_field_make(0, 15, 4U);
        options->x = mimhid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->contact_count = mimhid_integer_field_make(0, 10, 4U);
    }
}

/* Five contacts in one report, X and Y from 0 to 32767, no optional field, and one click button. */
static inline void mimhid_touchpad_options_init(mimhid_touchpad_options* options) {
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->maximum_contacts = 5U;
        options->contacts_per_report = 5U;
        options->contact_id = mimhid_integer_field_make(0, 15, 4U);
        options->x = mimhid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->contact_count = mimhid_integer_field_make(0, 5, 4U);
        options->button_count = 1U;
    }
}

/* X and Y from 0 to 32767, 12-bit pressure, one Barrel Switch, and hover. */
static inline void mimhid_pen_options_init(mimhid_pen_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->x = mimhid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->enable_pressure = 1U;
        options->pressure = mimhid_integer_field_make(0, 4095, 12U);
        options->barrel_usages = barrel_usages;
        options->barrel_usage_count = 1U;
        options->enable_hover = 1U;
    }
}

/* The same values as mimhid_pen_options_init. */
static inline void mimhid_digitizer_options_init(mimhid_digitizer_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    mimhid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->x = mimhid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->enable_pressure = 1U;
        options->pressure = mimhid_integer_field_make(0, 4095, 12U);
        options->barrel_usages = barrel_usages;
        options->barrel_usage_count = 1U;
        options->enable_hover = 1U;
    }
}

/* Only struct_size: the descriptor and its report table are the caller's. */
static inline void mimhid_raw_options_init(mimhid_raw_options* options) {
    mimhid_struct_init(options, sizeof(*options));
}

static inline void mimhid_capability_manifest_init(mimhid_capability_manifest* manifest) {
    mimhid_struct_init(manifest, sizeof(*manifest));
}

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* mimhid_last_error
 * Ownership: Returns the calling thread's record, valid until that thread's next call.
 * Blocking: None.
 * Synchronization: The record belongs to the calling thread; do not pass it to another.
 * Returns: Never null. */
MIMHID_API const mimhid_error_detail* MIMHID_CALL mimhid_last_error(void);

/* mimhid_result_name
 * Ownership: The string is static.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: The constant's name, such as "MIMHID_ERR_TIMEOUT", with MIMHID_OK recorded; for a value
 * that is not a result, "MIMHID_RESULT_UNKNOWN" with MIMHID_ERR_PARAM recorded. */
MIMHID_API const char* MIMHID_CALL mimhid_result_name(mimhid_result result);

/* mimhid_frame_type_name
 * Ownership: The string is static.
 * Blocking: None.
 * Synchronization: Any thread.
 * Names the serial frame type of mimhid_error_detail.frame_type (docs/PROTOCOL.md).
 * Returns: The frame's name, such as "COMMIT" or "REPORT", with MIMHID_OK recorded; for a value
 * that is not a frame type, "UNKNOWN" with MIMHID_ERR_PARAM recorded. */
MIMHID_API const char* MIMHID_CALL mimhid_frame_type_name(int32_t frame_type);

/* mimhid_version
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: (major << 24) | (minor << 16) | patch of the library that is running. */
MIMHID_API uint32_t MIMHID_CALL mimhid_version(void);

/* ---- Discovery -------------------------------------------------------------------------- */

/* mimhid_discover
 * Ownership: The caller destroys the snapshot with mimhid_discovery_destroy.
 * Blocking: Searches every USB serial port at once, one thread each, waiting up to timeout_ms for
 * each answer; a port another program has open is skipped. On each port it pings at the boot
 * rate, then at every rate of MIMHID_BAUD_RATES (a board a program left on a fast rate), so a
 * serial device that is not a board receives those pings too. On a board it measures the fastest
 * rate that carries pings both ways (baud_rate) and leaves the board on the boot rate. A board
 * whose firmware speaks another protocol is listed with only its version, protocol_version and
 * the rate it answered at, so that it can be flashed again.
 * Synchronization: Any thread.
 * Returns: MIMHID_OK, also when no board was found; MIMHID_ERR_PARAM for a null out_discovery;
 * MIMHID_ERR_UNSET_FIELD for a zero timeout. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_discover(uint32_t timeout_ms,
                                                     mimhid_discovery** out_discovery);

/* mimhid_discovery_count
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread, but not during mimhid_discovery_destroy.
 * Returns: The number of boards with MIMHID_OK recorded, or 0 with MIMHID_ERR_PARAM recorded for
 * a null snapshot. */
MIMHID_API size_t MIMHID_CALL mimhid_discovery_count(const mimhid_discovery* discovery);

/* mimhid_discovery_get
 * Ownership: The entry belongs to the snapshot.
 * Blocking: None.
 * Synchronization: As mimhid_discovery_count.
 * Returns: The entry with MIMHID_OK recorded, or null with MIMHID_ERR_PARAM recorded for a null
 * snapshot or an index out of range. */
MIMHID_API const mimhid_device_info* MIMHID_CALL
mimhid_discovery_get(const mimhid_discovery* discovery, size_t index);

/* mimhid_discovery_destroy
 * Ownership: Frees the snapshot and every entry of it.
 * Blocking: None.
 * Synchronization: No other call may use the snapshot meanwhile.
 * Returns: Nothing. MIMHID_OK is recorded, or MIMHID_ERR_PARAM for null (nothing is done). */
MIMHID_API void MIMHID_CALL mimhid_discovery_destroy(mimhid_discovery* discovery);

/* ---- Device ----------------------------------------------------------------------------- */

/* mimhid_device_open
 * Ownership: Copies config; the caller closes the Device with mimhid_device_close.
 * Blocking: Opens port_path (such as "/dev/ttyACM0" or "COM3") for this process alone, looks for
 * the board at the boot rate and then at each rate of MIMHID_BAUD_RATES (waiting
 * transport.handshake_timeout_ms for each answer), reads its limits, moves the link to
 * transport.baud_rate, and starts the reader thread. It sends no configuration: the board's USB
 * device stays off the target until a Node is opened.
 * Synchronization: Any thread, one call per port.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, a wrong struct_size or a bad value;
 * MIMHID_ERR_UNSET_FIELD for a required field left zero; MIMHID_ERR_UNSUPPORTED for a rate not in
 * MIMHID_BAUD_RATES, one the board cannot make, or one the adapter or wiring did not carry;
 * MIMHID_ERR_OVERFLOW for a maximum_report_bytes the board does not take; MIMHID_ERR_TIMEOUT when
 * no board answered; MIMHID_ERR_PROTOCOL for firmware of another protocol version;
 * MIMHID_ERR_ACCESS, MIMHID_ERR_BUSY or MIMHID_ERR_IO for a port that cannot be opened (another
 * program may have it) or set up. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_open(const char* port_path,
                                                        const mimhid_device_config* config,
                                                        mimhid_device** out_device);

/* mimhid_device_close
 * Ownership: Follows the "Closing" rule; the Device's Nodes go with it.
 * Blocking: Sends the neutral report of every open Node (keys and buttons up, contacts lifted,
 * axes at rest) within drain_timeout_ms, takes the board's USB device off the target unless
 * transport.keep_attached is 1, removes the control routing and loss reporting, puts the board
 * back on the boot rate and closes the port. Each configuration frame waits up to
 * control_timeout_ms. A failed step does not stop the next one.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null or closing Device; MIMHID_ERR_UNSET_FIELD for a
 * zero timeout; MIMHID_ERR_NO_DEVICE when the link was already gone, so the board was not reset;
 * otherwise the first link error on the way out. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_close(mimhid_device* device,
                                                         uint32_t control_timeout_ms,
                                                         uint32_t drain_timeout_ms);

/* mimhid_device_set_usb_identity
 * Ownership: Copies usb_device and usb_config.
 * Blocking: Commits (see the top of this header), waiting as timeouts says; inside a batch it only
 * stages the change.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, a wrong struct_size or a bad value;
 * MIMHID_ERR_UNSET_FIELD for a required field or timeout left zero; the results of
 * mimhid_device_commit. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_set_usb_identity(
    mimhid_device* device, const mimhid_usb_device* usb_device, const mimhid_usb_config* usb_config,
    const mimhid_commit_timeouts* timeouts);

/* mimhid_device_latched_error
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Reads the error the reader thread recorded for the link, and forgets it unless it is
 * MIMHID_ERR_NO_DEVICE, which stays.
 * Returns: MIMHID_OK when there is none; MIMHID_ERR_PARAM for an invalid Device;
 * MIMHID_ERR_NO_DEVICE after the port failed or was unplugged; MIMHID_ERR_IO for another link
 * error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_latched_error(mimhid_device* device);

/* mimhid_device_is_ready
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread while the Device is open.
 * Returns: 1 while the target has the board's USB device mounted and takes reports, else 0. */
MIMHID_API int32_t MIMHID_CALL mimhid_device_is_ready(const mimhid_device* device);

/* mimhid_device_wait_ready
 * Ownership: Nothing.
 * Blocking: Until the target mounts the board's USB device, or timeout_ms.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK once mounted; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_TIMEOUT;
 * MIMHID_ERR_NO_DEVICE when the link fails meanwhile. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_wait_ready(mimhid_device* device,
                                                              uint32_t timeout_ms);

/* mimhid_device_batch_begin
 * Ownership: Nothing.
 * Blocking: None; nothing is sent. Until mimhid_device_batch_end, mimhid_node_open,
 * mimhid_node_close and mimhid_device_set_usb_identity only stage their change, and a Node opened
 * in the batch refuses reports with MIMHID_ERR_BUSY.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_BUSY inside a batch. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_batch_begin(mimhid_device* device);

/* mimhid_device_batch_end
 * Ownership: Nothing.
 * Blocking: Commits every change staged since mimhid_device_batch_begin at once, waiting as
 * timeouts says.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device, a null timeouts or a wrong
 * struct_size; MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_BUSY outside a batch; the
 * results of mimhid_device_commit. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_device_batch_end(mimhid_device* device, const mimhid_commit_timeouts* timeouts);

/* mimhid_device_commit
 * Ownership: Nothing.
 * Blocking: Sends the configuration of every open Node and commits, waiting as timeouts says.
 * mimhid_node_open, mimhid_node_close and mimhid_device_batch_end do this already; call it to
 * retry after a failed commit, or for reconnect_delay_ms: with a nonzero delay on an attached
 * device the board keeps its USB device off the target (D+ at 0 V) for that many milliseconds.
 * Inside a batch it commits the staged changes and ends the batch.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device or timeouts; MIMHID_ERR_UNSET_FIELD
 * for a zero timeout; MIMHID_ERR_OVERFLOW when the Nodes do not fit the board;
 * MIMHID_ERR_DESCRIPTOR_REJECTED when the board refuses the configuration; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_commit(mimhid_device* device,
                                                          uint32_t reconnect_delay_ms,
                                                          const mimhid_commit_timeouts* timeouts);

/* mimhid_device_connect
 * Ownership: Nothing.
 * Blocking: Puts the board's USB device on the target (D+ pull-up on) and waits for the board's
 * answer. A Device that has not been configured yet commits instead.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device or timeouts; MIMHID_ERR_UNSET_FIELD
 * for a zero timeout; a link error; the results of mimhid_device_commit when it commits. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_connect(mimhid_device* device,
                                                           const mimhid_commit_timeouts* timeouts);

/* mimhid_device_disconnect
 * Ownership: Nothing.
 * Blocking: Takes the board's USB device off the target (D+ at 0 V, as if unplugged), waiting up
 * to control_timeout_ms for the board's answer. The next connect or commit configures it again.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_UNSET_FIELD for a zero
 * timeout; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_disconnect(mimhid_device* device,
                                                              uint32_t control_timeout_ms);

/* mimhid_device_remote_wakeup
 * Ownership: Nothing.
 * Blocking: Waits up to control_timeout_ms for the board's answer.
 * Synchronization: Device domain.
 * Wakes a target that suspended the board's USB device; the target must have enabled remote
 * wakeup (usb_config.bm_attributes bit 5 and MIMHID_EVENT_SUSPEND with value 1).
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_UNSET_FIELD for a zero
 * timeout; MIMHID_ERR_BUSY when the device is not suspended; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_remote_wakeup(mimhid_device* device,
                                                                 uint32_t control_timeout_ms);

/* mimhid_device_board_info
 * Ownership: Fills the caller's out_info.
 * Blocking: None; a copy of what the board reported at open.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device or a null out_info. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_board_info(const mimhid_device* device,
                                                              mimhid_device_info* out_info);

/* mimhid_device_limits
 * Ownership: Fills the caller's out_limits (set its struct_size; mimhid_board_limits_init does).
 * Blocking: None; a copy of what the board reported at open.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device, a null out_limits or a wrong
 * struct_size. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_limits(const mimhid_device* device,
                                                          mimhid_board_limits* out_limits);

/* ---- Control transfers of the target ----------------------------------------------------
 * Besides the reports, the target talks to the board on endpoint 0: descriptors, class and vendor
 * requests, GET_REPORT, SET_REPORT. The board answers all of these itself until a Device asks it
 * to report them (MIMHID_EVENT_CONTROL_SETUP) and to hold chosen ones until the program answers.
 * A rule says exactly which requests it holds, for how long, and what happens if no answer
 * comes. */

typedef int32_t mimhid_control_action;
enum {
    MIMHID_CONTROL_ACTION_LEAVE = 1,  /* the board's own handling, as if it had not asked */
    MIMHID_CONTROL_ACTION_ANSWER = 2, /* data (device to host) or an acknowledgement */
    MIMHID_CONTROL_ACTION_STALL = 3,
    MIMHID_CONTROL_ACTION_DROP = 4 /* no answer and no STALL, until the target gives up */
};

#define MIMHID_CONTROL_MAX_RULES 8
#define MIMHID_CONTROL_MAX_TIMEOUT_MS 4000

/* A request matches when (field & mask) == value for each of the four fields; a mask of 0 matches
 * anything. Rules are tried in order and the first match holds the request. */
typedef struct mimhid_control_rule {
    uint8_t request_type_mask; /* bmRequestType: direction, type and recipient */
    uint8_t request_type_value;
    uint8_t request_mask; /* bRequest */
    uint8_t request_value;
    uint16_t value_mask; /* wValue */
    uint16_t value_value;
    uint16_t index_mask; /* wIndex */
    uint16_t index_value;
    /* 1 to MIMHID_CONTROL_MAX_TIMEOUT_MS: how long the board waits for each answer. USB 2.0
     * expects the first data packet of a standard request within 500 ms (9.2.6.4) and a whole
     * control transfer within 5 s (9.2.6.1); hosts may give up sooner, so keep it short. */
    uint32_t timeout_ms;
    mimhid_control_action when_late; /* what the board does when no answer came in time */
} mimhid_control_rule;

/* mimhid_device_set_control_routing
 * Ownership: Copies rules.
 * Blocking: Waits up to control_timeout_ms for the board's answer (a rule's timeout_ms is the
 * board's wait for the program, a different one).
 * Synchronization: Device domain.
 * Replaces the whole set. report_all 1 reports every control transfer, held or not; with no rules
 * and report_all 0 the board handles everything itself, as it does at first. A held request
 * delays only the target's control transfer, never the reports, but while the board waits for the
 * verdict of a host-to-device transfer its USB task does nothing else. SET_ADDRESS and
 * SET_CONFIGURATION are never held, nor a host-to-device data stage longer than
 * max_control_data_bytes. The rules stay until replaced or mimhid_device_close.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device, report_all, count or rule (a value
 * outside its mask, a timeout over MIMHID_CONTROL_MAX_TIMEOUT_MS, an unknown action);
 * MIMHID_ERR_UNSET_FIELD for a zero control_timeout_ms, rule timeout_ms or when_late;
 * MIMHID_ERR_OVERFLOW for more rules than the board holds; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_set_control_routing(
    mimhid_device* device, uint32_t report_all, const mimhid_control_rule* rules, size_t rule_count,
    uint32_t control_timeout_ms);

/* mimhid_device_control_reply
 * Ownership: Copies data.
 * Blocking: Writes one frame, for up to send_timeout_ms without progress; it does not wait for the
 * board.
 * Synchronization: Device domain, and also from the event sink, which is the way to answer in
 * time.
 * Answers the held transfer of token (from MIMHID_EVENT_CONTROL_SETUP, or from
 * MIMHID_EVENT_CONTROL_DATA for the verdict of a host-to-device transfer):
 *  - ANSWER to a device-to-host request: data is the data stage, sent whole whatever wLength says.
 *    More than wLength makes the host report an overrun; data with wLength 0 still starts a data
 *    stage, which is how a device that does so is copied. Empty data is an empty data stage.
 *  - ANSWER to any other request: data must be empty. With a host-to-device data stage the board
 *    takes the data, reports it as MIMHID_EVENT_CONTROL_DATA and waits for the verdict: ANSWER
 *    (acknowledge), STALL, or the rule's when_late.
 *  - STALL, DROP: data must be empty.
 *  - LEAVE: the board's own handling; as a verdict it acknowledges.
 * A reply the board no longer waits for (too late, or the target gave up) is dropped.
 * Returns: MIMHID_OK once the frame is written; MIMHID_ERR_PARAM for an invalid Device, a token
 * of 0 or above 255, an unknown action, or data with an action that takes none;
 * MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_OVERFLOW for data longer than
 * max_control_data_bytes; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_control_reply(mimhid_device* device,
                                                                 uint32_t token,
                                                                 mimhid_control_action action,
                                                                 const uint8_t* data, size_t length,
                                                                 uint32_t send_timeout_ms);

/* The largest interval_ms of mimhid_device_set_loss_reporting, which is the least time between two
 * MIMHID_EVENT_REPORT_LOST events. */
#define MIMHID_LOSS_MAX_INTERVAL_MS 60000U

/* mimhid_device_set_loss_reporting
 * Ownership: Nothing.
 * Blocking: Waits up to control_timeout_ms for the board's answer.
 * Synchronization: Device domain.
 * enabled 1 makes the board report the reports it drops because its queue is full
 * (MIMHID_EVENT_REPORT_LOST), at most one event per interval_ms (0 to MIMHID_LOSS_MAX_INTERVAL_MS;
 * 0 reports each time at once). enabled 0, which is how the board starts, needs interval_ms 0 and
 * puts nothing extra on the link. The setting stays until replaced or mimhid_device_close.
 * Needs firmware 1.5.0 or later.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device, enabled or interval_ms;
 * MIMHID_ERR_UNSET_FIELD for a zero timeout; a link error (MIMHID_ERR_TIMEOUT from older
 * firmware, which does not answer). */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_set_loss_reporting(mimhid_device* device,
                                                                      uint32_t enabled,
                                                                      uint32_t interval_ms,
                                                                      uint32_t control_timeout_ms);

/* ---- Spec -------------------------------------------------------------------------------
 * The factories copy their options and every array and string the options point to. The Spec
 * holds one reference for the caller. All factories return:
 *   MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, a wrong struct_size or a value out of range;
 *   MIMHID_ERR_UNSET_FIELD for a required field left zero; MIMHID_ERR_OVERFLOW when the descriptor
 *   or report would be too large; the factory's own results listed with it. */

/* mimhid_spec_create_keyboard
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW; MIMHID_ERR_PARAM also for a
 * range that includes 0x00 to 0x03 or the modifiers. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_keyboard(const mimhid_keyboard_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_mouse
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_mouse(const mimhid_mouse_options* options,
                                                              mimhid_spec** out_spec);

/* mimhid_spec_create_toggle
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW; MIMHID_ERR_UNSUPPORTED for a
 * semantic that press and release cannot express, or a Camera Control Usage that is not
 * MIMHID_USAGE_ONE_SHOT. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_toggle(const mimhid_toggle_options* options,
                                                               mimhid_spec** out_spec);

/* mimhid_spec_create_gamepad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_gamepad(const mimhid_gamepad_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_touchscreen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_touchscreen(const mimhid_touchscreen_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_touchpad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_touchpad(const mimhid_touchpad_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_pen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_pen(const mimhid_pen_options* options,
                                                            mimhid_spec** out_spec);

/* mimhid_spec_create_digitizer
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_digitizer(const mimhid_digitizer_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_raw
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, MIMHID_OK through MIMHID_ERR_OVERFLOW; MIMHID_ERR_PARAM also for a
 * descriptor that does not parse or does not match the report table; MIMHID_ERR_UNSUPPORTED for
 * Output or Feature items without the matching requires_* flag, a long item, or a construct the
 * parser does not handle. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_raw(const mimhid_raw_options* options,
                                                            mimhid_spec** out_spec);

/* mimhid_spec_retain
 * Ownership: Adds a reference for the caller, who must already hold one.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: Nothing. MIMHID_OK is recorded; MIMHID_ERR_PARAM for null, or MIMHID_ERR_OVERFLOW when
 * the count is full (no reference is added). */
MIMHID_API void MIMHID_CALL mimhid_spec_retain(mimhid_spec* spec);

/* mimhid_spec_release
 * Ownership: Gives up one of the caller's references; the last one frees the Spec. A Node keeps
 * its own reference until it closes.
 * Blocking: None.
 * Synchronization: Any thread, as long as the last release does not race any other use.
 * Returns: Nothing. MIMHID_OK is recorded, or MIMHID_ERR_PARAM for null (nothing is done). */
MIMHID_API void MIMHID_CALL mimhid_spec_release(mimhid_spec* spec);

/* mimhid_spec_descriptor
 * Ownership: The bytes belong to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_descriptor(const mimhid_spec* spec,
                                                            const uint8_t** bytes, size_t* length);

/* mimhid_spec_manifest
 * Ownership: Fills the caller's manifest; manifest->reports belongs to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_manifest(const mimhid_spec* spec,
                                                          mimhid_capability_manifest* manifest);

/* ---- Node ------------------------------------------------------------------------------- */

/* mimhid_node_open
 * Ownership: Copies config; the Node keeps a reference to spec until it closes.
 * Blocking: Sends the configuration of every Node and commits (see the top of this header),
 * waiting as timeouts says, so the target sees the board's USB device unplugged and plugged in
 * again and every other Node is briefly away. Inside a batch it only stages the Node; timeouts is
 * still required. It returns once the board has enumerated; mimhid_device_wait_ready waits for the
 * target to mount the device.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null argument, a wrong struct_size, a bad value, or
 * Nodes sharing an interface that do not follow the rules of mimhid_usb_interface;
 * MIMHID_ERR_UNSET_FIELD for a required field or timeout left zero; MIMHID_ERR_OVERFLOW when the
 * board has no room for the Node, its interface or its descriptor, or the Spec exceeds
 * maximum_report_bytes or a linux_hid limit; the results of mimhid_device_commit. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_open(mimhid_device* device, mimhid_spec* spec,
                                                      const mimhid_node_config* config,
                                                      const mimhid_commit_timeouts* timeouts,
                                                      mimhid_node** out_node);

/* mimhid_node_open_raw
 * Ownership: Copies descriptor and config.
 * Blocking: As mimhid_node_open. The descriptor is sent as it is, neither parsed nor checked, so
 * the Node has no input calls: send its reports with mimhid_report_send.
 * Synchronization: Device domain.
 * Returns: As mimhid_node_open, MIMHID_OK through the results of mimhid_device_commit;
 * MIMHID_ERR_PARAM also for a null or empty descriptor. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_open_raw(mimhid_device* device,
                                                          const uint8_t* descriptor,
                                                          size_t descriptor_length,
                                                          const mimhid_node_config* config,
                                                          const mimhid_commit_timeouts* timeouts,
                                                          mimhid_node** out_node);

/* mimhid_node_close
 * Ownership: Follows the "Closing" rule.
 * Blocking: Finishes a touch frame that has started and sends the neutral report (keys and
 * buttons up, contacts lifted, axes at rest) within drain_timeout_ms, then commits the remaining
 * Nodes as mimhid_node_open does; closing the last Node takes the USB device off the target.
 * Inside a batch it only stages the removal. A failed step does not stop the next one; after a
 * failed commit the target may still see the Node until a commit succeeds (mimhid_device_commit).
 * Synchronization: Device domain; not from the event sink.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null or closed Node, a closing Device, or a wrong
 * timeouts struct_size; MIMHID_ERR_UNSET_FIELD for a zero timeout; otherwise the first error on
 * the way out. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_close(mimhid_node* node,
                                                       const mimhid_commit_timeouts* timeouts,
                                                       uint32_t drain_timeout_ms);

/* mimhid_node_id
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Returns: The number the board knows the Node by, plus one (so it is nonzero), with MIMHID_OK
 * recorded; or 0 with MIMHID_ERR_PARAM recorded for a null or closed Node. */
MIMHID_API uint16_t MIMHID_CALL mimhid_node_id(const mimhid_node* node);

/* mimhid_node_set_feature_response
 * Ownership: Copies data.
 * Blocking: Waits up to control_timeout_ms for the board's answer; inside a batch, or for a Node
 * not yet committed, it is sent with the commit instead.
 * Synchronization: Device domain.
 * Sets what the board answers (the bytes after the Report ID, at most the board's
 * max_feature_bytes; mimhid_device_limits) when the target reads Feature report report_id of this
 * Node. A touch Spec with enable_contact_count_maximum_feature_declaration has its answer set
 * already.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node, null data with a nonzero length, or
 * more than max_feature_bytes; MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_OVERFLOW when
 * the board has no room; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_set_feature_response(mimhid_node* node,
                                                                      uint8_t report_id,
                                                                      const uint8_t* data,
                                                                      size_t length,
                                                                      uint32_t control_timeout_ms);

/* mimhid_node_set_queue_depth
 * Ownership: Nothing.
 * Blocking: As mimhid_node_set_feature_response.
 * Synchronization: Device domain.
 * How many of this Node's reports may wait on the board for the target to read them; a report
 * that finds the queue full is dropped (MIMHID_EVENT_REPORT_LOST). With depth 1 a report is at
 * most one poll interval late. The board keeps one queue per interface, and Nodes that share an
 * interface share the least depth any of them set. At first the queue holds all it can: 15
 * reports when the board's max_report_bytes is at most 64, else 7. The depth stays with the Node
 * across commits. Needs firmware 1.5.0 or later.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node or a depth of 0 or above 255;
 * MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_OVERFLOW for a depth beyond the board's
 * queue; a link error (MIMHID_ERR_TIMEOUT from older firmware, which does not answer). */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_set_queue_depth(mimhid_node* node, uint32_t depth,
                                                                 uint32_t control_timeout_ms);

/* mimhid_node_manifest
 * Ownership: As mimhid_spec_manifest; manifest->reports is valid while the Node is open.
 * Blocking: None.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_manifest(const mimhid_node* node,
                                                          mimhid_capability_manifest* manifest);

/* mimhid_node_submit
 * Ownership: The report is built in the library's own buffer.
 * Blocking: Writes one report of the Node's current state, if it changed, to the serial port and
 * returns once the driver has it (microseconds on an idle port). It never waits for the target;
 * send_timeout_ms only bounds a write that makes no progress.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK, also when there was nothing to send; MIMHID_ERR_PARAM for an invalid Node;
 * MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_UNSUPPORTED for a Raw Node (use
 * mimhid_raw_submit); MIMHID_ERR_BUSY for a Node opened in a batch that has not ended, while the
 * board enumerates, or while a report of the Node is being sent; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_submit(mimhid_node* node,
                                                        uint32_t send_timeout_ms);

/* mimhid_node_submit_blocking
 * Ownership: As mimhid_node_submit.
 * Blocking: Writes reports until the Node's state has been sent, a touch frame of several reports
 * and a mouse's pending movement included, or timeout_ms has passed; each write is bounded by what
 * is left of it.
 * Synchronization: Device domain.
 * Returns: MIMHID_OK; MIMHID_ERR_TIMEOUT when timeout_ms passed first; the results of
 * mimhid_node_submit. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_submit_blocking(mimhid_node* node,
                                                                 uint32_t timeout_ms);

/* ---- Input ------------------------------------------------------------------------------
 * These calls change a Node's state; mimhid_node_submit sends it. None of them blocks, allocates
 * or does I/O. Each belongs to the Device domain and works only on a Node of its own profile.
 * A press and the release after it are never merged: until the report carrying the press has been
 * sent, the release is refused with MIMHID_ERR_BUSY, so a quick tap is not lost. Every input call
 * returns:
 *   MIMHID_OK; MIMHID_ERR_PARAM for a Node of another profile or a value out of range;
 *   MIMHID_ERR_BUSY while a report of the Node is being sent, an opposite edge is unsent, the
 *   Node was opened in a batch that has not ended, or the board enumerates;
 *   MIMHID_ERR_NO_DEVICE once the link is gone; the call's own results listed with it. */

/* mimhid_keyboard_key
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) one Keyboard page Usage, a modifier or a key of the Spec's
 * range. Closing the Node releases every key still down.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_keyboard_key(mimhid_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimhid_mouse_move
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Adds to the pending X and Y movement. A report carries as much as the fields hold and keeps the
 * rest for the next one.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE; MIMHID_ERR_OVERFLOW when
 * the 64-bit pending total overflows. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_move(mimhid_node* node, int32_t dx, int32_t dy);

/* mimhid_mouse_scroll
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Adds to the pending wheel and pan movement, as mimhid_mouse_move does.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE; MIMHID_ERR_UNSUPPORTED
 * for a nonzero value on a field the Spec lacks; MIMHID_ERR_OVERFLOW when the 64-bit pending total
 * overflows. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_scroll(mimhid_node* node, int32_t wheel,
                                                         int32_t pan);

/* mimhid_mouse_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_button(mimhid_node* node, uint32_t button,
                                                         uint32_t down);

/* mimhid_toggle_usage
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) one Usage of the Spec's allow-list; any number may be
 * pressed at once. Closing the Node releases every Usage still down.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_toggle_usage(mimhid_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimhid_gamepad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_button(mimhid_node* node, uint32_t button,
                                                           uint32_t down);

/* mimhid_gamepad_axis
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets axes[axis_index] of the Spec to value, which must be in its logical range.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_axis(mimhid_node* node, size_t axis_index,
                                                         int32_t value);

/* mimhid_gamepad_dpad
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the D-pad from four 0/1 directions. With a Hat, up and down (or left and right) together
 * are refused and no direction sends the Null state; with four buttons the bits go out as given.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE; MIMHID_ERR_PARAM also
 * when the Spec has no D-pad. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_dpad(mimhid_node* node, uint32_t up,
                                                         uint32_t down, uint32_t right,
                                                         uint32_t left);

/* mimhid_touchscreen_contact
 * Ownership: Copies extra, which may be null (every optional field 0).
 * Blocking: None.
 * Synchronization: Device domain.
 * down 1 places contact_id in a free slot, or moves it if it is already down; down 0 lifts it at
 * x, y. A lifted contact_id can be placed again once its lift has been sent. While a frame split
 * over several reports is being sent, every call is refused with MIMHID_ERR_BUSY.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE; MIMHID_ERR_PARAM also for
 * lifting a contact_id that is not down; MIMHID_ERR_BUSY also for placing one whose lift is
 * unsent; MIMHID_ERR_OVERFLOW when every slot is taken. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_touchscreen_contact(mimhid_node* node, uint32_t contact_id, uint32_t down, int32_t x,
                           int32_t y, const mimhid_touchscreen_extra* extra);

/* mimhid_touchpad_contact
 * Ownership: Copies extra, which may be null.
 * Blocking: None.
 * Synchronization: Device domain.
 * As mimhid_touchscreen_contact, for a Touchpad Node.
 * Returns: As mimhid_touchscreen_contact, MIMHID_OK through MIMHID_ERR_OVERFLOW. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_touchpad_contact(mimhid_node* node, uint32_t contact_id,
                                                             uint32_t down, int32_t x, int32_t y,
                                                             const mimhid_touchpad_extra* extra);

/* mimhid_touchpad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Presses (down 1) or releases (down 0) click button 1 to button_count.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_touchpad_button(mimhid_node* node, uint32_t button,
                                                            uint32_t down);

/* mimhid_pen_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the whole pen state. in_range 0 takes the pen away: In Range, Tip, pressure, Invert and the
 * barrel bits go out as 0 with the sample's own X, Y, tilt and twist, so repeat the last position
 * to leave the pointer where it was. Switching between tip and eraser while in range first sends
 * an out-of-range report.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_pen_update(mimhid_node* node,
                                                       const mimhid_pen_sample* sample);

/* mimhid_digitizer_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Device domain.
 * As mimhid_pen_update, for a Digitizer Node.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_digitizer_update(mimhid_node* node,
                                                             const mimhid_digitizer_sample* sample);

/* mimhid_battery_update
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Device domain.
 * Sets the battery level of a Node whose Spec has enable_battery. It goes out with the next
 * mimhid_node_submit that has no input report to send, and only when it changed. A level not yet
 * sent is dropped on close.
 * Returns: As every input call, MIMHID_OK through MIMHID_ERR_NO_DEVICE; MIMHID_ERR_PARAM also for
 * a Node without a battery report. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_battery_update(mimhid_node* node, int32_t strength);

/* ---- Reports of the caller's own -------------------------------------------------------- */

/* mimhid_raw_submit
 * Ownership: Copies report.
 * Blocking: As mimhid_node_submit.
 * Synchronization: Device domain.
 * Sends one report of a Raw Spec's Node after checking it against the Spec's report table.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a Node that is not Raw, a null report, a Report ID or
 * length the Spec does not accept, or nonzero padding bits; MIMHID_ERR_UNSET_FIELD for a zero
 * timeout; MIMHID_ERR_BUSY as for mimhid_node_submit; MIMHID_ERR_OVERFLOW for a report longer than
 * maximum_report_bytes; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_raw_submit(mimhid_node* node, const uint8_t* report,
                                                       size_t length, uint32_t send_timeout_ms);

/* mimhid_report_send
 * Ownership: Copies report_bytes.
 * Blocking: As mimhid_node_submit.
 * Synchronization: Device domain.
 * Sends report_bytes as one report of the Node, Report ID included, without any check and without
 * touching the Node's state: the way to send on a Node of mimhid_node_open_raw.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node or a null report_bytes with a nonzero
 * length; MIMHID_ERR_UNSET_FIELD for a zero timeout; MIMHID_ERR_BUSY for a Node opened in a batch
 * that has not ended or while the board enumerates; MIMHID_ERR_OVERFLOW for a report longer than
 * maximum_report_bytes; a link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_report_send(mimhid_node* node,
                                                        const uint8_t* report_bytes,
                                                        size_t report_length,
                                                        uint32_t send_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
