// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/print/print_validation.hpp"

#include <QJsonObject>

#include <memory>
#include <string>
#include <vector>

namespace docsuite::desktop {

class DeviceServiceGateway final {
public:
    explicit DeviceServiceGateway(std::shared_ptr<DeviceManager> fallback);

    [[nodiscard]] bool service_available() const noexcept;
    [[nodiscard]] DeviceSnapshot snapshot() const;
    [[nodiscard]] PrinterCapabilities printer_capabilities(
        const std::string& printer,
        bool refresh = false) const;
    [[nodiscard]] PrinterStatus printer_status(const std::string& printer) const;
    [[nodiscard]] ScannerCapabilities scanner_capabilities(const std::string& scanner) const;
    [[nodiscard]] std::vector<PrintJobInfo> printer_jobs(
        const std::string& printer,
        bool include_completed = true) const;
    [[nodiscard]] PrintPreflightResult printer_preflight(
        const std::string& printer,
        const PrintProfile& profile,
        bool detailed = false,
        bool refresh = false) const;

private:
    [[nodiscard]] QJsonObject call(
        const QString& method,
        const QJsonObject& params = {},
        int timeout_ms = 2500) const;

    std::shared_ptr<DeviceManager> fallback_;
};

} // namespace docsuite::desktop
