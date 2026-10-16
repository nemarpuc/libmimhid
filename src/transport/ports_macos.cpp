// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "transport/serial_port.hpp"

#include <dirent.h>

namespace mimhid::transport {

// Only the callout (cu.*) nodes are used: tty.* blocks on open waiting for carrier detect.
std::vector<PortCandidate> list_candidate_ports() {
    static const char* const prefixes[] = {"cu.usbserial", "cu.usbmodem", "cu.SLAB_USBtoUART",
                                           "cu.wchusbserial"};
    std::vector<PortCandidate> out;
    DIR* dir = opendir("/dev");
    if (!dir)
        return out;

    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        for (const char* prefix : prefixes) {
            if (name.rfind(prefix, 0) == 0) {
                out.push_back({"/dev/" + name, "USB Serial Device"});
                break;
            }
        }
    }
    closedir(dir);
    return out;
}

} // namespace mimhid::transport
