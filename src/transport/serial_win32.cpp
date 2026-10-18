// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Windows serial port: overlapped I/O on a \\.\COMn handle.
#include "transport/serial_port.hpp"

#include <cstring>
#include <string>
#include <windows.h>

namespace mimhid::transport {

namespace {

// COM10 and above only open through the \\.\ namespace; it is valid for every port.
std::string device_path(const char* port) {
    std::string name = port;
    if (name.rfind("\\\\.\\", 0) == 0)
        return name;
    return "\\\\.\\" + name;
}

bool set_read_timeout(HANDLE h, DWORD timeout_ms, DWORD write_stall_ms) noexcept {
    // MAXDWORD / MAXDWORD / N: a read returns at once when bytes are queued, otherwise it
    // waits up to N ms for the first byte, so the reader wakes on the first received byte.
    COMMTIMEOUTS to;
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant =
        timeout_ms == 0 ? 1 : (timeout_ms >= MAXDWORD ? MAXDWORD - 1 : timeout_ms);
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = write_stall_ms;
    return SetCommTimeouts(h, &to) != FALSE;
}

} // namespace

SerialPort::SerialPort() noexcept
    : handle_(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE)), read_event_(nullptr),
      write_event_(nullptr) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::baud_supported(std::uint32_t baud) noexcept {
    switch (baud) {
    case 115200:
    case 230400:
    case 460800:
    case 576000:
    case 921600:
    case 1000000:
    case 1500000:
    case 2000000:
    case 3000000:
    case 4000000:
        return true;
    default:
        return false;
    }
}

bool SerialPort::is_open() const noexcept {
    return handle_ != reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE);
}

OpenResult SerialPort::open(const char* path, std::uint32_t baud) noexcept {
    OpenResult result;
    if (!baud_supported(baud)) {
        result.step = OpenStep::unsupported_baud;
        return result;
    }
    HANDLE h = CreateFileA(device_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        result.step = OpenStep::open;
        result.os_error = static_cast<std::int32_t>(GetLastError());
        return result;
    }

    DCB dcb;
    std::memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    bool ok = GetCommState(h, &dcb) != FALSE;
    if (ok) {
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fAbortOnError = FALSE;
        ok = SetCommState(h, &dcb) != FALSE && set_read_timeout(h, 50, 500);
    }
    if (!ok) {
        result.step = OpenStep::configure;
        result.os_error = static_cast<std::int32_t>(GetLastError());
        CloseHandle(h);
        return result;
    }

    read_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (read_event_ == nullptr || write_event_ == nullptr) {
        result.step = OpenStep::configure;
        result.os_error = static_cast<std::int32_t>(GetLastError());
        if (read_event_)
            CloseHandle(read_event_);
        if (write_event_)
            CloseHandle(write_event_);
        read_event_ = write_event_ = nullptr;
        CloseHandle(h);
        return result;
    }

    SetupComm(h, 4096, 4096);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    handle_ = reinterpret_cast<std::intptr_t>(h);
    result.ok = true;
    return result;
}

bool SerialPort::set_baud(std::uint32_t baud) noexcept {
    if (!is_open() || !baud_supported(baud))
        return false;
    HANDLE h = reinterpret_cast<HANDLE>(handle_);
    FlushFileBuffers(h);
    DCB dcb;
    std::memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb))
        return false;
    dcb.BaudRate = baud;
    if (!SetCommState(h, &dcb))
        return false;
    PurgeComm(h, PURGE_RXCLEAR);
    return true;
}

void SerialPort::close() noexcept {
    if (!is_open())
        return;
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE);
    if (read_event_)
        CloseHandle(read_event_);
    if (write_event_)
        CloseHandle(write_event_);
    read_event_ = write_event_ = nullptr;
}

IoStatus SerialPort::write_all(const std::uint8_t* data, std::size_t length, std::uint32_t stall_ms,
                               std::int32_t* os_error) noexcept {
    if (!is_open())
        return IoStatus::closed;
    HANDLE h = reinterpret_cast<HANDLE>(handle_);
    if (stall_ms != current_write_stall_) {
        if (!set_read_timeout(h, current_read_timeout_, stall_ms))
            return IoStatus::error;
        current_write_stall_ = stall_ms;
    }
    std::size_t done = 0;
    while (done < length) {
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = static_cast<HANDLE>(write_event_);
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        if (!WriteFile(h, data + done, static_cast<DWORD>(length - done), &n, &ov)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (os_error != nullptr)
                    *os_error = static_cast<std::int32_t>(err);
                return IoStatus::error;
            }
            // The comm write timeout set at open bounds this wait.
            if (!GetOverlappedResult(h, &ov, &n, TRUE)) {
                if (os_error != nullptr)
                    *os_error = static_cast<std::int32_t>(GetLastError());
                return IoStatus::error;
            }
        }
        if (n == 0)
            return done == 0 ? IoStatus::timeout : IoStatus::short_write;
        done += n;
    }
    return IoStatus::ok;
}

long SerialPort::read(std::uint8_t* buffer, std::size_t capacity,
                      std::uint32_t timeout_ms) noexcept {
    if (!is_open())
        return -1;
    HANDLE h = reinterpret_cast<HANDLE>(handle_);
    if (timeout_ms != current_read_timeout_) {
        if (!set_read_timeout(h, timeout_ms, current_write_stall_))
            return -1;
        current_read_timeout_ = timeout_ms;
    }
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    ov.hEvent = static_cast<HANDLE>(read_event_);
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    if (!ReadFile(h, buffer, static_cast<DWORD>(capacity), &n, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return -1;
        if (!GetOverlappedResult(h, &ov, &n, TRUE))
            return -1;
    }
    return static_cast<long>(n);
}

void SerialPort::drain() noexcept {
    if (!is_open())
        return;
    HANDLE h = reinterpret_cast<HANDLE>(handle_);
    FlushFileBuffers(h);
    PurgeComm(h, PURGE_RXCLEAR);
}

void SerialPort::discard_input() noexcept {
    if (is_open())
        PurgeComm(reinterpret_cast<HANDLE>(handle_), PURGE_RXCLEAR);
}

} // namespace mimhid::transport
