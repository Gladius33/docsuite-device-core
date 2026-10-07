// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace docsuite {
namespace {

[[nodiscard]] bool smoke_test_mode() {
    const char* value = std::getenv("DOCSUITE_SMOKE_TEST");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

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

[[nodiscard]] std::string model_token_with_digit(const std::string& value) {
    std::string token;
    for (std::size_t i = 0; i <= value.size(); ++i) {
        const unsigned char ch = i < value.size()
            ? static_cast<unsigned char>(value[i])
            : static_cast<unsigned char>(' ');
        if (std::isalnum(ch) != 0) {
            token.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }
        if (token.size() >= 4U &&
            std::any_of(token.begin(), token.end(), [](const unsigned char candidate) {
                return std::isdigit(candidate) != 0;
            })) {
            return token;
        }
        token.clear();
    }
    return {};
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

    const std::string left_token = model_token_with_digit(left.model + " " + left.name);
    const std::string right_token = model_token_with_digit(right.model + " " + right.name);
    if (!left_token.empty() && left_token == right_token) {
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

[[nodiscard]] bool direct_escl_name(const std::string& scanner) {
    return scanner.rfind("escl:http://", 0) == 0 || scanner.rfind("escl:https://", 0) == 0;
}

[[nodiscard]] std::optional<ScannerInfo> matching_direct_escl(
    const std::string& requested,
    const SaneScanBackend& sane,
    const EsclScanBackend& escl,
    const CupsPrintBackend& print) {

    ScannerInfo requested_info{
        .name = requested,
        .vendor = {},
        .model = requested,
        .type = {},
        .backend = "requested",
    };

    try {
        for (const auto& candidate : sane.list_scanners()) {
            if (candidate.name == requested) {
                requested_info = candidate;
                break;
            }
        }
    } catch (...) {
        // Stale or unavailable SANE discovery must not prevent direct eSCL fallback.
    }

    for (const auto& candidate : escl.probe_printers(print.list_printers())) {
        if (same_scanner(requested_info, candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

DeviceSnapshot DeviceManager::snapshot() const {
    if (smoke_test_mode()) {
        return {};
    }

    auto printers = print_backend_.list_printers();
    const auto sane_scanners = sane_backend_.list_scanners();

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

ScannerCapabilities DeviceManager::scanner_capabilities(const std::string& scanner) const {
    if (direct_escl_name(scanner)) {
        return escl_backend_.capabilities(scanner);
    }

    try {
        return sane_backend_.capabilities(scanner);
    } catch (const std::exception& sane_error) {
        const auto direct = matching_direct_escl(
            scanner,
            sane_backend_,
            escl_backend_,
            print_backend_);
        if (!direct.has_value()) {
            throw;
        }
        try {
            return escl_backend_.capabilities(direct->name);
        } catch (const std::exception& escl_error) {
            throw std::runtime_error(
                std::string{"SANE capabilities failed: "} + sane_error.what() +
                "; direct eSCL fallback failed: " + escl_error.what());
        }
    }
}

ScanFrame DeviceManager::scan(
    const std::string& scanner,
    const ScanSettings& settings) const {

    if (direct_escl_name(scanner)) {
        return escl_backend_.scan(scanner, settings);
    }

    try {
        return sane_backend_.scan(scanner, settings);
    } catch (const std::exception& sane_error) {
        if (!escl_backend_.acquisition_available()) {
            throw;
        }

        const auto direct = matching_direct_escl(
            scanner,
            sane_backend_,
            escl_backend_,
            print_backend_);
        if (!direct.has_value()) {
            throw;
        }

        try {
            return escl_backend_.scan(direct->name, settings);
        } catch (const std::exception& escl_error) {
            throw std::runtime_error(
                std::string{"SANE acquisition failed: "} + sane_error.what() +
                "; direct eSCL fallback failed: " + escl_error.what());
        }
    }
}

void DeviceManager::save_pnm(
    const ScanFrame& frame,
    const std::string& path) const {
    sane_backend_.save_pnm(frame, path);
}

} // namespace docsuite
