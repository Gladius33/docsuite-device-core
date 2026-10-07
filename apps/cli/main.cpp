// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"
#include "docsuite/print/direct_ipp_probe.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout
        << "docsuite-device-cli list\n"
        << "docsuite-device-cli capabilities <printer> [--refresh]\n"
        << "docsuite-device-cli status <printer>\n"
        << "docsuite-device-cli probe-ipp <ipp-uri>\n"
        << "docsuite-device-cli jobs <printer> [active]\n"
        << "docsuite-device-cli cancel <printer> <job-id>\n"
        << "docsuite-device-cli preflight <printer> [color|mono]\n"
        << "docsuite-device-cli print <printer> <file> [color|mono]\n"
        << "docsuite-device-cli diagnose-print <printer> <file> [color|mono]\n"
        << "docsuite-device-cli scan <scanner> <output.pnm> [dpi] [color|gray]\n";
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

void print_capabilities(const docsuite::PrinterCapabilities& caps) {
    std::cout << "Printer: " << caps.printer << '\n'
              << "Source: " << caps.source << '\n';
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
}

void print_status(const docsuite::PrinterStatus& status) {
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
}

[[nodiscard]] docsuite::PrintProfile profile_from_arg(
    const int argc,
    char** argv,
    const int index) {

    docsuite::PrintProfile profile;
    if (argc > index && std::string{argv[index]} == "mono") {
        profile.name = "Monochrome";
        profile.color_mode = "monochrome";
    } else {
        profile.name = "Color";
        profile.color_mode = "color";
    }
    return profile;
}

void print_optional_duration(
    const char* label,
    const std::optional<std::chrono::milliseconds>& value) {
    std::cout << label << ": ";
    if (value.has_value()) {
        std::cout << value->count() << " ms";
    } else {
        std::cout << "not reported";
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
                if (printer.is_default) std::cout << " [default]";
                if (!printer.model.empty()) std::cout << " | " << printer.model;
                if (!printer.uri.empty()) std::cout << " | " << printer.uri;
                std::cout << '\n';
            }

            std::cout << "Scanners:\n";
            for (const auto& scanner : snapshot.scanners) {
                std::cout << "  - " << scanner.name
                          << " | " << scanner.vendor
                          << " | " << scanner.model
                          << " | " << scanner.type;
                if (!scanner.backend.empty()) std::cout << " | " << scanner.backend;
                std::cout << '\n';
            }
            return 0;
        }

        if (command == "capabilities") {
            if (argc < 3) { print_usage(); return 2; }
            const bool refresh = argc >= 4 && std::string{argv[3]} == "--refresh";
            print_capabilities(manager.print_backend().capabilities(argv[2], refresh));
            return 0;
        }

        if (command == "status") {
            if (argc < 3) { print_usage(); return 2; }
            print_status(manager.print_backend().status(argv[2]));
            return 0;
        }

        if (command == "probe-ipp") {
            if (argc < 3) { print_usage(); return 2; }
            docsuite::DirectIppProbe probe;
            print_capabilities(probe.capabilities(argv[2]));
            std::cout << '\n';
            print_status(probe.status(argv[2]));
            return 0;
        }

        if (command == "jobs") {
            if (argc < 3) { print_usage(); return 2; }
            const bool include_completed = !(argc >= 4 && std::string{argv[3]} == "active");
            const auto jobs = manager.job_manager().list_jobs(argv[2], include_completed);
            std::cout << "Jobs: " << jobs.size() << '\n';
            for (const auto& job : jobs) {
                std::cout << "  - #" << job.id
                          << " | " << docsuite::print_job_state_name(job.state)
                          << " | " << job.title
                          << " | " << job.size_kib << " KiB";
                if (!job.format.empty()) std::cout << " | " << job.format;
                std::cout << '\n';
            }
            return 0;
        }

        if (command == "cancel") {
            if (argc < 4) { print_usage(); return 2; }
            const int job_id = std::stoi(argv[3]);
            const bool canceled = manager.job_manager().cancel(argv[2], job_id);
            std::cout << (canceled ? "Canceled" : "Cancel failed") << " job " << job_id << '\n';
            return canceled ? 0 : 1;
        }

        if (command == "preflight") {
            if (argc < 3) { print_usage(); return 2; }
            const auto profile = profile_from_arg(argc, argv, 3);
            const auto result = manager.print_backend().preflight(argv[2], profile, false);
            std::cout << "Preflight: " << (result.ok ? "OK" : "FAIL") << '\n';
            for (const auto& warning : result.warnings) {
                std::cout << "  warning: " << warning << '\n';
            }
            for (const auto& error : result.errors) {
                std::cout << "  error: " << error << '\n';
            }
            return result.ok ? 0 : 1;
        }

        if (command == "print") {
            if (argc < 4) { print_usage(); return 2; }
            const auto profile = profile_from_arg(argc, argv, 4);
            const int job_id = manager.print_backend().print_file_advanced(
                argv[2], argv[3], "DocSuite print job", profile);
            std::cout << "Submitted CUPS job " << job_id << '\n';
            return 0;
        }

        if (command == "diagnose-print") {
            if (argc < 4) { print_usage(); return 2; }
            const auto profile = profile_from_arg(argc, argv, 4);
            const auto trace = manager.job_manager().diagnose_print(
                argv[2], argv[3], "DocSuite diagnostic print", profile);

            std::cout << "Job: " << trace.job_id << '\n'
                      << "Printer: " << trace.printer << '\n'
                      << "Final state: " << docsuite::print_job_state_name(trace.final_state) << '\n'
                      << "Submit -> CUPS accepted: " << trace.submit_to_accept.count() << " ms\n"
                      << "Timeline:\n";
            for (const auto& event : trace.events) {
                std::cout << "  +" << event.since_submit.count() << " ms"
                          << " | " << event.name
                          << " | " << docsuite::print_job_state_name(event.state) << '\n';
            }
            print_optional_duration("CUPS queue delay", trace.queue_delay);
            print_optional_duration("CUPS processing duration", trace.processing_duration);
            print_optional_duration("CUPS total duration", trace.total_duration);
            std::cout << "Timed out: " << (trace.timed_out ? "yes" : "no") << '\n';
            if (!trace.history_path.empty()) std::cout << "History: " << trace.history_path << '\n';
            return trace.final_state == docsuite::PrintJobState::completed ? 0 : 1;
        }

        if (command == "scan") {
            if (argc < 4) { print_usage(); return 2; }

            docsuite::ScanSettings settings;
            if (argc >= 5) settings.dpi = std::stoi(argv[4]);
            if (argc >= 6 && std::string{argv[5]} == "gray") settings.mode = "Gray";

            const auto frame = manager.scan_backend().scan(argv[2], settings);
            manager.scan_backend().save_pnm(frame, argv[3]);

            std::cout << "Scan saved: " << argv[3] << '\n'
                      << "Size: " << frame.width << 'x' << frame.height << '\n'
                      << "DPI: " << frame.dpi << '\n'
                      << "Mode: " << (frame.format == docsuite::ScanPixelFormat::rgb24 ? "RGB24" : "Gray8") << '\n'
                      << "Bytes: " << frame.pixels.size() << '\n';
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
