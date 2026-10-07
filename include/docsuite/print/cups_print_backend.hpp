// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

#include <string>
#include <vector>

namespace docsuite {

class CupsPrintBackend {
public:
    [[nodiscard]] std::vector<PrinterInfo> list_printers() const;
    [[nodiscard]] int print_file(
        const std::string& printer,
        const std::string& path,
        const std::string& title,
        const PrintProfile& profile) const;
};

} // namespace docsuite
