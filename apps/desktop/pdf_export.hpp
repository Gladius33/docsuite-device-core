// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/ocr/tesseract_ocr.hpp"

#include <QImage>
#include <QString>

#include <optional>
#include <vector>

namespace docsuite::desktop {

struct PdfScanPage {
    QImage image;
    int dpi{300};
    std::optional<OcrResult> ocr;

    // One-level edit history. QImage is implicitly shared, so taking this
    // snapshot is cheap until the active page is replaced by the transform.
    QImage undo_image;
    int undo_dpi{300};
    std::optional<OcrResult> undo_ocr;
    bool has_undo{false};
};

[[nodiscard]] bool export_scan_pdf(
    const QString& path,
    const QImage& image,
    int dpi,
    const std::optional<OcrResult>& ocr,
    QString* error_message = nullptr);

[[nodiscard]] bool export_scan_pdf_pages(
    const QString& path,
    const std::vector<PdfScanPage>& pages,
    QString* error_message = nullptr);

} // namespace docsuite::desktop
