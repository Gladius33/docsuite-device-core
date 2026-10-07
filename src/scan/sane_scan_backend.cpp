// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/scan/sane_scan_backend.hpp"

#include <sane/sane.h>

#include <stdexcept>

namespace docsuite {

std::vector<ScannerInfo> SaneScanBackend::list_scanners() const {
    SANE_Int version_code = 0;
    const SANE_Status init_status = sane_init(&version_code, nullptr);
    if (init_status != SANE_STATUS_GOOD) {
        throw std::runtime_error(sane_strstatus(init_status));
    }

    const SANE_Device** devices = nullptr;
    const SANE_Status list_status = sane_get_devices(&devices, SANE_FALSE);
    if (list_status != SANE_STATUS_GOOD) {
        sane_exit();
        throw std::runtime_error(sane_strstatus(list_status));
    }

    std::vector<ScannerInfo> result;
    if (devices != nullptr) {
        for (std::size_t i = 0; devices[i] != nullptr; ++i) {
            const SANE_Device& device = *devices[i];
            result.push_back(ScannerInfo{
                .name = device.name != nullptr ? device.name : "",
                .vendor = device.vendor != nullptr ? device.vendor : "",
                .model = device.model != nullptr ? device.model : "",
                .type = device.type != nullptr ? device.type : "",
            });
        }
    }

    sane_exit();
    return result;
}

} // namespace docsuite
