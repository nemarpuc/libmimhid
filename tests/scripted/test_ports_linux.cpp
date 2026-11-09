// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The Linux port list, on a sysfs tty class directory made by the test: which entries are
 * candidates and where their product name comes from. Nothing outside the temporary directory is
 * read, and no port is opened. */
#include "scripted_support.hpp"
#include "transport/serial_port.hpp"

#ifdef __linux__
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __linux__
namespace {

void write_file(const std::string& path, const char* text) {
    std::ofstream file(path);
    file << text;
}

} // namespace

void test_ports_linux() {
    char pattern[] = "/tmp/mimhid-ports-XXXXXX";
    const char* root_c = mkdtemp(pattern);
    MIMHID_CHECK(root_c != nullptr);
    if (root_c == nullptr)
        return;
    const std::string root = root_c;
    const std::string tty = root + "/class/tty";
    const std::string usb = root + "/devices/usb1";
    // A CDC ACM port: device -> the interface, whose parent has the product.
    // A USB serial adapter: device -> the port, two levels below the product.
    // A port without a product, a non-USB tty, and a USB name without a device link.
    for (const std::string& dir :
         {root + "/class", tty, root + "/devices", usb, usb + "/1-1", usb + "/1-1/1-1:1.0",
          usb + "/1-2", usb + "/1-2/1-2:1.0", usb + "/1-2/1-2:1.0/ttyUSB0", usb + "/1-3",
          usb + "/1-3/1-3:1.0", tty + "/ttyACM0", tty + "/ttyUSB0", tty + "/ttyACM1",
          tty + "/ttyS0", tty + "/ttyUSB9"}) {
        MIMHID_CHECK(mkdir(dir.c_str(), 0700) == 0);
    }
    write_file(usb + "/1-1/product", "Pico board\r\n");
    write_file(usb + "/1-2/product", "USB serial adapter\n");
    MIMHID_CHECK(symlink((usb + "/1-1/1-1:1.0").c_str(), (tty + "/ttyACM0/device").c_str()) == 0);
    MIMHID_CHECK(
        symlink((usb + "/1-2/1-2:1.0/ttyUSB0").c_str(), (tty + "/ttyUSB0/device").c_str()) == 0);
    MIMHID_CHECK(symlink((usb + "/1-3/1-3:1.0").c_str(), (tty + "/ttyACM1/device").c_str()) == 0);
    MIMHID_CHECK(symlink((usb + "/1-1/1-1:1.0").c_str(), (tty + "/ttyS0/device").c_str()) == 0);

    auto ports = mimhid::transport::detail::list_candidate_ports_in(tty, "/dev");
    std::sort(ports.begin(), ports.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    MIMHID_CHECK(ports.size() == 3U);
    if (ports.size() == 3U) {
        MIMHID_CHECK(ports[0].path == "/dev/ttyACM0" && ports[0].description == "Pico board");
        MIMHID_CHECK(ports[1].path == "/dev/ttyACM1" &&
                     ports[1].description == "USB Serial Device");
        MIMHID_CHECK(ports[2].path == "/dev/ttyUSB0" &&
                     ports[2].description == "USB serial adapter");
    }
    // A directory that is not there lists nothing.
    MIMHID_CHECK(
        mimhid::transport::detail::list_candidate_ports_in(root + "/none", "/dev").empty());
    // The real list holds only USB ttys (it reads sysfs and opens nothing).
    for (const auto& port : mimhid::transport::list_candidate_ports()) {
        MIMHID_CHECK(port.path.rfind("/dev/ttyUSB", 0) == 0 ||
                     port.path.rfind("/dev/ttyACM", 0) == 0);
        MIMHID_CHECK(!port.description.empty());
    }
    const std::string remove = "rm -rf '" + root + "'";
    MIMHID_CHECK(std::system(remove.c_str()) == 0);
}
#else
void test_ports_linux() {}
#endif
