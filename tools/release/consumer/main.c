// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include <mimusb.h>

#include <stdio.h>

int main(void) {
    const uint32_t version = mimusb_version();
    printf("libmimusb %u.%u.%u (" MIMUSB_CONSUMER_COMPONENT ")\n", (unsigned)(version >> 24),
           (unsigned)((version >> 16) & 0xFF), (unsigned)(version & 0xFFFF));
    return mimusb_result_name(MIMUSB_OK) != NULL ? 0 : 1;
}
