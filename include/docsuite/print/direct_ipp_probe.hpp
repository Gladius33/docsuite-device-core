// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

#include <string>

namespace docsuite {

class DirectIppProbe {
public:
    [[nodiscard]] PrinterCapabilities capabilities(
        const std::string& device_uri) const;

    [[nodiscard]] PrinterStatus status(
        const std::string& device_uri) const;
};

} // namespace docsuite
