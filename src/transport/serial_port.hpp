// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The operating-system serial port, below the wire protocol. One implementation per platform
 * (serial_posix.cpp, serial_win32.cpp); everything above it is platform independent. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mimhid::transport {

// Where a failed open stopped, for diagnostics.
enum class OpenStep : std::uint8_t { none, open, configure, speed, unsupported_baud };

struct OpenResult {
    bool ok{};
    OpenStep step{OpenStep::none};
    std::int32_t os_error{};
};

enum class IoStatus : std::uint8_t { ok, timeout, short_write, closed, error };

class SerialPort final {
  public:
    SerialPort() noexcept;
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    static bool baud_supported(std::uint32_t baud) noexcept;

    // Opens a raw 8N1 port without flow control.
    OpenResult open(const char* path, std::uint32_t baud) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // Writes every byte or reports why not. A write that makes no progress for stall_ms fails
    // with short_write when some bytes went out and timeout when none did.
    IoStatus write_all(const std::uint8_t* data, std::size_t length, std::uint32_t stall_ms,
                       std::int32_t* os_error) noexcept;

    // Reads up to capacity bytes, waiting at most timeout_ms for the first one. Returns the
    // count (zero on timeout), or -1 when the port failed or was unplugged.
    long read(std::uint8_t* buffer, std::size_t capacity, std::uint32_t timeout_ms) noexcept;

    // Waits until queued output has left, then discards unread input.
    void drain() noexcept;
    void discard_input() noexcept;

  private:
    std::intptr_t handle_;
#ifdef _WIN32
    // Windows keeps one event per direction for overlapped I/O.
    void* read_event_;
    void* write_event_;
    std::uint32_t current_read_timeout_ = 50;
    std::uint32_t current_write_stall_ = 500;
#endif
};

struct PortCandidate {
    std::string path;
    std::string description;
};

// USB serial ports that may hold a board.
std::vector<PortCandidate> list_candidate_ports();

} // namespace mimhid::transport
