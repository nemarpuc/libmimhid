// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "transport/serial_port.hpp"

#include <cstring>

// setupapi.h needs the types of windows.h.
#include <windows.h>

#include <setupapi.h>

namespace mimhid::transport {

// Lists present USB-backed COM ports (native USB CDC, CP210x, CH340, and FTDI VCP).
std::vector<PortCandidate> list_candidate_ports() {
    std::vector<PortCandidate> out;

    GUID guid;
    DWORD count = 0;
    if (!SetupDiClassGuidsFromNameA("Ports", &guid, 1, &count) || count == 0)
        return out;

    HDEVINFO set = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return out;

    SP_DEVINFO_DATA dev;
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char enumerator[64] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_ENUMERATOR_NAME, nullptr,
                                               reinterpret_cast<PBYTE>(enumerator),
                                               sizeof(enumerator) - 1, nullptr)) {
            continue;
        }
        if (std::strcmp(enumerator, "USB") != 0 && std::strcmp(enumerator, "FTDIBUS") != 0)
            continue;

        char name[64] = {0};
        DWORD size = sizeof(name) - 1;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        LONG rc = RegQueryValueExA(key, "PortName", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(name), &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || std::strncmp(name, "COM", 3) != 0)
            continue;

        char friendly[128] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, nullptr,
                                               reinterpret_cast<PBYTE>(friendly),
                                               sizeof(friendly) - 1, nullptr)) {
            std::strcpy(friendly, "USB Serial Device");
        }
        out.push_back({name, friendly});
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

} // namespace mimhid::transport
