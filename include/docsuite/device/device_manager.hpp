// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"
#include "docsuite/print/cups_print_backend.hpp"
#include "docsuite/scan/escl_scan_backend.hpp"
#include "docsuite/scan/sane_scan_backend.hpp"

namespace docsuite {

class DeviceManager {
public:
    [[nodiscard]] DeviceSnapshot snapshot() const;
    [[nodiscard]] const CupsPrintBackend& print_backend() const noexcept { return print_backend_; }

private:
    CupsPrintBackend print_backend_{};
    SaneScanBackend scan_backend_{};
    EsclScanBackend escl_backend_{};
};

} // namespace docsuite
