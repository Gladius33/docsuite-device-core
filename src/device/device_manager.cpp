// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace docsuite {
namespace {

[[nodiscard]] std::string normalized_model(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return result;
}

[[nodiscard]] std::string ipv4_key(const ScannerInfo& scanner) {
    const std::string text = scanner.name + " " + scanner.model + " " + scanner.type;

    for (std::size_t start = 0; start < text.size(); ++start) {
        if (std::isdigit(static_cast<unsigned char>(text[start])) == 0) {
            continue;
        }

        std::size_t end = start;
        int dots = 0;
        while (end < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[end]);
            if (std::isdigit(ch) != 0) {
                ++end;
                continue;
            }
            if (text[end] == '.' && dots < 3) {
                ++dots;
                ++end;
                continue;
            }
            break;
        }

        if (dots == 3 && end - start >= 7U && end - start <= 15U) {
            return text.substr(start, end - start);
        }
    }

    return {};
}

[[nodiscard]] bool same_scanner(const ScannerInfo& left, const ScannerInfo& right) {
    if (left.name == right.name) {
        return true;
    }

    const std::string left_ip = ipv4_key(left);
    const std::string right_ip = ipv4_key(right);
    if (!left_ip.empty() && left_ip == right_ip) {
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

void append_unique_scanner(std::vector<ScannerInfo>& scanners, ScannerInfo candidate) {
    const bool duplicate = std::any_of(
        scanners.begin(), scanners.end(),
        [&candidate](const ScannerInfo& existing) {
            return same_scanner(existing, candidate);
        });

    if (!duplicate) {
        scanners.push_back(std::move(candidate));
    }
}

} // namespace

DeviceSnapshot DeviceManager::snapshot() const {
    auto printers = print_backend_.list_printers();
    const auto sane_scanners = scan_backend_.list_scanners();

    std::vector<ScannerInfo> scanners;
    scanners.reserve(sane_scanners.size() + printers.size());
    for (const auto& scanner : sane_scanners) {
        append_unique_scanner(scanners, scanner);
    }

    const auto direct_escl = escl_backend_.probe_printers(printers);
    for (const auto& candidate : direct_escl) {
        append_unique_scanner(scanners, candidate);
    }

    return DeviceSnapshot{
        .printers = std::move(printers),
        .scanners = std::move(scanners),
    };
}

} // namespace docsuite
