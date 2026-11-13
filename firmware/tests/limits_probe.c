// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// Reads config.h and nothing else: built without the port's mimusb_limits.h, or with one that leaves
// a limit out, it must not compile (config.h says which).
#include "config.h"

int main(void) { return 0; }
