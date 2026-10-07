// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

namespace docsuite {

[[nodiscard]] PrintPreflightResult validate_print_profile(
    const PrinterCapabilities& capabilities,
    const PrintProfile& profile);

} // namespace docsuite
