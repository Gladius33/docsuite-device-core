// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

#include <vector>

namespace docsuite {

class EsclScanBackend {
public:
    [[nodiscard]] std::vector<ScannerInfo> probe_printers(
        const std::vector<PrinterInfo>& printers) const;
};

} // namespace docsuite
