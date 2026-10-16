// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <mimhid.h>

#include <stdio.h>

int main(void) {
    const uint32_t version = mimhid_version();
    printf("libmimhid %u.%u.%u\n", (unsigned)(version >> 24), (unsigned)((version >> 16) & 0xFF),
           (unsigned)(version & 0xFFFF));
    return mimhid_result_name(MIMHID_OK) != NULL ? 0 : 1;
}
