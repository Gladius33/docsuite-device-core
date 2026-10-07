// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"

namespace docsuite {

struct ImageRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

class ImageProcessor {
public:
    [[nodiscard]] bool deskew_available() const noexcept;

    [[nodiscard]] ScanFrame grayscale(const ScanFrame& frame) const;
    [[nodiscard]] ScanFrame enhance_document(const ScanFrame& frame, bool binarize = false) const;
    [[nodiscard]] ScanFrame deskew(const ScanFrame& frame) const;
    [[nodiscard]] ImageRect detect_content(const ScanFrame& frame) const;
    [[nodiscard]] ScanFrame crop(const ScanFrame& frame, ImageRect rect) const;
    [[nodiscard]] bool is_blank(const ScanFrame& frame, double white_ratio = 0.995) const;
};

} // namespace docsuite
