// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Proves the public header and exported entry points are usable from C; USB
 * transport behavior is covered by the integration suite. */
#include "mimhid.h"

#include <stddef.h>
#include <stdint.h>

/* These assertions are part of the ABI gate, not platform-behaviour claims.
 * They remain true even when a consumer enables a short-enum compiler mode. */
_Static_assert(sizeof(mimhid_result) == sizeof(int32_t), "mimhid_result ABI width");
_Static_assert(sizeof(mimhid_event_kind) == sizeof(int32_t), "mimhid_event_kind ABI width");
_Static_assert(sizeof(mimhid_log_level) == sizeof(int32_t), "mimhid_log_level ABI width");
_Static_assert(sizeof(mimhid_profile_kind) == sizeof(int32_t), "mimhid_profile_kind ABI width");
_Static_assert(sizeof(mimhid_axis_role) == sizeof(int32_t), "mimhid_axis_role ABI width");
_Static_assert(sizeof(mimhid_dpad_representation) == sizeof(int32_t),
               "mimhid_dpad_representation ABI width");
_Static_assert(sizeof(mimhid_usage_semantic) == sizeof(int32_t), "mimhid_usage_semantic ABI width");

/* Structures without pointers have the same layout on every supported ABI (LP64, LLP64, ILP32).
 * Members that precede the first pointer have fixed offsets too. */
_Static_assert(sizeof(mimhid_physical_properties) == 20, "physical_properties layout");
_Static_assert(sizeof(mimhid_integer_field) == 32, "integer_field layout");
_Static_assert(sizeof(mimhid_report_id_option) == 8, "report_id_option layout");
_Static_assert(sizeof(mimhid_battery_report) == 40, "battery_report layout");
_Static_assert(sizeof(mimhid_device_info) == 168, "device_info layout");
_Static_assert(offsetof(mimhid_device_info, baud_rate) == 140, "device_info.baud_rate");
_Static_assert(offsetof(mimhid_device_info, unique_id) == 149, "device_info.unique_id");
_Static_assert(offsetof(mimhid_device_info, version_major) == 128, "device_info.version_major");

_Static_assert(sizeof(mimhid_usb_endpoint) == 4, "usb_endpoint layout");
_Static_assert(offsetof(mimhid_usb_endpoint, address) == 0, "usb_endpoint.address");
_Static_assert(offsetof(mimhid_usb_endpoint, poll_interval_ms) == 1, "usb_endpoint.poll_interval_ms");
_Static_assert(offsetof(mimhid_usb_endpoint, max_packet_size) == 2, "usb_endpoint.max_packet_size");

_Static_assert(offsetof(mimhid_usb_interface, number) == 0, "usb_interface.number");
_Static_assert(offsetof(mimhid_usb_interface, class_code) == 1, "usb_interface.class_code");
_Static_assert(offsetof(mimhid_usb_interface, bcd_hid) == 4, "usb_interface.bcd_hid");

_Static_assert(offsetof(mimhid_usb_config, configuration_value) == 0, "usb_config.configuration_value");
_Static_assert(offsetof(mimhid_usb_config, bm_attributes) == 1, "usb_config.bm_attributes");
_Static_assert(offsetof(mimhid_usb_config, bmax_power) == 2, "usb_config.bmax_power");

_Static_assert(offsetof(mimhid_usb_device, bcd_usb) == 0, "usb_device.bcd_usb");
_Static_assert(offsetof(mimhid_usb_device, vendor_id) == 6, "usb_device.vendor_id");
_Static_assert(offsetof(mimhid_usb_device, product_id) == 8, "usb_device.product_id");
_Static_assert(offsetof(mimhid_usb_device, bcd_device) == 10, "usb_device.bcd_device");

_Static_assert(offsetof(mimhid_node_config, struct_size) == 0, "node_config.struct_size");
_Static_assert(offsetof(mimhid_device_config, struct_size) == 0, "device_config.struct_size");
_Static_assert(offsetof(mimhid_commit_timeouts, struct_size) == 0, "commit_timeouts.struct_size");
_Static_assert(sizeof(mimhid_commit_timeouts) == 12, "commit_timeouts layout");

_Static_assert(offsetof(mimhid_event, kind) == 8, "event.kind");
_Static_assert(offsetof(mimhid_event, value) == 16, "event.value");

int main(void) {
    return MIMHID_OK == 0 && MIMHID_ERR_INTERNAL == 16 && MIMHID_ERR_PROTOCOL == 4 && MIMHID_USAGE_NAMED_ARRAY == 10 ? 0 : 1;
}
