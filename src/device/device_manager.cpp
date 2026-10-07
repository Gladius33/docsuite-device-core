// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <algorithm>
#include <cctype>

namespace docsuite {
namespace {

[[nodiscard]] std::string normalized_model(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return result;
}

[[nodiscard]] bool same_scanner(const ScannerInfo& left, const ScannerInfo& right) {
    if (left.name == right.name) {
        return true;
    }

    const std::string left_model = normalized_model(left.model);
    const std::string right_model = normalized_model(right.model);
    if (left_model.size() < 5U || right_model.size() < 5U) {
        return false;
    }

    return left_model.find(right_model) != std::string::npos ||
        right_model.find(left_model) != std::string::npos;
}

} // namespace

DeviceSnapshot DeviceManager::snapshot() const {
    auto printers = print_backend_.list_printers();
    auto scanners = scan_backend_.list_scanners();
    const auto direct_escl = escl_backend_.probe_printers(printers);

    for (const auto& candidate : direct_escl) {
        const bool duplicate = std::any_of(
            scanners.begin(), scanners.end(),
            [&candidate](const ScannerInfo& existing) {
                return same_scanner(existing, candidate);
            });
        if (!duplicate) {
            scanners.push_back(candidate);
        }
    }

    return DeviceSnapshot{
        .printers = std::move(printers),
        .scanners = std::move(scanners),
    };
}

} // namespace docsuite
