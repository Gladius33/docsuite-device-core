// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout
        << "docsuite-device-cli list\n"
        << "docsuite-device-cli print <printer> <file> [color|mono]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        docsuite::DeviceManager manager;

        if (argc < 2) {
            print_usage();
            return 2;
        }

        const std::string command = argv[1];
        if (command == "list") {
            const auto snapshot = manager.snapshot();

            std::cout << "Printers:\n";
            for (const auto& printer : snapshot.printers) {
                std::cout << "  - " << printer.name;
                if (printer.is_default) {
                    std::cout << " [default]";
                }
                if (!printer.model.empty()) {
                    std::cout << " | " << printer.model;
                }
                if (!printer.uri.empty()) {
                    std::cout << " | " << printer.uri;
                }
                std::cout << '\n';
            }

            std::cout << "Scanners:\n";
            for (const auto& scanner : snapshot.scanners) {
                std::cout << "  - " << scanner.name
                          << " | " << scanner.vendor
                          << " | " << scanner.model
                          << " | " << scanner.type << '\n';
            }
            return 0;
        }

        if (command == "print") {
            if (argc < 4) {
                print_usage();
                return 2;
            }

            docsuite::PrintProfile profile;
            if (argc >= 5 && std::string{argv[4]} == "mono") {
                profile.name = "Monochrome";
                profile.color_mode = "monochrome";
            } else {
                profile.name = "Color";
                profile.color_mode = "color";
            }

            const int job_id = manager.print_backend().print_file(
                argv[2], argv[3], "DocSuite print job", profile);

            std::cout << "Submitted CUPS job " << job_id << '\n';
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
