// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

#include <string>

namespace docsuite {

struct OcrResult {
    std::string text;
    int mean_confidence{0};
    std::string language;
};

class TesseractOcr {
public:
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] OcrResult recognize(
        const ScanFrame& frame,
        const std::string& language = "fra+eng") const;
};

} // namespace docsuite
