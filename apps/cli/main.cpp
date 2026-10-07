// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout
        << "docsuite-device-cli list\n"
        << "docsuite-device-cli capabilities <printer> [--refresh]\n"
        << "docsuite-device-cli status <printer>\n"
        << "docsuite-device-cli print <printer> <file> [color|mono]\n";
}

const char* state_name(const docsuite::DeviceState state) {
    switch (state) {
        case docsuite::DeviceState::idle: return "idle";
        case docsuite::DeviceState::processing: return "processing";
        case docsuite::DeviceState::stopped: return "stopped";
        case docsuite::DeviceState::offline: return "offline";
        case docsuite::DeviceState::unknown: return "unknown";
    }
    return "unknown";
}

void print_strings(const char* label, const std::vector<std::string>& values) {
    std::cout << label << ':';
    if (values.empty()) {
        std::cout << " (not reported)";
    } else {
        for (const auto& value : values) {
            std::cout << ' ' << value;
        }
    }
    std::cout << '\n';
}

void print_ints(const char* label, const std::vector<int>& values) {
    std::cout << label << ':';
    if (values.empty()) {
        std::cout << " (not reported)";
    } else {
        for (const int value : values) {
            std::cout << ' ' << value;
        }
    }
    std::cout << '\n';
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
                          << " | " << scanner.type;
                if (!scanner.backend.empty()) {
                    std::cout << " | " << scanner.backend;
                }
                std::cout << '\n';
            }
            return 0;
        }

        if (command == "capabilities") {
            if (argc < 3) {
                print_usage();
                return 2;
            }
            const bool refresh = argc >= 4 && std::string{argv[3]} == "--refresh";
            const auto caps = manager.print_backend().capabilities(argv[2], refresh);

            std::cout << "Printer: " << caps.printer << '\n';
            print_strings("Color modes", caps.color_modes);
            print_strings("Sides", caps.sides);
            print_ints("Quality", caps.qualities);
            print_ints("Resolution DPI", caps.resolutions_dpi);
            print_strings("Media sources", caps.media_sources);
            print_strings("Media types", caps.media_types);
            print_strings("Document formats", caps.document_formats);
            std::cout << "Copies: " << caps.copies_min << '-' << caps.copies_max << '\n';
            std::cout << "Media (" << caps.media.size() << "):\n";
            for (const auto& medium : caps.media) {
                std::cout << "  - " << medium << '\n';
            }
            return 0;
        }

        if (command == "status") {
            if (argc < 3) {
                print_usage();
                return 2;
            }
            const auto status = manager.print_backend().status(argv[2]);
            std::cout << "Printer: " << status.printer << '\n'
                      << "Source: " << status.source << '\n'
                      << "State: " << state_name(status.state) << '\n'
                      << "Accepting jobs: " << (status.accepting_jobs ? "yes" : "no") << '\n';

            std::cout << "Reasons:";
            if (status.reasons.empty()) {
                std::cout << " none";
            } else {
                for (const auto& reason : status.reasons) {
                    std::cout << ' ' << reason;
                }
            }
            std::cout << '\n';

            std::cout << "Supplies:\n";
            if (status.supplies.empty()) {
                std::cout << "  (not reported)\n";
            }
            for (const auto& supply : status.supplies) {
                std::cout << "  - " << supply.name;
                if (!supply.type.empty()) {
                    std::cout << " | " << supply.type;
                }
                if (supply.percent.has_value()) {
                    std::cout << " | " << *supply.percent << '%';
                    if (*supply.percent <= supply.low_threshold) {
                        std::cout << " [low]";
                    }
                } else {
                    std::cout << " | level unavailable";
                }
                std::cout << '\n';
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
