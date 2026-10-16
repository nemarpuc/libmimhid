// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_H
#define MIMHID_H

/*
 * This header is the complete stable C ABI for libmimhid. It deliberately exposes
 * no platform-specific serial handle or C++ type, and it contains no option initializer or
 * implicit preset. It follows libaoahid's structure: the application builds Specs, opens them
 * as Nodes on a Device, and submits reports, here to a microcontroller board over a serial
 * link. Two layers are visible in this header: low level (Device, Nodes, raw reports, events,
 * bus control; usable without any profile helper) and profile layer (Spec factories and the
 * input mutators that build reports on top of a Node). Linking only against the low level
 * is enough for hand-written descriptors.
 */

#include <stddef.h>
#include <stdint.h>

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

#define MIMHID_VERSION_MAJOR 0
#define MIMHID_VERSION_MINOR 1
#define MIMHID_VERSION_PATCH 0

typedef struct mimhid_discovery mimhid_discovery;
typedef struct mimhid_device mimhid_device;
typedef struct mimhid_spec mimhid_spec;
typedef struct mimhid_node mimhid_node;

/* Every public enum domain has an explicit 32-bit ABI representation.  The
 * anonymous enums below provide integral constants without making structure
 * layout depend on a compiler's enum-size option. */
typedef int32_t mimhid_result;
enum {
    MIMHID_OK = 0,
    MIMHID_ERR_PARAM = 1,
    MIMHID_ERR_UNSET_FIELD = 2,
    MIMHID_ERR_UNSUPPORTED = 3,
    MIMHID_ERR_PROTOCOL = 4,
    MIMHID_ERR_ACCESS = 6,
    MIMHID_ERR_BUSY = 7,
    MIMHID_ERR_NO_DEVICE = 8,
    MIMHID_ERR_TIMEOUT = 10,
    MIMHID_ERR_SHORT_TRANSFER = 11,
    MIMHID_ERR_DESCRIPTOR_REJECTED = 12,
    MIMHID_ERR_IO = 13,
    MIMHID_ERR_OVERFLOW = 14,
    MIMHID_ERR_INTERNAL = 16
};

typedef int32_t mimhid_log_level;
enum { MIMHID_LOG_DISABLED = 1, MIMHID_LOG_ERROR = 2, MIMHID_LOG_INFO = 3, MIMHID_LOG_TRACE = 4 };

typedef int32_t mimhid_profile_kind;
enum {
    MIMHID_PROFILE_KEYBOARD = 1,
    MIMHID_PROFILE_MOUSE = 2,
    /* Always the fixed-slot Multi-Touch Touch Screen Application Collection
     * Usage. */
    MIMHID_PROFILE_TOUCHSCREEN = 3,
    /* Always the Digitizers / Touch Pad Application Collection Usage. */
    MIMHID_PROFILE_TOUCHPAD = 4,
    /* Always the Digitizers / Pen Application Collection Usage. */
    MIMHID_PROFILE_PEN = 5,
    /* Always the Digitizers / Digitizer Application Collection Usage. */
    MIMHID_PROFILE_DIGITIZER = 6,
    /* Always the Generic Desktop / Game Pad Application Collection Usage. */
    MIMHID_PROFILE_GAMEPAD = 7,
    /* One-bit allow-listed selector controls on any HUT Usage Page: Consumer,
     * System Control, Camera Control, Telephony, or a caller-chosen page. The
     * exact Application Collection and field Usage Page are caller fields. */
    MIMHID_PROFILE_TOGGLE = 8,
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
enum { MIMHID_DPAD_NONE = 1, MIMHID_DPAD_HAT = 2, MIMHID_DPAD_BUTTONS = 3 };

/* Exact one-control report semantics selected for each Consumer, System,
 * Camera, or Telephony Usage.  These values name a descriptor encoding, not
 * an operating-system mapping.  LINEAR, DYNAMIC_VALUE, NAMED_ARRAY, and the
 * two-direction ON_OFF_PAIR are defined values so a caller can describe
 * the HUT type without lying; the press/release profile factories reject them
 * as unsupported because their value/state cannot be represented by that API. */
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

typedef void(MIMHID_CALL* mimhid_log_sink)(void* user, mimhid_log_level level, const char* message);

typedef struct mimhid_error_detail {
    int32_t code;
    const char* field;
    const char* reason;
    /* errno or the Win32 error of a failed serial call, or the board's NAK code. */
    int32_t transport_status;
    /* The wire frame type involved (MIMHID_T_* in proto/mimhid_proto.h), or zero. */
    int32_t frame_type;
    /* Nonzero node id (the node index plus one), or zero. */
    uint16_t node_id;
    uint16_t report_id;
    uint32_t offset;
    uint32_t length;
} mimhid_error_detail;

typedef int32_t mimhid_event_kind;
enum {
    MIMHID_EVENT_MOUNTED = 1,
    MIMHID_EVENT_UNMOUNTED = 2,
    /* value: 1 when the target enabled remote wakeup for the device. */
    MIMHID_EVENT_SUSPEND = 3,
    MIMHID_EVENT_RESUME = 4,
    /* The target sent an Output report (a keyboard's LEDs, for example) or a Feature report. */
    MIMHID_EVENT_OUTPUT_REPORT = 5,
    MIMHID_EVENT_FEATURE_REPORT = 6,
    /* value: 0 for the boot protocol, 1 for the report protocol. */
    MIMHID_EVENT_SET_PROTOCOL = 7,
    /* value: the idle rate in 4 ms units. */
    MIMHID_EVENT_SET_IDLE = 8
};

/* Delivered on the library's reader thread. The callback must be short, must not call any
 * libmimhid function, and must copy data it wants to keep: data is valid only during the call. */
typedef struct mimhid_event {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_event_kind kind;
    /* The node the event belongs to, or zero when it concerns the whole device. */
    uint16_t node_id;
    uint16_t report_id;
    uint32_t value;
    /* Report bytes without the Report ID (OUTPUT_REPORT and FEATURE_REPORT). */
    const uint8_t* data;
    size_t length;
} mimhid_event;

typedef void(MIMHID_CALL* mimhid_event_sink)(void* user, const mimhid_event* event);

/* A board found on a serial port by mimhid_discover(). */
typedef struct mimhid_device_info {
    char port_name[64];       /* "/dev/ttyUSB0", "/dev/cu.usbserial-0001", "COM3" */
    char description[64];     /* the bridge's USB product string, where the OS gives one */
    uint8_t version_major;    /* firmware version */
    uint8_t version_minor;
    uint8_t version_patch;
    uint8_t is_ready;         /* 1 when the target has mounted the board's USB device */
    uint8_t protocol_version; /* serial protocol version the firmware speaks */
    uint8_t max_nodes;        /* nodes the firmware can hold */
    uint8_t max_interfaces;   /* USB HID interfaces (IN endpoints) the MCU provides */
    uint8_t max_report_bytes; /* largest report the firmware accepts */
    uint16_t max_descriptor_bytes; /* room for all report descriptors together */
    uint8_t has_unique_id;    /* 1 when the MCU can supply a chip serial number */
    uint8_t reserved8;
} mimhid_device_info;

typedef struct mimhid_device_options {
    uint32_t struct_size;
    uint32_t reserved;
    /* Link tuning: zero selects the documented bounded fallback. These are project policies,
     * not USB requirements. */
    /* zero -> 921600; the firmware is built for this rate */
    uint32_t baud_rate;
    /* zero -> 500 ms per configuration frame; each frame is tried three times */
    uint32_t control_timeout_ms;
    /* zero -> 500 ms. A serial write that makes no progress for this long fails. */
    uint32_t send_timeout_ms;
    /* zero -> 3000 ms: how long to wait for the board to re-enumerate after a node is opened
     * or closed. */
    uint32_t reconnect_timeout_ms;
    /* zero -> the board's limit; otherwise at most that limit */
    uint32_t maximum_report_bytes;
    /* zero -> 1000 ms: bound on the neutral reports sent when a node closes */
    uint32_t close_drain_timeout_ms;
    /* Caller-selected limits for the exact target HID parser revision. These are not universal
     * Linux, HID, or library defaults. Each must be nonzero. */
    uint32_t linux_descriptor_policy_bytes;
    uint32_t linux_hid_fields_per_report_policy;
    uint32_t linux_hid_global_stack_depth_policy;
    uint64_t linux_hid_usages_policy;
    uint64_t linux_hid_report_data_policy_bits;
    uint32_t linux_hid_report_size_policy_bits;
    /* 0 or 1. 1 decodes every generated report again before it is sent. */
    uint32_t validate_reports;
    /* 0 or 1. Zero: mimhid_device_close detaches the board's USB device from the target.
     * One: the device stays attached with its last configuration. */
    uint32_t keep_attached;

    /* USB device identity and configuration. */
    uint16_t vendor_id;   /* required, nonzero */
    uint16_t product_id;
    uint16_t bcd_device;  /* zero -> 0x0100 */
    uint16_t language_id; /* zero -> 0x0409 */
    /* NUL-terminated printable ASCII, at most 31 characters, or null. Null manufacturer and
     * product select "mimhid" and "mimhid Composite HID"; null serial and configuration
     * leave those strings out. */
    const char* manufacturer;
    const char* product;
    const char* serial;
    const char* configuration;
    /* 0 or 1. 1 with a null serial uses the MCU's unique id as the serial number. */
    uint32_t serial_from_chip_id;
    uint8_t device_class;    /* 0 -> defined by the interfaces; 0xEF/2/1 for an IAD composite */
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t reserved8;
    /* 0 or 1. Written to the configuration's bmAttributes. */
    uint32_t self_powered;
    uint32_t remote_wakeup;
    /* zero -> 100 mA; at most 500 */
    uint32_t max_power_ma;

    /* Events from the target. Null disables them. */
    mimhid_event_sink event_sink;
    void* event_user;
    mimhid_log_level log_level;
    mimhid_log_sink log_sink;
    void* log_user;
} mimhid_device_options;

typedef int32_t mimhid_boot_protocol;
enum {
    MIMHID_BOOT_NONE = 0,
    /* The two boot values require a Raw node whose descriptor follows HID 1.11 Appendix B. */
    MIMHID_BOOT_KEYBOARD = 1,
    MIMHID_BOOT_MOUSE = 2
};

/* Settings of the USB interface that carries a node. */
typedef struct mimhid_node_options {
    uint32_t struct_size;       /* sizeof(mimhid_node_options) */
    uint32_t interface_number;  /* 0, 1, 2... must be explicitly specified (no auto) */
    uint32_t poll_interval_ms;  /* polling interval in ms (1..255 ms, typical 1) */
    uint32_t ep_in_size;        /* IN packet size (8, 16, 32, or 64; typical 64) */
    uint32_t ep_out_size;       /* OUT packet size (0: OUT disabled, 8/16/32/64: OUT enabled) */
    uint32_t boot_protocol;     /* 0: None, 1: Keyboard, 2: Mouse */
    uint16_t bcd_hid;           /* 0x0111 (HID 1.11) */
    uint8_t  country_code;      /* 0: Not Supported */
    const char* interface_name; /* interface string (NULL: none) */
} mimhid_node_options;

typedef struct mimhid_physical_properties {
    /* Disabled state is all-zero. When enabled, the four HID 1.11 Global
     * items are emitted for this field and reset immediately after its Main
     * item so they cannot leak into the next field. */
    uint32_t enabled;
    int32_t minimum;
    int32_t maximum;
    int32_t unit_exponent;
    uint32_t unit;
} mimhid_physical_properties;

typedef struct mimhid_integer_field {
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t bit_width;
    mimhid_physical_properties physical;
} mimhid_integer_field;

typedef struct mimhid_report_id_option {
    uint32_t enabled;
    uint8_t value;
    uint8_t reserved8[3];
} mimhid_report_id_option;

/* Battery Strength (Generic Device Controls 0x06/0x20) as a second Input
 * report in the profile's own Application Collection. Linux registers a
 * battery only for a HID device that also has input, so it rides on a
 * profile instead of being one. Disabled state is all-zero. When enabled,
 * the profile's report_id and this report_id must both be enabled and
 * differ; neither is chosen for you. There is no "unknown" value: Linux drops
 * out-of-range battery values and reports the level as unknown until the
 * first one arrives. */
typedef struct mimhid_battery_report {
    mimhid_report_id_option report_id;
    mimhid_integer_field strength;
} mimhid_battery_report;

/* The Keyboard profile is exclusively full-NKRO: every declared Usage is a
 * one-bit Variable field, so every simultaneously pressed key is reported at
 * once with no Array slot count and no ErrorRollOver overflow encoding. */
typedef struct mimhid_keyboard_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint16_t usage_minimum;
    uint16_t usage_maximum;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_keyboard_options;

typedef struct mimhid_mouse_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint32_t button_count;
    mimhid_integer_field x;
    mimhid_integer_field y;
    uint32_t enable_wheel;
    mimhid_integer_field wheel;
    uint32_t enable_pan;
    mimhid_integer_field pan;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_mouse_options;

/* The four pointer arrays below each contain allowed_usage_count entries.
 * Every pointer and every expected-event string is required.
 *
 * application_page/application_usage select the Application Collection (for
 * example HUT 0x0C/0x01 Consumer Control, 0x01/0x80 System Control, or
 * 0x0B/0x01 Telephony Phone); field_page selects the Usage Page every entry
 * of allowed_usages is read from (for example Camera Control 0x90, nested
 * inside a Consumer Control Application Collection). A caller free to declare
 * any HUT page here is exactly what lets one factory reach Consumer, System
 * Control, Camera keys, Telephony keys, or another vendor/HUT page without a
 * dedicated function per page. Camera Control (0x90) fields are additionally
 * restricted to HUT 1.7's Auto-focus (0x20) and Shutter (0x21), each as a One
 * Shot Control, because that page defines no other Usage. */
typedef struct mimhid_toggle_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    uint16_t application_page;
    uint16_t application_usage;
    uint16_t field_page;
    uint16_t reserved16;
    const uint16_t* allowed_usages;
    size_t allowed_usage_count;
    const mimhid_usage_semantic* usage_semantics;
    const char* const* expected_linux_event_types;
    const char* const* expected_linux_codes;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_toggle_options;

typedef struct mimhid_gamepad_axis_option {
    mimhid_axis_role role;
    uint16_t usage_page;
    uint16_t usage;
    mimhid_integer_field value;
    int32_t neutral_value;
    const char* expected_linux_code;
} mimhid_gamepad_axis_option;

typedef struct mimhid_gamepad_options {
    uint32_t struct_size;
    uint32_t reserved;
    mimhid_report_id_option report_id;
    const mimhid_gamepad_axis_option* axes;
    size_t axis_count;
    uint32_t button_count;
    uint16_t button_usage_minimum;
    /* Explicitly selects no D-pad, a Hat Switch, or the four Generic Desktop
     * D-pad OOC usages. The Hat requires caller-
     * explicit logical 0..7 in four bits; fields must be zero outside HAT. */
    mimhid_dpad_representation dpad_representation;
    int32_t hat_logical_minimum;
    int32_t hat_logical_maximum;
    uint32_t hat_bit_width;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_gamepad_options;

/* Always the fixed-slot Multi-Touch report shape (explicit Contact
 * Identifier and Contact Count fields, one packet per declared slot unless
 * enable_multi_packet_frames selects more) under the Touch Screen Application
 * Collection Usage. Report shapes and the optional contact-count-maximum
 * feature match the Linux hid-multitouch driver observed behaviour; other
 * targets are untested. */
typedef struct mimhid_touchscreen_options {
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
    /* HUT Azimuth is counter-clockwise through a full circular range. The
     * audited Linux implementation at commit 35556bed836f8dc07ac55f69c8d17dce3e7f0e25
     * consumes samples in [0, logical_maximum). That half-open range is an
     * exact-revision implementation observation, not a HID/HUT rule or a claim
     * about another target. */
    mimhid_integer_field azimuth;
    uint32_t enable_scan_time;
    mimhid_integer_field scan_time;
    uint32_t scan_time_unit_100us;
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_touchscreen_options;

/* Same fixed-slot Multi-Touch report shape as mimhid_touchscreen_options, under the
 * Digitizers / Touch Pad Application Collection Usage instead of Touch Screen,
 * plus button_count physical click buttons. button_count may be zero for a
 * buttonless clickpad; only then does mimhid_touchpad_button stay inert. */
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
    /* HUT Azimuth is counter-clockwise through a full circular range. The
     * audited Linux implementation at commit 35556bed836f8dc07ac55f69c8d17dce3e7f0e25
     * consumes samples in [0, logical_maximum). That half-open range is an
     * exact-revision implementation observation, not a HID/HUT rule or a claim
     * about another target. */
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

typedef struct mimhid_pen_options {
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
    /* Exact Digitizers-page Usage IDs for caller-selected barrel controls.
     * HUT does not define these controls as a contiguous numeric range. */
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    /* When enabled, the wire field is HUT Invert (0D/3C), the opposite-end
     * selector. It is not the separate Eraser Usage (0D/45). */
    uint32_t enable_eraser;
    uint32_t enable_hover;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_pen_options;

/* Same stylus report shape as mimhid_pen_options, under the Digitizers /
 * Digitizer Application Collection Usage instead of Pen. */
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
    /* Exact Digitizers-page Usage IDs for caller-selected barrel controls.
     * HUT does not define these controls as a contiguous numeric range. */
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    /* When enabled, the wire field is HUT Invert (0D/3C), the opposite-end
     * selector. It is not the separate Eraser Usage (0D/45). */
    uint32_t enable_eraser;
    uint32_t enable_hover;
    uint32_t enable_battery;
    mimhid_battery_report battery;
} mimhid_digitizer_options;

typedef struct mimhid_raw_report {
    mimhid_report_id_option report_id;
    uint32_t wire_length;
} mimhid_raw_report;

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

typedef struct mimhid_capability_manifest {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t input_supported;
    uint32_t output_supported;
    uint32_t feature_transport_supported;
    mimhid_profile_kind profile_kind;
    const mimhid_report_capability* reports;
    size_t report_count;
    size_t descriptor_length;
} mimhid_capability_manifest;

/* Optional fields for mimhid_touchscreen_contact(); pass null to send
 * zero/absent for every field the Spec did not enable. Passing a value for a
 * field the Spec left disabled is rejected exactly as an out-of-range value
 * would be. */
typedef struct mimhid_touchscreen_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimhid_touchscreen_extra;

/* Optional fields for mimhid_touchpad_contact(), with the same rules as
 * mimhid_touchscreen_extra. */
typedef struct mimhid_touchpad_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} mimhid_touchpad_extra;

typedef struct mimhid_pen_sample {
    uint32_t in_range;
    uint32_t tip;
    /* Selects the inverted/rubber end through HUT Invert (0D/3C). Tip may be
     * one at the same time to report rubber-end contact. */
    uint32_t eraser;
    uint32_t barrel_buttons;
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} mimhid_pen_sample;

/* The sample of mimhid_digitizer_update(): the same fields as mimhid_pen_sample, for
 * a Digitizer Node (Digitizers / Digitizer, indirect). */
typedef struct mimhid_digitizer_sample {
    uint32_t in_range;
    uint32_t tip;
    /* Selects the inverted/rubber end through HUT Invert (0D/3C). Tip may be
     * one at the same time to report rubber-end contact. */
    uint32_t eraser;
    uint32_t barrel_buttons;
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} mimhid_digitizer_sample;

/* mimhid_last_error
 * Ownership: Returns a borrowed pointer to the calling thread's record; its
 * strings and the record remain valid only until that thread's next public call.
 * Blocking: Does not block, allocate, perform I/O, or alter the record.
 * Synchronization: The record is thread-local; do not transfer its pointer to
 * another thread. Internal-thread completions retain diagnostics elsewhere.
 * Returns: Always returns a nonnull pointer. An unexpected ABI-boundary failure
 * returns the same pointer after replacing the record with MIMHID_ERR_INTERNAL. */
MIMHID_API const mimhid_error_detail* MIMHID_CALL mimhid_last_error(void);

/* mimhid_result_name
 * Ownership: Returns a borrowed, process-lifetime string; the caller frees nothing.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Has no handle domain and is safe on concurrent threads; only
 * the calling thread's diagnostic record is changed.
 * Returns: A known constant's name and a clear diagnostic, or the nonnull
 * sentinel "MIMHID_RESULT_UNKNOWN" with MIMHID_ERR_PARAM in last_error. An
 * unexpected failure returns "MIMHID_ERR_INTERNAL" and records that code. */
MIMHID_API const char* MIMHID_CALL mimhid_result_name(mimhid_result result);

/* mimhid_version
 * Ownership: Returns a value and creates no owned object.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Has no handle domain and is safe on concurrent threads; only
 * the calling thread's diagnostic record is changed.
 * Returns: The packed major/minor/patch value on success. Zero is the failure
 * sentinel and is accompanied by MIMHID_ERR_INTERNAL in last_error. */
MIMHID_API uint32_t MIMHID_CALL mimhid_version(void);

/* mimhid_discover
 * Ownership: On success, out_discovery receives a caller-owned snapshot containing every
 * returned info. A nonnull output is set to null before validation and stays null on failure.
 * Blocking: Opens each candidate USB serial port, sends one ping, and waits up to the share of
 * timeout_ms it has been given (at least 50 ms) for a pong. Ports that do not answer are
 * skipped. It allocates the snapshot.
 * Synchronization: Safe on concurrent threads; the snapshot has its own lifetime.
 * Returns: MIMHID_OK, including for an empty result; MIMHID_ERR_PARAM for a null output or
 * zero timeout; MIMHID_ERR_INTERNAL for allocation or unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_discover(uint32_t timeout_ms,
                                                     mimhid_discovery** out_discovery);

/* mimhid_discovery_count
 * Ownership: Borrows an immutable Discovery and creates no reference.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Concurrent read-only snapshot access is allowed; the caller
 * must serialize this call against mimhid_discovery_destroy.
 * Returns: The entry count. Zero is both a valid empty count and the failure
 * sentinel; last_error is MIMHID_OK for an empty result, MIMHID_ERR_PARAM for a
 * null handle, or MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API size_t MIMHID_CALL mimhid_discovery_count(const mimhid_discovery* discovery);

/* mimhid_discovery_get
 * Ownership: Returns a borrowed pointer that remains valid only until the owning Discovery is
 * destroyed; the caller frees nothing.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Concurrent read-only snapshot access is allowed; the caller
 * must serialize this call against mimhid_discovery_destroy.
 * Returns: The requested entry, or null as the failure sentinel with
 * MIMHID_ERR_PARAM for a null handle/out-of-range index or MIMHID_ERR_INTERNAL
 * for an unexpected ABI-boundary failure in last_error. */
MIMHID_API const mimhid_device_info* MIMHID_CALL
mimhid_discovery_get(const mimhid_discovery* discovery, size_t index);

/* mimhid_discovery_destroy
 * Ownership: Consumes the Discovery and invalidates all borrowed entries. The pointer becomes
 * invalid and must never be passed again. A null argument consumes nothing.
 * Blocking: Does not perform I/O; deallocation may run.
 * Synchronization: The caller must serialize destruction against all snapshot readers.
 * Returns: Void. Success clears last_error; a null handle records MIMHID_ERR_PARAM. */
MIMHID_API void MIMHID_CALL mimhid_discovery_destroy(mimhid_discovery* discovery);

/* mimhid_device_open
 * Ownership: Borrows port_path and options for the call; it copies retained values and
 * normalizes documented zero-valued tuning fields only in that copy, never in caller storage.
 * On success, out_device receives one Device owned by the caller. A nonnull output stays null
 * on failure.
 * Blocking: Opens the serial port at options->baud_rate and starts the reader thread.
 * It sends no ping or configuration: the board's USB device stays disconnected from the
 * target until nodes are opened and committed. port_path is the serial device path
 * (e.g., "/dev/ttyACM0" or "COM3").
 * Synchronization: Safe on concurrent threads for different ports.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields/identity;
 * MIMHID_ERR_UNSET_FIELD for a missing vendor_id or required product policy;
 * MIMHID_ERR_UNSUPPORTED for an unsupported baud rate; MIMHID_ERR_ACCESS, MIMHID_ERR_BUSY,
 * or MIMHID_ERR_IO for a port that cannot be opened or configured;
 * MIMHID_ERR_INTERNAL for allocation or unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_open(const char* port_path,
                                                        const mimhid_device_options* options,
                                                        mimhid_device** out_device);

/* mimhid_device_close
 * Ownership: A null call consumes nothing. The first valid call consumes Device and every
 * child Node for every returned result; all those pointers become invalid and must never be
 * passed again.
 * Blocking: Sends neutral reports for every open node (bounded by close_drain_timeout_ms), then
 * detaches the board's USB device from the target unless options->keep_attached is 1, and closes
 * the serial port.
 * Synchronization: Must be serialized against every Device and Node call.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for null or for a Device already closing;
 * MIMHID_ERR_TIMEOUT, MIMHID_ERR_NO_DEVICE, MIMHID_ERR_IO, or MIMHID_ERR_SHORT_TRANSFER
 * for a retained neutral-report failure after ownership is consumed. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_close(mimhid_device* device);

/* mimhid_device_latched_error
 * Ownership: Borrows a live Device and consumes the reported non-NO_DEVICE latch;
 * MIMHID_ERR_NO_DEVICE remains sticky. No handle ownership changes.
 * Blocking: Does not perform I/O or wait.
 * Synchronization: Serialize application calls; the reader thread may publish the latch
 * concurrently.
 * Returns: MIMHID_OK when no error is pending; MIMHID_ERR_PARAM for an invalid Device;
 * MIMHID_ERR_NO_DEVICE after the serial port failed or was unplugged; MIMHID_ERR_IO for
 * another retained link error. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_latched_error(mimhid_device* device);

/* mimhid_device_is_ready
 * Ownership: Borrows Device.
 * Blocking: Does not block.
 * Returns: 1 while the target has mounted the board's USB device and accepts reports; 0 otherwise. */
MIMHID_API int32_t MIMHID_CALL mimhid_device_is_ready(const mimhid_device* device);

/* mimhid_device_wait_ready
 * Ownership: Borrows Device.
 * Blocking: Waits until the target mounts the USB device or timeout_ms passes.
 * Returns: MIMHID_OK once mounted; MIMHID_ERR_TIMEOUT otherwise; MIMHID_ERR_NO_DEVICE if the
 * link fails meanwhile; MIMHID_ERR_PARAM for a null Device. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_wait_ready(mimhid_device* device,
                                                              uint32_t timeout_ms);

/* mimhid_device_batch_begin / mimhid_device_batch_end
 * Ownership: Borrows a live Device; no handle ownership changes.
 * Blocking: begin does not re-enumerate. end performs the staged commit with one
 * re-enumeration, as documented for mimhid_node_open.
 * Opening or closing a node re-enumerates the board's USB device, which the target sees as an
 * unplug and replug. Between begin and end, mimhid_node_open and mimhid_node_close only
 * stage their change; end applies all of them with one re-enumeration. A node opened inside a
 * batch rejects reports with MIMHID_ERR_BUSY until end succeeds.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null Device; MIMHID_ERR_BUSY for begin inside
 * a batch or end outside one; end also returns the errors of the commit it performs. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_batch_begin(mimhid_device* device);
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_batch_end(mimhid_device* device);

/* mimhid_device_remote_wakeup
 * Ownership: Borrows a live Device; no handle ownership changes.
 * Blocking: Waits for the board's answer (up to control_timeout_ms).
 * Returns: MIMHID_OK when the board signaled remote wakeup; MIMHID_ERR_BUSY when the target
 * has not suspended the device; MIMHID_ERR_PARAM for a null Device. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_remote_wakeup(mimhid_device* device);

/* mimhid_device_board_info
 * Ownership: Borrows Device and fills caller-owned info; no handle ownership changes.
 * Blocking: Does not perform I/O; the copy only.
 * Copies what the board reported at open (version, limits) into out_info.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device or null out_info;
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_device_board_info(const mimhid_device* device,
                                                              mimhid_device_info* out_info);

/* mimhid_device_commit
 * Ownership: Borrows live Device.
 * Blocking: Re-sends the configuration of every open node and re-enumerates the board's USB
 * device. mimhid_node_open, mimhid_node_open_raw, mimhid_node_close, and mimhid_device_batch_end
 * already do this, so the call is only needed to choose reconnect_delay_ms: with a nonzero
 * delay on an attached device the board holds D+=0V for that many milliseconds before it
 * re-asserts D+=3.3V. Inside a batch it applies the staged nodes and ends the batch.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device;
 * MIMHID_ERR_TIMEOUT, MIMHID_ERR_NO_DEVICE, or MIMHID_ERR_IO on communication failure. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_device_commit(mimhid_device* device, uint32_t reconnect_delay_ms);

/* mimhid_device_connect
 * Ownership: Borrows a live Device; no handle ownership changes.
 * Blocking: Waits for the board's acknowledgement. If no node is configured it performs the
 * full commit instead of only a connect.
 * Commands the board to physically connect to the USB host (D+=3.3V pull-up active).
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_TIMEOUT,
 * MIMHID_ERR_NO_DEVICE, or MIMHID_ERR_IO on communication failure; plus commit errors
 * when the Device is not yet configured. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_device_connect(mimhid_device* device);

/* mimhid_device_disconnect
 * Ownership: Borrows a live Device and marks it unconfigured on success.
 * Blocking: Waits for the board's acknowledgement.
 * Commands the board to physically disconnect from the USB host (D+=0V stealth/silent).
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Device; MIMHID_ERR_TIMEOUT,
 * MIMHID_ERR_NO_DEVICE, or MIMHID_ERR_IO on communication failure. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_device_disconnect(mimhid_device* device);

/* mimhid_spec_create_keyboard
 * Ownership: Borrows options for the call and copies them. On success, out_spec
 * receives one caller-owned immutable reference; it is null on every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; out_spec and
 * the calling thread's diagnostic record are exclusive to the caller.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields/ranges;
 * MIMHID_ERR_OVERFLOW for descriptor/layout size overflow; MIMHID_ERR_INTERNAL
 * for allocation, generation inconsistency, or unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_keyboard(const mimhid_keyboard_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_mouse
 * Ownership: Borrows options for the call and copies them. On success, out_spec
 * receives one caller-owned immutable reference; it is null on every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; out_spec and
 * the calling thread's diagnostic record are exclusive to the caller.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields, flags,
 * ranges, or disabled fields; MIMHID_ERR_OVERFLOW for descriptor/layout size
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or an
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_mouse(const mimhid_mouse_options* options,
                                                              mimhid_spec** out_spec);

/* mimhid_spec_create_toggle
 * Ownership: Borrows options and all parallel arrays/strings for the call and
 * copies them. On success, out_spec receives one caller-owned immutable reference;
 * it is null on failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; caller-owned
 * input/output storage must not be concurrently mutated.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields/usages,
 * including a Camera Control field Usage outside Auto-focus/Shutter;
 * MIMHID_ERR_UNSET_FIELD for a missing Application Collection, field Usage
 * Page, allow-list, semantic, or evidence array; MIMHID_ERR_UNSUPPORTED for a
 * semantic press/release cannot represent, or for a Camera Control field
 * Usage not declared One Shot;
 * MIMHID_ERR_OVERFLOW for count/descriptor/layout overflow; MIMHID_ERR_INTERNAL
 * for allocation, generation inconsistency, or unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_toggle(const mimhid_toggle_options* options,
                                                               mimhid_spec** out_spec);

/* mimhid_spec_create_gamepad
 * Ownership: Borrows options, axes, and evidence strings for the call and copies
 * them. On success, out_spec receives one caller-owned immutable reference; it is
 * null on failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; caller-owned
 * input/output storage must not be concurrently mutated.
 * Always the Generic Desktop / Game Pad Application Collection.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields, axis,
 * button, or D-pad declarations; MIMHID_ERR_UNSET_FIELD for missing
 * axis width/evidence; MIMHID_ERR_OVERFLOW for count/Usage/descriptor/layout
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or an
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_gamepad(const mimhid_gamepad_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_touchscreen
 * Ownership: Borrows and copies options. On success, out_spec receives one
 * caller-owned immutable reference; it is null on every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; out_spec and
 * caller-owned options must not be concurrently mutated.
 * Always the fixed-slot Multi-Touch Touch Screen Application Collection.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields, range,
 * count, or flag; MIMHID_ERR_OVERFLOW for button, descriptor, or layout
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_touchscreen(const mimhid_touchscreen_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_touchpad
 * Ownership: Borrows and copies options. On success, out_spec receives one
 * caller-owned immutable reference; it is null on every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; out_spec and
 * caller-owned options must not be concurrently mutated.
 * Always the Digitizers / Touch Pad Application Collection. button_count may
 * be zero (buttonless clickpad).
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields, range,
 * count, or flag; MIMHID_ERR_OVERFLOW for button, descriptor, or layout
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_touchpad(const mimhid_touchpad_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_pen
 * Ownership: Borrows options and barrel_usages for the call and copies them. On
 * success, out_spec receives one caller-owned immutable reference; it is null on
 * every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; caller-owned
 * input/output storage must not be concurrently mutated.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields,
 * fields, flags, or barrel usages; MIMHID_ERR_OVERFLOW for descriptor/layout
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or an
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_pen(const mimhid_pen_options* options,
                                                            mimhid_spec** out_spec);

/* mimhid_spec_create_digitizer
 * Ownership: Borrows options and barrel_usages for the call and copies them. On
 * success, out_spec receives one caller-owned immutable reference; it is null on
 * every failure.
 * Blocking: Performs validation and allocation but no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; caller-owned
 * input/output storage must not be concurrently mutated.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields,
 * fields, flags, or barrel usages; MIMHID_ERR_OVERFLOW for descriptor/layout
 * overflow; MIMHID_ERR_INTERNAL for allocation, generation inconsistency, or an
 * unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_spec_create_digitizer(const mimhid_digitizer_options* options, mimhid_spec** out_spec);

/* mimhid_spec_create_raw
 * Ownership: Borrows descriptor and report table for the call and copies them.
 * On success, out_spec receives one caller-owned immutable reference; it is null
 * on every failure.
 * Blocking: Parses/validates and allocates but performs no I/O or waiting.
 * Synchronization: Has no Device domain and may run concurrently; caller-owned
 * input/output storage must not be concurrently mutated.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid pointers/ABI fields or HID
 * encoding/report-table mismatch; MIMHID_ERR_UNSET_FIELD for a missing descriptor
 * or report table; MIMHID_ERR_UNSUPPORTED for Output/Feature dependency, missing
 * acknowledgement, long items, or unsupported raw collection/range constructs;
 * MIMHID_ERR_OVERFLOW for descriptor/report/field/bit-count overflow;
 * MIMHID_ERR_INTERNAL for allocation or unexpected exception. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_create_raw(const mimhid_raw_options* options,
                                                            mimhid_spec** out_spec);

/* mimhid_spec_retain
 * Ownership: Requires the caller already to own a live reference. Adds one
 * caller-owned reference on success; the caller must later release it. A null or
 * saturated-count failure adds no reference.
 * Blocking: Does not allocate, perform I/O, or wait; it uses an atomic counter.
 * Synchronization: May run concurrently while the caller already owns a live
 * reference; never race a last release with a retain lacking its own reference.
 * Returns: Void. Success clears last_error; null records MIMHID_ERR_PARAM,
 * saturated reference count records MIMHID_ERR_OVERFLOW, and an unexpected
 * ABI-boundary failure records MIMHID_ERR_INTERNAL. */
MIMHID_API void MIMHID_CALL mimhid_spec_retain(mimhid_spec* spec);

/* mimhid_spec_release
 * Ownership: Requires and consumes exactly one caller-owned live reference.
 * When it was the final reference, Spec is destroyed and the pointer becomes
 * invalid and must never be passed again. A null call consumes nothing.
 * Blocking: Performs no I/O or waiting; final deallocation may run.
 * Synchronization: Atomic reference release may be concurrent when each caller
 * owns a distinct reference; final release must be serialized against all access.
 * Returns: Void. Success clears last_error; null records MIMHID_ERR_PARAM, and an
 * unexpected ABI-boundary failure records MIMHID_ERR_INTERNAL. A missing owned
 * reference is a caller lifetime violation, not a safely detectable result. */
MIMHID_API void MIMHID_CALL mimhid_spec_release(mimhid_spec* spec);

/* mimhid_spec_descriptor
 * Ownership: Borrows Spec and returns a borrowed immutable byte span valid while
 * that Spec reference remains alive; nonnull outputs are reset on failure.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Concurrent reads of immutable Spec are allowed while every
 * reader owns a reference; serialize against final release.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null Spec/bytes/length pointer;
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_descriptor(const mimhid_spec* spec,
                                                            const uint8_t** bytes, size_t* length);

/* mimhid_spec_manifest
 * Ownership: Borrows Spec, fills caller-owned manifest storage, and returns
 * borrowed report-capability storage valid while Spec remains alive.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Concurrent reads of immutable Spec are allowed while every
 * reader owns a reference; each caller supplies distinct manifest storage.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for a null pointer, wrong manifest
 * struct_size, or nonzero reserved field; MIMHID_ERR_INTERNAL for an unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_spec_manifest(const mimhid_spec* spec,
                                                          mimhid_capability_manifest* manifest);

/* mimhid_node_open
 * Ownership: Borrows Device and options, retains Spec, and on success returns one Node owned
 * by the caller and Device. out_node is null on every failure.
 * Blocking: Stages the new node on the explicit USB interface specified by
 * options->interface_number, sends the configuration to the board, and commits it.
 * The board then drops its USB device from the
 * target and re-enumerates it with the new set of nodes, so the target sees an unplug and
 * replug and every other node is briefly unavailable. Inside mimhid_device_batch_begin the
 * call only stages. It returns once the board re-enumerates; use mimhid_device_wait_ready to
 * wait for the target to mount the device.
 * Synchronization: Belongs to the parent Device domain and must be serialized with all Device
 * and Node application calls.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid handles/options/settings;
 * MIMHID_ERR_UNSUPPORTED for a nonzero boot_protocol on a non-Raw Spec;
 * MIMHID_ERR_OVERFLOW when the board has no free node, interface, or descriptor space, when a
 * report exceeds the board's or the caller's limit, or when a descriptor exceeds a parser
 * policy; MIMHID_ERR_DESCRIPTOR_REJECTED when the board refuses the configuration;
 * MIMHID_ERR_TIMEOUT when the board or target does not answer in time;
 * MIMHID_ERR_NO_DEVICE, MIMHID_ERR_IO, or MIMHID_ERR_SHORT_TRANSFER for a serial failure;
 * MIMHID_ERR_INTERNAL for allocation, state initialization, or unexpected failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_open(mimhid_device* device, mimhid_spec* spec,
                                                      const mimhid_node_options* options,
                                                      mimhid_node** out_node);

/* mimhid_node_open_raw
 * Ownership: Borrows Device, descriptor bytes, and options. Returns one Node owned by
 * the caller and Device.
 * Blocking: Stages the raw descriptor directly without parsing or validation, assigning
 * it to a USB interface. Commits immediately unless inside a batch.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid handles/pointers;
 * MIMHID_ERR_OVERFLOW when board node or descriptor limits are exceeded;
 * MIMHID_ERR_INTERNAL for allocation failure. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_node_open_raw(mimhid_device* device,
                      const uint8_t* descriptor,
                      size_t descriptor_length,
                      const mimhid_node_options* options,
                      mimhid_node** out_node);

/* mimhid_node_close
 * Ownership: Only MIMHID_OK consumes Node and its retained Spec reference. Every error leaves
 * Node live and caller-owned for a retry or a later Device close. After MIMHID_OK the Node
 * pointer is invalid and must never be passed again.
 * Blocking: Waits up to the configured close drain budget, finishes any touch frame, sends the
 * neutral reports, then commits the remaining nodes (a re-enumeration, as in mimhid_node_open;
 * the last node's close detaches the USB device). Inside a batch it only stages the removal.
 * Synchronization: Belongs to the parent Device domain and must be serialized against all
 * Device and Node calls; never call it from the event sink.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for null or for a still-live closed Node;
 * MIMHID_ERR_TIMEOUT, MIMHID_ERR_NO_DEVICE, MIMHID_ERR_IO, MIMHID_ERR_SHORT_TRANSFER,
 * MIMHID_ERR_DESCRIPTOR_REJECTED, or MIMHID_ERR_OVERFLOW for a neutral-report or commit
 * failure; MIMHID_ERR_INTERNAL for an invariant, allocation, or unexpected failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_close(mimhid_node* node);

/* mimhid_node_id
 * Ownership: Borrows a live Node and creates no reference.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Belongs to the parent Device domain and must be serialized against
 * Node/Device teardown.
 * Returns: The node id the board knows the Node by, plus one (so it is nonzero), on success.
 * Zero is the failure sentinel with MIMHID_ERR_PARAM for a null/closed Node or
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure in last_error. */
MIMHID_API uint16_t MIMHID_CALL mimhid_node_id(const mimhid_node* node);

/* mimhid_node_set_feature_response
 * Sets the bytes (without the Report ID) the board returns when the target reads Feature
 * report report_id of this Node (GET_REPORT). A touch Spec with
 * enable_contact_count_maximum_feature_declaration gets its answer automatically.
 * Ownership: Borrows Node; copies data.
 * Blocking: Waits for the board's acknowledgement unless a batch is open or the Node is staged.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node or more than 32 bytes;
 * MIMHID_ERR_TIMEOUT, MIMHID_ERR_NO_DEVICE, MIMHID_ERR_IO, or MIMHID_ERR_OVERFLOW for a
 * board or link failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_set_feature_response(mimhid_node* node,
                                                                      uint8_t report_id,
                                                                      const uint8_t* data,
                                                                      size_t length);

/* mimhid_node_manifest
 * Ownership: Borrows Node, fills caller-owned manifest storage, and returns a
 * borrowed report array valid until Node closes or its retained Spec is released.
 * Blocking: Does not block, allocate, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize against Node
 * or Device teardown and use distinct manifest storage per caller.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node/manifest, wrong
 * struct_size, or nonzero reserved field; MIMHID_ERR_INTERNAL for an unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_manifest(const mimhid_node* node,
                                                          mimhid_capability_manifest* manifest);

/* mimhid_node_submit
 * Ownership: Borrows Node; the library serializes the report into its own buffer, so no caller
 * buffer ownership changes.
 * Blocking: Writes one report to the serial port and returns once the bytes are handed to the
 * driver (a few microseconds on an idle port). It never waits for the target.
 * Synchronization: Belongs to the parent Device domain. Callers serialize state and submit
 * calls.
 * Returns: MIMHID_OK for clean state or an accepted report; MIMHID_ERR_PARAM for an invalid
 * Node; MIMHID_ERR_UNSUPPORTED for a Raw Node; MIMHID_ERR_BUSY while a batch is open or the
 * board is re-enumerating; MIMHID_ERR_NO_DEVICE,
 * MIMHID_ERR_IO, MIMHID_ERR_SHORT_TRANSFER, or MIMHID_ERR_TIMEOUT for a serial failure;
 * MIMHID_ERR_INTERNAL for serializer, validation, allocation, or unexpected failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_submit(mimhid_node* node);

/* mimhid_node_submit_blocking
 * Ownership: Borrows Node and changes no handle ownership.
 * Blocking: Sends every pending report fragment (a touch frame can take several reports) or
 * stops at the timeout.
 * Synchronization: Belongs to the parent Device domain and must be serialized against all
 * Device and Node application calls.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node; MIMHID_ERR_UNSET_FIELD for a
 * zero timeout; MIMHID_ERR_TIMEOUT when the timeout expires; and the errors of
 * mimhid_node_submit. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_node_submit_blocking(mimhid_node* node,
                                                                 uint32_t timeout_ms);

/* mimhid_keyboard_key
 * Ownership: Borrows a Keyboard Node; no ownership changes.
 * Blocking: Does not block, allocate after Node open, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * down must be exactly zero or one: one presses usage, matching the prior
 * key_down; zero releases it, matching the prior key_up. There is no
 * release-all call; release every usage the caller pressed before closing the
 * Node, or the target may see it as stuck.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid profile/Usage/Node/down;
 * MIMHID_ERR_BUSY for an in-flight report or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky device loss; MIMHID_ERR_INTERNAL for an
 * unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_keyboard_key(mimhid_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimhid_mouse_move
 * Ownership: Borrows a Mouse Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid profile/Node;
 * MIMHID_ERR_BUSY for an in-flight report; MIMHID_ERR_NO_DEVICE for sticky
 * device loss; MIMHID_ERR_OVERFLOW if accumulated int64_t deltas overflow;
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_move(mimhid_node* node, int32_t dx, int32_t dy);

/* mimhid_mouse_scroll
 * Ownership: Borrows a Mouse Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid profile/Node;
 * MIMHID_ERR_UNSUPPORTED when a requested axis is absent; MIMHID_ERR_BUSY for an
 * in-flight report; MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_OVERFLOW if
 * accumulated int64_t scroll deltas overflow; MIMHID_ERR_INTERNAL for an
 * unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_scroll(mimhid_node* node, int32_t wheel,
                                                         int32_t pan);

/* mimhid_mouse_button
 * Ownership: Borrows a Mouse Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile, one-based button,
 * or down flag; MIMHID_ERR_BUSY for an in-flight report or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for an unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_mouse_button(mimhid_node* node, uint32_t button,
                                                         uint32_t down);

/* mimhid_toggle_usage
 * Ownership: Borrows a Toggle Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * down must be exactly zero or one: one presses usage and zero releases it.
 * usage must be in the Spec allow-list. Any number of Usages may be pressed at
 * once; each is its own bit. There is no release-all call; release every Usage
 * you pressed before closing the Node (close does clear them all).
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile/down or a
 * Usage outside the Spec allow-list; MIMHID_ERR_BUSY for an in-flight report or an unsent opposite
 * edge; MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for an unexpected ABI-boundary
 * failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_toggle_usage(mimhid_node* node, uint16_t usage,
                                                         uint32_t down);

/* mimhid_gamepad_button
 * Ownership: Borrows a Gamepad Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile, one-based button,
 * or down flag; MIMHID_ERR_BUSY for an in-flight report or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for an unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_button(mimhid_node* node, uint32_t button,
                                                           uint32_t down);

/* mimhid_gamepad_axis
 * Ownership: Borrows a Gamepad Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node/profile/index/value;
 * MIMHID_ERR_BUSY for an in-flight report; MIMHID_ERR_NO_DEVICE for sticky loss;
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_axis(mimhid_node* node, size_t axis_index,
                                                         int32_t value);

/* mimhid_gamepad_dpad
 * Ownership: Borrows a Gamepad Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Derives either four independent D-pad OOC fields or the canonical eight-way
 * Hat value, matching whichever mimhid_dpad_representation the Spec selected.
 * Hat mode rejects opposite pairs and no direction emits Null State.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM unless the Spec selects a D-pad and all
 * four values form a valid boolean direction; MIMHID_ERR_BUSY for an in-flight
 * report or an unsent opposite edge; MIMHID_ERR_NO_DEVICE for sticky loss;
 * MIMHID_ERR_INTERNAL for an unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_gamepad_dpad(mimhid_node* node, uint32_t up,
                                                         uint32_t down, uint32_t right,
                                                         uint32_t left);

/* mimhid_touchscreen_contact
 * Ownership: Borrows a Touchscreen Node and extra for the call;
 * it copies the sample and changes no ownership.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected during an in-flight/multi-packet frame.
 * down must be exactly zero or one. One with a contact_id not currently active
 * places it in a new slot, matching the prior touch_down; one with a
 * contact_id already active moves it, matching the prior touch_move; zero
 * lifts an active contact_id at the given final x/y, matching the prior
 * touch_up. extra may be null, which sends zero/absent for every field the
 * Spec did not enable; a non-null field the Spec left disabled is rejected.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile/down/contact
 * values, or an inactive contact_id on lift/move; MIMHID_ERR_BUSY for an
 * in-flight frame, an unsent opposite edge, or a duplicate active ID on
 * placement; MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_OVERFLOW when no
 * declared contact slot remains; MIMHID_ERR_INTERNAL for unexpected ABI-boundary
 * failure. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_touchscreen_contact(mimhid_node* node, uint32_t contact_id, uint32_t down, int32_t x,
                           int32_t y, const mimhid_touchscreen_extra* extra);

/* mimhid_touchpad_contact
 * Ownership: Borrows a Touchpad Node and extra for the call;
 * it copies the sample and changes no ownership.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected during an in-flight/multi-packet frame.
 * down must be exactly zero or one. One with a contact_id not currently active
 * places it in a new slot, matching the prior touch_down; one with a
 * contact_id already active moves it, matching the prior touch_move; zero
 * lifts an active contact_id at the given final x/y, matching the prior
 * touch_up. extra may be null, which sends zero/absent for every field the
 * Spec did not enable; a non-null field the Spec left disabled is rejected.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile/down/contact
 * values, or an inactive contact_id on lift/move; MIMHID_ERR_BUSY for an
 * in-flight frame, an unsent opposite edge, or a duplicate active ID on
 * placement; MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_OVERFLOW when no
 * declared contact slot remains; MIMHID_ERR_INTERNAL for unexpected ABI-boundary
 * failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_touchpad_contact(mimhid_node* node, uint32_t contact_id,
                                                             uint32_t down, int32_t x, int32_t y,
                                                             const mimhid_touchpad_extra* extra);

/* mimhid_touchpad_button
 * Ownership: Borrows exactly a Touchpad Node; no ownership changes.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected during an in-flight/multi-packet frame.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile, one-based button,
 * or down flag; MIMHID_ERR_BUSY for an in-flight frame or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_touchpad_button(mimhid_node* node, uint32_t button,
                                                            uint32_t down);

/* mimhid_pen_update
 * Ownership: Borrows a Pen Node and sample for the call; it copies the sample and
 * changes no ownership.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * A sample with in_range 0 takes the tool away: the report carries In Range,
 * Tip, pressure, Invert, and the barrel bits as 0 and the sample's own X/Y, tilt,
 * and twist, so repeat the last position to leave the pointer where it was.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile/sample flags,
 * buttons, or values; MIMHID_ERR_BUSY for an in-flight report or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_pen_update(mimhid_node* node,
                                                       const mimhid_pen_sample* sample);

/* mimhid_digitizer_update
 * Ownership: Borrows a Digitizer Node and sample for the call; it copies the sample and
 * changes no ownership.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * A sample with in_range 0 takes the tool away: the report carries In Range,
 * Tip, pressure, Invert, and the barrel bits as 0 and the sample's own X/Y, tilt,
 * and twist, so repeat the last position to leave the pointer where it was.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for invalid Node/profile/sample flags,
 * buttons, or values; MIMHID_ERR_BUSY for an in-flight report or an unsent opposite edge;
 * MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for unexpected
 * ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_digitizer_update(mimhid_node* node,
                                                             const mimhid_digitizer_sample* sample);

/* mimhid_battery_update
 * Ownership: Borrows a Node whose Spec was created with enable_battery; no
 * ownership changes. The report is sent by the next mimhid_node_submit that
 * has no input report to send, and only when the value changed.
 * Blocking: Does not block, allocate, log, or perform I/O.
 * Synchronization: Belongs to the parent Device domain; serialize mutation and
 * submission calls. It is rejected while one report is in flight.
 * Returns: MIMHID_OK; MIMHID_ERR_PARAM for an invalid Node, a Node without a
 * battery report, or an out-of-range strength; MIMHID_ERR_BUSY for an in-flight
 * report; MIMHID_ERR_NO_DEVICE for sticky loss; MIMHID_ERR_INTERNAL for
 * unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_battery_update(mimhid_node* node, int32_t strength);

/* mimhid_raw_submit
 * Ownership: Borrows a Raw Node and report; no ownership changes.
 * Blocking: Validates the report and writes the framed report to the serial port
 * without waiting for the target USB host to poll the endpoint.
 * Synchronization: Belongs to the parent Device domain; serialize raw submissions
 * with other Device and Node calls on the same Device.
 * Returns: MIMHID_OK for accepted submission; MIMHID_ERR_PARAM for invalid
 * Node/profile/buffer, unaccepted ID/length, or nonzero trailing padding;
 * MIMHID_ERR_UNSUPPORTED, MIMHID_ERR_ACCESS, MIMHID_ERR_BUSY,
 * MIMHID_ERR_NO_DEVICE, MIMHID_ERR_TIMEOUT,
 * MIMHID_ERR_SHORT_TRANSFER, MIMHID_ERR_IO, or MIMHID_ERR_OVERFLOW for deferred,
 * prior-completion, pool, or submit failure; MIMHID_ERR_INTERNAL for allocation
 * or unexpected ABI-boundary failure. */
MIMHID_API mimhid_result MIMHID_CALL mimhid_raw_submit(mimhid_node* node, const uint8_t* report,
                                                       size_t length);

/* mimhid_report_send
 * Ownership: Borrows Node and report_bytes; no ownership changes.
 * Blocking: Directly writes raw report bytes to the serial link for this node's ID.
 * Intermediate copy free.
 * Returns: MIMHID_OK on success; MIMHID_ERR_PARAM for invalid handles/buffers;
 * MIMHID_ERR_TIMEOUT, MIMHID_ERR_NO_DEVICE, MIMHID_ERR_SHORT_TRANSFER, or
 * MIMHID_ERR_IO on transport failure. */
MIMHID_API mimhid_result MIMHID_CALL
mimhid_report_send(mimhid_node* node, const uint8_t* report_bytes, size_t report_length);

#ifdef __cplusplus
}
#endif

#endif
