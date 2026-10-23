// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Checks that the C++ header forwards to the C ABI under namespace mimhid. */
#include "mimhid.hpp"

int main() {
    if (mimhid::version() != mimhid_version() || mimhid::result_name(MIMHID_OK) == nullptr ||
        mimhid::keyboard_key(nullptr, 0U, 0U) != MIMHID_ERR_PARAM ||
        mimhid::mouse_move(nullptr, 0, 0) != MIMHID_ERR_PARAM ||
        mimhid::node_submit(nullptr, 1U) != MIMHID_ERR_PARAM ||
        mimhid::discover(0U, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_open(nullptr, nullptr, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_commit(nullptr, 0U, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_connect(nullptr, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::device_disconnect(nullptr, 1U) != MIMHID_ERR_PARAM ||
        mimhid::node_open_raw(nullptr, nullptr, 0U, nullptr, nullptr, nullptr) != MIMHID_ERR_PARAM ||
        mimhid::report_send(nullptr, nullptr, 0U, 1U) != MIMHID_ERR_PARAM ||
        mimhid::last_error() != mimhid_last_error()) {
        return 1;
    }
    return 0;
}
