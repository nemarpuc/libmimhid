// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

#include <cstdint>

namespace mimusb::transport::detail {

#ifdef __linux__
// Sets the line rate of a tty to any value; *actual is what the driver reports afterwards.
bool linux_set_baud(int fd, std::uint32_t baud, std::uint32_t* actual) noexcept;
#endif

} // namespace mimusb::transport::detail
