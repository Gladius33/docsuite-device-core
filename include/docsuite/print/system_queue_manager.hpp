// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <string>

namespace docsuite {

struct SystemQueueInfo {
    std::string name;
    std::string device_uri;
    std::string printer_uri;
    bool exists{false};
    bool driverless{false};
    bool temporary{false};
};

class SystemQueueManager {
public:
    [[nodiscard]] SystemQueueInfo inspect(const std::string& name) const;

    [[nodiscard]] SystemQueueInfo ensure_temporary_driverless(
        const std::string& name,
        const std::string& device_uri,
        const std::string& info = "DocSuite driverless printer") const;
};

} // namespace docsuite
