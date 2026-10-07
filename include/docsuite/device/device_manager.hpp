// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"
#include "docsuite/print/cups_print_backend.hpp"
#include "docsuite/print/job_manager.hpp"
#include "docsuite/scan/escl_scan_backend.hpp"
#include "docsuite/scan/sane_scan_backend.hpp"

#include <string>

namespace docsuite {

class DeviceManager;

class ScannerRouteView {
public:
    explicit ScannerRouteView(const DeviceManager& manager) noexcept : manager_{manager} {}

    [[nodiscard]] ScannerCapabilities capabilities(const std::string& scanner) const;
    [[nodiscard]] ScanFrame scan(
        const std::string& scanner,
        const ScanSettings& settings = {}) const;
    void save_pnm(const ScanFrame& frame, const std::string& path) const;

private:
    const DeviceManager& manager_;
};

class DeviceManager {
public:
    DeviceManager() : job_manager_{print_backend_} {}

    [[nodiscard]] DeviceSnapshot snapshot() const;
    [[nodiscard]] ScannerCapabilities scanner_capabilities(const std::string& scanner) const;
    [[nodiscard]] ScanFrame scan(const std::string& scanner, const ScanSettings& settings) const;
    void save_pnm(const ScanFrame& frame, const std::string& path) const;

    [[nodiscard]] const CupsPrintBackend& print_backend() const noexcept { return print_backend_; }
    [[nodiscard]] const JobManager& job_manager() const noexcept { return job_manager_; }
    [[nodiscard]] ScannerRouteView scan_backend() const noexcept { return ScannerRouteView{*this}; }
    [[nodiscard]] const SaneScanBackend& sane_backend() const noexcept { return sane_backend_; }
    [[nodiscard]] const EsclScanBackend& escl_backend() const noexcept { return escl_backend_; }

private:
    CupsPrintBackend print_backend_{};
    JobManager job_manager_;
    SaneScanBackend sane_backend_{};
    EsclScanBackend escl_backend_{};
};

inline ScannerCapabilities ScannerRouteView::capabilities(const std::string& scanner) const {
    return manager_.scanner_capabilities(scanner);
}

inline ScanFrame ScannerRouteView::scan(
    const std::string& scanner,
    const ScanSettings& settings) const {
    return manager_.scan(scanner, settings);
}

inline void ScannerRouteView::save_pnm(
    const ScanFrame& frame,
    const std::string& path) const {
    manager_.save_pnm(frame, path);
}

} // namespace docsuite
