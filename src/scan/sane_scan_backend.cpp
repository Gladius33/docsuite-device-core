// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/scan/sane_scan_backend.hpp"

#include <sane/sane.h>
#include <sane/saneopts.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace docsuite {
namespace {

class SaneSession {
public:
    SaneSession() {
        SANE_Int version_code = 0;
        const SANE_Status status = sane_init(&version_code, nullptr);
        if (status != SANE_STATUS_GOOD) {
            throw std::runtime_error(sane_strstatus(status));
        }
    }

    ~SaneSession() { sane_exit(); }

    SaneSession(const SaneSession&) = delete;
    SaneSession& operator=(const SaneSession&) = delete;
};

class SaneHandleGuard {
public:
    explicit SaneHandleGuard(const SANE_Handle handle) : handle_{handle} {}

    ~SaneHandleGuard() {
        if (handle_ != nullptr) {
            sane_close(handle_);
        }
    }

    [[nodiscard]] SANE_Handle get() const noexcept { return handle_; }

    SaneHandleGuard(const SaneHandleGuard&) = delete;
    SaneHandleGuard& operator=(const SaneHandleGuard&) = delete;

private:
    SANE_Handle handle_{nullptr};
};

[[nodiscard]] std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string model_token_with_digit(const std::string& text) {
    std::string token;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const unsigned char ch = i < text.size()
            ? static_cast<unsigned char>(text[i])
            : static_cast<unsigned char>(' ');

        if (std::isalnum(ch) != 0) {
            token.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }

        if (token.size() >= 4U &&
            std::any_of(token.begin(), token.end(), [](const unsigned char value) {
                return std::isdigit(value) != 0;
            })) {
            return token;
        }
        token.clear();
    }
    return {};
}

[[nodiscard]] bool same_device_identity(
    const std::string& requested,
    const SANE_Device& device) {

    const std::string requested_token = model_token_with_digit(requested);
    const std::string candidate_text =
        std::string{device.name != nullptr ? device.name : ""} + " " +
        std::string{device.vendor != nullptr ? device.vendor : ""} + " " +
        std::string{device.model != nullptr ? device.model : ""};
    const std::string candidate_token = model_token_with_digit(candidate_text);

    if (!requested_token.empty() && requested_token == candidate_token) {
        return true;
    }

    const auto lowered_requested = lower_copy(requested);
    const auto lowered_candidate = lower_copy(candidate_text);
    return lowered_requested.size() >= 8U &&
        (lowered_candidate.find(lowered_requested) != std::string::npos ||
         lowered_requested.find(lowered_candidate) != std::string::npos);
}

[[nodiscard]] std::vector<std::string> scanner_candidates(const std::string& requested) {
    std::vector<std::string> result{requested};

    const SANE_Device** devices = nullptr;
    if (sane_get_devices(&devices, SANE_FALSE) != SANE_STATUS_GOOD || devices == nullptr) {
        return result;
    }

    for (std::size_t i = 0; devices[i] != nullptr; ++i) {
        const SANE_Device& device = *devices[i];
        if (device.name == nullptr) {
            continue;
        }
        const std::string name = device.name;
        if (name != requested && same_device_identity(requested, device)) {
            result.push_back(name);
        }
    }

    return result;
}

[[nodiscard]] SANE_Handle open_scanner_resilient(const std::string& requested) {
    SANE_Status last_status = SANE_STATUS_INVAL;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto candidates = scanner_candidates(requested);
        for (const auto& candidate : candidates) {
            SANE_Handle handle = nullptr;
            last_status = sane_open(candidate.c_str(), &handle);
            if (last_status == SANE_STATUS_GOOD && handle != nullptr) {
                return handle;
            }
        }

        if (attempt < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds{250});
        }
    }

    throw std::runtime_error(
        "Unable to open scanner '" + requested + "' after rediscovery/retry: " +
        sane_strstatus(last_status));
}

[[nodiscard]] int option_count(const SANE_Handle handle) {
    SANE_Int count = 0;
    const SANE_Status status = sane_control_option(
        handle, 0, SANE_ACTION_GET_VALUE, &count, nullptr);
    if (status != SANE_STATUS_GOOD) {
        throw std::runtime_error(std::string{"Unable to read SANE option count: "} + sane_strstatus(status));
    }
    return count;
}

[[nodiscard]] int find_option(const SANE_Handle handle, const std::string_view name) {
    const int count = option_count(handle);
    for (int index = 1; index < count; ++index) {
        const SANE_Option_Descriptor* descriptor = sane_get_option_descriptor(handle, index);
        if (descriptor != nullptr && descriptor->name != nullptr && name == descriptor->name) {
            return index;
        }
    }
    return -1;
}

[[nodiscard]] std::string constrained_string_value(
    const SANE_Option_Descriptor& descriptor,
    const std::string& requested) {

    if (descriptor.constraint_type != SANE_CONSTRAINT_STRING_LIST ||
        descriptor.constraint.string_list == nullptr) {
        return requested;
    }

    const std::string lowered_requested = lower_copy(requested);
    for (std::size_t i = 0; descriptor.constraint.string_list[i] != nullptr; ++i) {
        const std::string supported = descriptor.constraint.string_list[i];
        if (lower_copy(supported) == lowered_requested) {
            return supported;
        }
    }

    if (lowered_requested == "gray" || lowered_requested == "grey" ||
        lowered_requested == "grayscale" || lowered_requested == "greyscale") {
        for (std::size_t i = 0; descriptor.constraint.string_list[i] != nullptr; ++i) {
            const std::string supported = descriptor.constraint.string_list[i];
            const std::string lowered = lower_copy(supported);
            if (lowered.find("gray") != std::string::npos ||
                lowered.find("grey") != std::string::npos) {
                return supported;
            }
        }
    }

    return requested;
}

void set_string_option(
    const SANE_Handle handle,
    const std::string_view name,
    const std::string& requested_value,
    const bool required) {

    const int index = find_option(handle, name);
    if (index < 0) {
        if (required) {
            throw std::runtime_error("Scanner does not expose SANE option '" + std::string{name} + "'");
        }
        return;
    }

    const SANE_Option_Descriptor* descriptor = sane_get_option_descriptor(handle, index);
    if (descriptor == nullptr || !SANE_OPTION_IS_ACTIVE(descriptor->cap) ||
        !SANE_OPTION_IS_SETTABLE(descriptor->cap) || descriptor->type != SANE_TYPE_STRING) {
        if (required) {
            throw std::runtime_error("SANE option '" + std::string{name} + "' is not settable");
        }
        return;
    }

    const std::string value = constrained_string_value(*descriptor, requested_value);
    std::vector<SANE_Char> buffer(static_cast<std::size_t>(std::max(descriptor->size, 1)));
    std::strncpy(buffer.data(), value.c_str(), buffer.size() - 1U);
    buffer.back() = '\0';

    SANE_Int info = 0;
    const SANE_Status status = sane_control_option(
        handle, index, SANE_ACTION_SET_VALUE, buffer.data(), &info);
    if (status != SANE_STATUS_GOOD) {
        throw std::runtime_error(
            "Unable to set SANE option '" + std::string{name} + "' to '" + value + "': " +
            sane_strstatus(status));
    }
}

void set_int_option(
    const SANE_Handle handle,
    const std::string_view name,
    const int value,
    const bool required) {

    const int index = find_option(handle, name);
    if (index < 0) {
        if (required) {
            throw std::runtime_error("Scanner does not expose SANE option '" + std::string{name} + "'");
        }
        return;
    }

    const SANE_Option_Descriptor* descriptor = sane_get_option_descriptor(handle, index);
    if (descriptor == nullptr || !SANE_OPTION_IS_ACTIVE(descriptor->cap) ||
        !SANE_OPTION_IS_SETTABLE(descriptor->cap) || descriptor->type != SANE_TYPE_INT) {
        if (required) {
            throw std::runtime_error("SANE option '" + std::string{name} + "' is not a settable integer");
        }
        return;
    }

    SANE_Word sane_value = static_cast<SANE_Word>(value);
    SANE_Int info = 0;
    const SANE_Status status = sane_control_option(
        handle, index, SANE_ACTION_SET_VALUE, &sane_value, &info);
    if (status != SANE_STATUS_GOOD) {
        throw std::runtime_error(
            "Unable to set SANE option '" + std::string{name} + "': " + sane_strstatus(status));
    }
}

[[nodiscard]] ScanPixelFormat pixel_format(const SANE_Frame format) {
    switch (format) {
        case SANE_FRAME_GRAY: return ScanPixelFormat::gray8;
        case SANE_FRAME_RGB: return ScanPixelFormat::rgb24;
        default:
            throw std::runtime_error("Unsupported SANE frame format (planar scans are not supported yet)");
    }
}

} // namespace

std::vector<ScannerInfo> SaneScanBackend::list_scanners(const bool include_virtual) const {
    SaneSession session;

    const SANE_Device** devices = nullptr;
    const SANE_Status list_status = sane_get_devices(&devices, SANE_FALSE);
    if (list_status != SANE_STATUS_GOOD) {
        throw std::runtime_error(sane_strstatus(list_status));
    }

    std::vector<ScannerInfo> result;
    if (devices != nullptr) {
        for (std::size_t i = 0; devices[i] != nullptr; ++i) {
            const SANE_Device& device = *devices[i];
            const std::string name = device.name != nullptr ? device.name : "";
            const std::string type = device.type != nullptr ? device.type : "";

            const bool virtual_camera = name.rfind("v4l:", 0) == 0 ||
                std::string_view{type}.find("virtual") != std::string_view::npos;
            if (!include_virtual && virtual_camera) {
                continue;
            }

            result.push_back(ScannerInfo{
                .name = name,
                .vendor = device.vendor != nullptr ? device.vendor : "",
                .model = device.model != nullptr ? device.model : "",
                .type = type,
                .backend = "SANE",
            });
        }
    }

    return result;
}

ScanFrame SaneScanBackend::scan(
    const std::string& scanner_name,
    const ScanSettings& settings) const {

    SaneSession session;
    SaneHandleGuard scanner{open_scanner_resilient(scanner_name)};
    const SANE_Handle handle = scanner.get();

    set_string_option(handle, SANE_NAME_SCAN_SOURCE, settings.source, false);
    set_string_option(handle, SANE_NAME_SCAN_MODE, settings.mode, true);
    set_int_option(handle, SANE_NAME_SCAN_RESOLUTION, settings.dpi, true);

    const SANE_Status start_status = sane_start(handle);
    if (start_status != SANE_STATUS_GOOD) {
        throw std::runtime_error(std::string{"Unable to start scan: "} + sane_strstatus(start_status));
    }

    SANE_Parameters parameters{};
    const SANE_Status parameter_status = sane_get_parameters(handle, &parameters);
    if (parameter_status != SANE_STATUS_GOOD) {
        sane_cancel(handle);
        throw std::runtime_error(std::string{"Unable to read scan parameters: "} + sane_strstatus(parameter_status));
    }

    if (parameters.depth != 8) {
        sane_cancel(handle);
        throw std::runtime_error("Only 8-bit SANE scans are supported in this first acquisition pipeline");
    }
    if (parameters.pixels_per_line <= 0 || parameters.bytes_per_line <= 0) {
        sane_cancel(handle);
        throw std::runtime_error("Scanner returned invalid image dimensions");
    }

    const ScanPixelFormat format = pixel_format(parameters.format);
    const int channels = format == ScanPixelFormat::rgb24 ? 3 : 1;
    const int packed_stride = parameters.pixels_per_line * channels;
    if (parameters.bytes_per_line < packed_stride) {
        sane_cancel(handle);
        throw std::runtime_error("Scanner returned an invalid bytes-per-line value");
    }

    std::vector<std::uint8_t> raw;
    if (parameters.lines > 0) {
        raw.reserve(static_cast<std::size_t>(parameters.bytes_per_line) *
            static_cast<std::size_t>(parameters.lines));
    }

    std::array<SANE_Byte, 64U * 1024U> buffer{};
    while (true) {
        SANE_Int bytes_read = 0;
        const SANE_Status status = sane_read(
            handle,
            buffer.data(),
            static_cast<SANE_Int>(buffer.size()),
            &bytes_read);

        if (bytes_read > 0) {
            raw.insert(
                raw.end(),
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(bytes_read));
        }

        if (status == SANE_STATUS_EOF) {
            break;
        }
        if (status != SANE_STATUS_GOOD) {
            sane_cancel(handle);
            throw std::runtime_error(std::string{"Scan read failed: "} + sane_strstatus(status));
        }
    }

    const std::size_t bytes_per_line = static_cast<std::size_t>(parameters.bytes_per_line);
    int lines = parameters.lines;
    if (lines <= 0) {
        lines = static_cast<int>(raw.size() / bytes_per_line);
    }
    if (lines <= 0 || raw.size() < bytes_per_line * static_cast<std::size_t>(lines)) {
        throw std::runtime_error("Scanner returned incomplete image data");
    }

    ScanFrame frame;
    frame.width = parameters.pixels_per_line;
    frame.height = lines;
    frame.dpi = settings.dpi;
    frame.format = format;
    frame.pixels.resize(
        static_cast<std::size_t>(packed_stride) * static_cast<std::size_t>(lines));

    for (int line = 0; line < lines; ++line) {
        const auto source_offset = static_cast<std::size_t>(line) * bytes_per_line;
        const auto target_offset = static_cast<std::size_t>(line) *
            static_cast<std::size_t>(packed_stride);
        std::copy_n(
            raw.data() + source_offset,
            packed_stride,
            frame.pixels.data() + target_offset);
    }

    return frame;
}

void SaneScanBackend::save_pnm(const ScanFrame& frame, const std::string& path) const {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        throw std::runtime_error("Cannot save an empty scan frame");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create scan output file: " + path);
    }

    output << (frame.format == ScanPixelFormat::rgb24 ? "P6\n" : "P5\n")
           << frame.width << ' ' << frame.height << "\n255\n";
    output.write(
        reinterpret_cast<const char*>(frame.pixels.data()),
        static_cast<std::streamsize>(frame.pixels.size()));

    if (!output) {
        throw std::runtime_error("Failed while writing scan output file: " + path);
    }
}

} // namespace docsuite
