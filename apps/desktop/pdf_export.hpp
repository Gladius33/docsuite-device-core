// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/ocr/tesseract_ocr.hpp"

#include <QImage>
#include <QString>

#include <optional>

namespace docsuite::desktop {

[[nodiscard]] bool export_scan_pdf(
    const QString& path,
    const QImage& image,
    int dpi,
    const std::optional<OcrResult>& ocr,
    QString* error_message = nullptr);

} // namespace docsuite::desktop
