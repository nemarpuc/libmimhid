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
_Static_assert(sizeof(mimhid_boot_protocol) == sizeof(int32_t), "mimhid_boot_protocol ABI width");
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
_Static_assert(offsetof(mimhid_device_options, keep_attached) == 72, "device_options.keep_attached");
_Static_assert(offsetof(mimhid_device_options, vendor_id) == 76, "device_options.vendor_id");
_Static_assert(offsetof(mimhid_device_options, bcd_device) == 80, "device_options.bcd_device");
_Static_assert(offsetof(mimhid_node_options, struct_size) == 0, "node_options.struct_size");
_Static_assert(offsetof(mimhid_node_options, interface_number) == 4, "node_options.interface_number");
_Static_assert(offsetof(mimhid_node_options, poll_interval_ms) == 8, "node_options.poll_interval_ms");
_Static_assert(offsetof(mimhid_node_options, ep_in_size) == 12, "node_options.ep_in_size");
_Static_assert(offsetof(mimhid_node_options, ep_out_size) == 16, "node_options.ep_out_size");
_Static_assert(offsetof(mimhid_node_options, boot_protocol) == 20, "node_options.boot_protocol");
_Static_assert(offsetof(mimhid_node_options, bcd_hid) == 24, "node_options.bcd_hid");
_Static_assert(offsetof(mimhid_node_options, country_code) == 26, "node_options.country_code");
_Static_assert(offsetof(mimhid_event, kind) == 8, "event.kind");
_Static_assert(offsetof(mimhid_event, value) == 16, "event.value");

int main(void) {
    return MIMHID_OK == 0 && MIMHID_ERR_INTERNAL == 16 && MIMHID_ERR_PROTOCOL == 4 && MIMHID_USAGE_NAMED_ARRAY == 10 ? 0 : 1;
}
