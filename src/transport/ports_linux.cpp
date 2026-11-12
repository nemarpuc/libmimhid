// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "transport/serial_port.hpp"

#include <dirent.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

std::string read_sysfs_string(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return "";
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

} // namespace

namespace mimusb::transport {

namespace detail {

std::vector<PortCandidate> list_candidate_ports_in(
    const std::string& tty_class, // NOLINT(bugprone-easily-swappable-parameters)
    const std::string& dev) {
    std::vector<PortCandidate> out;
    DIR* dir = opendir(tty_class.c_str());
    if (!dir)
        return out;

    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.rfind("ttyUSB", 0) != 0 && name.rfind("ttyACM", 0) != 0)
            continue;

        std::string base = tty_class;
        base += '/';
        base += name;
        if (access((base + "/device").c_str(), F_OK) != 0)
            continue;

        std::string product = read_sysfs_string(base + "/device/../product");
        if (product.empty())
            product = read_sysfs_string(base + "/device/../../product");
        if (product.empty())
            product = "USB Serial Device";

        std::string path = dev;
        path += '/';
        path += name;
        out.push_back({std::move(path), product});
    }
    closedir(dir);
    return out;
}

} // namespace detail

std::vector<PortCandidate> list_candidate_ports() {
    return detail::list_candidate_ports_in("/sys/class/tty", "/dev");
}

} // namespace mimusb::transport
