// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/scan/sane_scan_backend.hpp"

#include <sane/sane.h>
#include <sane/saneopts.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace docsuite {
namespace {

class Session {
public:
    Session() {
        SANE_Int version = 0;
        const auto status = sane_init(&version, nullptr);
        if (status != SANE_STATUS_GOOD) {
            throw std::runtime_error(sane_strstatus(status));
        }
    }
    ~Session() { sane_exit(); }
};

class Handle {
public:
    explicit Handle(SANE_Handle handle) : handle_{handle} {}
    ~Handle() {
        if (handle_ != nullptr) {
            sane_close(handle_);
        }
    }
    [[nodiscard]] SANE_Handle get() const noexcept { return handle_; }
private:
    SANE_Handle handle_{nullptr};
};

[[nodiscard]] std::string model_token(const std::string& text) {
    std::string token;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const unsigned char ch = i < text.size()
            ? static_cast<unsigned char>(text[i])
            : static_cast<unsigned char>(' ');
        if (std::isalnum(ch) != 0) {
            token.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }
        if (token.size() >= 4U && std::any_of(token.begin(), token.end(), [](const char value) {
                return std::isdigit(static_cast<unsigned char>(value)) != 0;
            })) {
            return token;
        }
        token.clear();
    }
    return {};
}

[[nodiscard]] SANE_Handle open_resolved(const std::string& requested) {
    SANE_Handle handle = nullptr;
    if (sane_open(requested.c_str(), &handle) == SANE_STATUS_GOOD && handle != nullptr) {
        return handle;
    }

    const std::string requested_token = model_token(requested);
    const SANE_Device** devices = nullptr;
    const auto list_status = sane_get_devices(&devices, SANE_FALSE);
    if (list_status == SANE_STATUS_GOOD && devices != nullptr && !requested_token.empty()) {
        for (std::size_t i = 0; devices[i] != nullptr; ++i) {
            const auto& device = *devices[i];
            const std::string candidate =
                std::string{device.name != nullptr ? device.name : ""} + " " +
                std::string{device.vendor != nullptr ? device.vendor : ""} + " " +
                std::string{device.model != nullptr ? device.model : ""};
            if (model_token(candidate) != requested_token || device.name == nullptr) {
                continue;
            }
            handle = nullptr;
            if (sane_open(device.name, &handle) == SANE_STATUS_GOOD && handle != nullptr) {
                return handle;
            }
        }
    }

    throw std::runtime_error("Unable to open scanner for capability discovery: " + requested);
}

[[nodiscard]] const SANE_Option_Descriptor* find_descriptor(
    const SANE_Handle handle,
    const char* name) {

    SANE_Int count = 0;
    if (sane_control_option(handle, 0, SANE_ACTION_GET_VALUE, &count, nullptr) != SANE_STATUS_GOOD) {
        return nullptr;
    }
    for (int index = 1; index < count; ++index) {
        const auto* descriptor = sane_get_option_descriptor(handle, index);
        if (descriptor != nullptr && descriptor->name != nullptr &&
            std::string{descriptor->name} == name) {
            return descriptor;
        }
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> string_values(const SANE_Option_Descriptor* descriptor) {
    std::vector<std::string> values;
    if (descriptor == nullptr || descriptor->constraint_type != SANE_CONSTRAINT_STRING_LIST ||
        descriptor->constraint.string_list == nullptr) {
        return values;
    }
    for (std::size_t i = 0; descriptor->constraint.string_list[i] != nullptr; ++i) {
        values.emplace_back(descriptor->constraint.string_list[i]);
    }
    return values;
}

[[nodiscard]] std::vector<int> resolution_values(const SANE_Option_Descriptor* descriptor) {
    std::vector<int> values;
    if (descriptor == nullptr || descriptor->type != SANE_TYPE_INT) {
        return values;
    }

    if (descriptor->constraint_type == SANE_CONSTRAINT_WORD_LIST &&
        descriptor->constraint.word_list != nullptr) {
        const int count = static_cast<int>(descriptor->constraint.word_list[0]);
        for (int i = 1; i <= count; ++i) {
            values.push_back(static_cast<int>(descriptor->constraint.word_list[i]));
        }
    } else if (descriptor->constraint_type == SANE_CONSTRAINT_RANGE &&
               descriptor->constraint.range != nullptr) {
        const int minimum = static_cast<int>(descriptor->constraint.range->min);
        const int maximum = static_cast<int>(descriptor->constraint.range->max);
        for (const int common : {75, 100, 150, 200, 300, 400, 600, 1200, 2400}) {
            if (common >= minimum && common <= maximum) {
                values.push_back(common);
            }
        }
        if (values.empty()) {
            values.push_back(minimum);
            if (maximum != minimum) {
                values.push_back(maximum);
            }
        }
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

[[nodiscard]] double range_max_mm(const SANE_Option_Descriptor* descriptor) {
    if (descriptor == nullptr || descriptor->constraint_type != SANE_CONSTRAINT_RANGE ||
        descriptor->constraint.range == nullptr) {
        return 0.0;
    }
    if (descriptor->type == SANE_TYPE_FIXED) {
        return SANE_UNFIX(descriptor->constraint.range->max);
    }
    if (descriptor->type == SANE_TYPE_INT) {
        return static_cast<double>(descriptor->constraint.range->max);
    }
    return 0.0;
}

} // namespace

ScannerCapabilities SaneScanBackend::capabilities(const std::string& scanner_name) const {
    Session session;
    Handle scanner{open_resolved(scanner_name)};
    const SANE_Handle handle = scanner.get();

    ScannerCapabilities result;
    result.scanner = scanner_name;
    result.source = "sane";
    result.modes = string_values(find_descriptor(handle, SANE_NAME_SCAN_MODE));
    result.sources = string_values(find_descriptor(handle, SANE_NAME_SCAN_SOURCE));
    result.resolutions_dpi = resolution_values(find_descriptor(handle, SANE_NAME_SCAN_RESOLUTION));
    result.max_width_mm = range_max_mm(find_descriptor(handle, SANE_NAME_SCAN_BR_X));
    result.max_height_mm = range_max_mm(find_descriptor(handle, SANE_NAME_SCAN_BR_Y));
    return result;
}

} // namespace docsuite
