// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

namespace docsuite {

DeviceSnapshot DeviceManager::snapshot() const {
    return DeviceSnapshot{
        .printers = print_backend_.list_printers(),
        .scanners = scan_backend_.list_scanners(),
    };
}

} // namespace docsuite
