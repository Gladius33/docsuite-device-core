// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pdf_export.hpp"

#include <QFont>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRectF>

#include <algorithm>

namespace docsuite::desktop {

bool export_scan_pdf(
    const QString& path,
    const QImage& image,
    const int dpi,
    const std::optional<OcrResult>& ocr,
    QString* error_message) {

    if (image.isNull()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("No scan image is available.");
        }
        return false;
    }

    QPdfWriter writer(path);
    writer.setCreator(QStringLiteral("DocSuite Device Center"));
    writer.setTitle(QStringLiteral("Scanned document"));
    writer.setResolution(std::max(dpi, 150));
    writer.setPageSize(QPageSize(QPageSize::A4));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Unable to initialize the PDF painter.");
        }
        return false;
    }

    QRectF target = painter.viewport();
    QSizeF scaled = image.size();
    scaled.scale(target.size(), Qt::KeepAspectRatio);
    target.setSize(scaled);
    target.moveCenter(QRectF(painter.viewport()).center());

    // Paint OCR text first so the image hides it visually while PDF viewers can
    // still search/select the underlying text layer.
    if (ocr.has_value() && !ocr->words.empty()) {
        const double scale_x = target.width() / static_cast<double>(image.width());
        const double scale_y = target.height() / static_cast<double>(image.height());
        painter.setPen(Qt::black);

        for (const auto& word : ocr->words) {
            if (word.text.empty() || word.width <= 0 || word.height <= 0) {
                continue;
            }

            const QRectF word_rect(
                target.left() + static_cast<double>(word.left) * scale_x,
                target.top() + static_cast<double>(word.top) * scale_y,
                static_cast<double>(word.width) * scale_x,
                static_cast<double>(word.height) * scale_y);

            QFont font = painter.font();
            font.setPixelSize(std::max(1, static_cast<int>(word_rect.height() * 0.82)));
            painter.setFont(font);
            painter.drawText(
                word_rect,
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                QString::fromStdString(word.text));
        }
    }

    painter.drawImage(target, image);
    painter.end();
    return true;
}

} // namespace docsuite::desktop
