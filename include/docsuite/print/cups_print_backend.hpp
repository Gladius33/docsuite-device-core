// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace docsuite {

class CupsPrintBackend {
public:
    [[nodiscard]] std::vector<PrinterInfo> list_printers(bool include_transient = false) const;

    [[nodiscard]] PrinterCapabilities capabilities(
        const std::string& printer,
        bool force_refresh = false) const;

    [[nodiscard]] PrinterStatus status(const std::string& printer) const;

    [[nodiscard]] int print_file(
        const std::string& printer,
        const std::string& path,
        const std::string& title,
        const PrintProfile& profile) const;

    [[nodiscard]] int print_file_advanced(
        const std::string& printer,
        const std::string& path,
        const std::string& title,
        const PrintProfile& profile) const;

    void clear_capability_cache() const;

private:
    struct CachedCapabilities {
        PrinterCapabilities value;
        std::chrono::steady_clock::time_point fetched_at;
    };

    static constexpr auto capability_ttl_ = std::chrono::minutes{10};

    mutable std::mutex capability_cache_mutex_;
    mutable std::unordered_map<std::string, CachedCapabilities> capability_cache_;
};

} // namespace docsuite
