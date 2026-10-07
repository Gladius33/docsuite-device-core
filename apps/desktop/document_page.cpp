// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "document_page.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>

namespace docsuite::desktop {
namespace {

[[nodiscard]] QImage image_from_frame(const ScanFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        return {};
    }
    const bool rgb = frame.format == ScanPixelFormat::rgb24;
    const int channels = rgb ? 3 : 1;
    return QImage(
        frame.pixels.data(),
        frame.width,
        frame.height,
        frame.width * channels,
        rgb ? QImage::Format_RGB888 : QImage::Format_Grayscale8).copy();
}

[[nodiscard]] ScanFrame frame_from_image(const QImage& source, const int dpi) {
    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    ScanFrame frame;
    frame.width = rgb.width();
    frame.height = rgb.height();
    frame.dpi = dpi;
    frame.format = ScanPixelFormat::rgb24;
    const int stride = frame.width * 3;
    frame.pixels.resize(
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(frame.height));
    for (int y = 0; y < frame.height; ++y) {
        std::copy_n(
            rgb.constScanLine(y),
            stride,
            frame.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
    }
    return frame;
}

void populate_strings(QComboBox* combo, const std::vector<std::string>& values) {
    const QSignalBlocker blocker{combo};
    combo->clear();
    for (const auto& value : values) {
        combo->addItem(QString::fromStdString(value), QString::fromStdString(value));
    }
    combo->setEnabled(!values.empty());
}

} // namespace

DocumentPage::DocumentPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* layout = new QVBoxLayout(this);
    auto* settings = new QGroupBox(QStringLiteral("Document acquisition"), this);
    auto* settings_layout = new QHBoxLayout(settings);
    auto* form = new QFormLayout();

    scanner_ = new QComboBox(settings);
    mode_ = new QComboBox(settings);
    dpi_ = new QComboBox(settings);
    source_ = new QComboBox(settings);
    form->addRow(QStringLiteral("Scanner"), scanner_);
    form->addRow(QStringLiteral("Mode"), mode_);
    form->addRow(QStringLiteral("Resolution"), dpi_);
    form->addRow(QStringLiteral("Source"), source_);
    settings_layout->addLayout(form, 1);

    auto* acquisition_buttons = new QVBoxLayout();
    refresh_ = new QPushButton(QStringLiteral("Refresh scanners"), settings);
    add_ = new QPushButton(QStringLiteral("Scan + add page"), settings);
    acquisition_buttons->addWidget(refresh_);
    acquisition_buttons->addWidget(add_);
    acquisition_buttons->addStretch();
    settings_layout->addLayout(acquisition_buttons);

    status_ = new QLabel(QStringLiteral("Ready"), this);
    status_->setWordWrap(true);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    pages_list_ = new QListWidget(left);
    left_layout->addWidget(new QLabel(QStringLiteral("Pages"), left));
    left_layout->addWidget(pages_list_, 1);

    auto* edit_row = new QHBoxLayout();
    up_ = new QPushButton(QStringLiteral("↑"), left);
    down_ = new QPushButton(QStringLiteral("↓"), left);
    delete_ = new QPushButton(QStringLiteral("Delete"), left);
    clear_ = new QPushButton(QStringLiteral("Clear"), left);
    edit_row->addWidget(up_);
    edit_row->addWidget(down_);
    edit_row->addWidget(delete_);
    edit_row->addWidget(clear_);
    left_layout->addLayout(edit_row);

    ocr_ = new QPushButton(QStringLiteral("OCR selected page (fra+eng)"), left);
    export_ = new QPushButton(QStringLiteral("Export multipage PDF"), left);
    left_layout->addWidget(ocr_);
    left_layout->addWidget(export_);

    preview_scroll_ = new QScrollArea(splitter);
    preview_scroll_->setWidgetResizable(true);
    preview_ = new QLabel(preview_scroll_);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setText(QStringLiteral("No document pages yet"));
    preview_scroll_->setWidget(preview_);

    splitter->addWidget(left);
    splitter->addWidget(preview_scroll_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    layout->addWidget(settings);
    layout->addWidget(status_);
    layout->addWidget(splitter, 1);

    connect(refresh_, &QPushButton::clicked, this, [this]() { refresh_scanners(); });
    connect(add_, &QPushButton::clicked, this, [this]() { add_page(); });
    connect(delete_, &QPushButton::clicked, this, [this]() { delete_selected(); });
    connect(up_, &QPushButton::clicked, this, [this]() { move_selected(-1); });
    connect(down_, &QPushButton::clicked, this, [this]() { move_selected(1); });
    connect(clear_, &QPushButton::clicked, this, [this]() { clear_pages(); });
    connect(ocr_, &QPushButton::clicked, this, [this]() { run_ocr_selected(); });
    connect(export_, &QPushButton::clicked, this, [this]() { export_pdf(); });
    connect(scanner_, &QComboBox::currentIndexChanged, this,
        [this](int) { refresh_capabilities(); });
    connect(pages_list_, &QListWidget::currentRowChanged, this,
        [this](int) { update_preview(); });

    refresh_scanners();
    update_list();
}

void DocumentPage::refresh_scanners() {
    refresh_->setEnabled(false);
    add_->setEnabled(false);
    status_->setText(QStringLiteral("Discovering scanners…"));

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher]() {
            try {
                const QString previous = scanner_->currentData().toString();
                const auto snapshot = watcher->result();
                const QSignalBlocker blocker{scanner_};
                scanner_->clear();
                int restore = -1;
                for (const auto& scanner : snapshot.scanners) {
                    const QString label = QStringLiteral("%1 %2 — %3")
                        .arg(QString::fromStdString(scanner.vendor))
                        .arg(QString::fromStdString(scanner.model))
                        .arg(QString::fromStdString(scanner.backend));
                    scanner_->addItem(label, QString::fromStdString(scanner.name));
                    if (scanner_->itemData(scanner_->count() - 1).toString() == previous) {
                        restore = scanner_->count() - 1;
                    }
                }
                if (restore >= 0) {
                    scanner_->setCurrentIndex(restore);
                }
                add_->setEnabled(scanner_->count() > 0);
                status_->setText(
                    scanner_->count() > 0
                        ? QStringLiteral("Scanner ready — add pages to the document session")
                        : QStringLiteral("No scanner discovered"));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scanner discovery error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            watcher->deleteLater();
            refresh_capabilities();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void DocumentPage::refresh_capabilities() {
    const QString scanner = scanner_->currentData().toString();
    if (scanner.isEmpty()) {
        return;
    }
    const std::string name = scanner.toStdString();
    auto* watcher = new QFutureWatcher<ScannerCapabilities>(this);
    connect(watcher, &QFutureWatcher<ScannerCapabilities>::finished, this,
        [this, watcher, scanner]() {
            try {
                const auto caps = watcher->result();
                if (scanner_->currentData().toString() != scanner) {
                    watcher->deleteLater();
                    return;
                }
                populate_strings(mode_, caps.modes);
                populate_strings(source_, caps.sources);
                {
                    const QSignalBlocker blocker{dpi_};
                    dpi_->clear();
                    for (const int dpi : caps.resolutions_dpi) {
                        dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
                    }
                    const int preferred = dpi_->findData(300);
                    if (preferred >= 0) {
                        dpi_->setCurrentIndex(preferred);
                    }
                    dpi_->setEnabled(!caps.resolutions_dpi.empty());
                }
                status_->setText(
                    QStringLiteral("Capabilities loaded — max bed %1 × %2 mm")
                        .arg(caps.max_width_mm, 0, 'f', 1)
                        .arg(caps.max_height_mm, 0, 'f', 1));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scanner capability error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_, name]() {
        return manager->scan_backend().capabilities(name);
    }));
}

void DocumentPage::add_page() {
    const QString scanner = scanner_->currentData().toString();
    if (scanner.isEmpty()) {
        return;
    }

    ScanSettings settings;
    settings.dpi = dpi_->currentData().toInt();
    if (settings.dpi <= 0) {
        settings.dpi = 300;
    }
    settings.mode = mode_->currentData().toString().toStdString();
    if (settings.mode.empty()) {
        settings.mode = "Color";
    }
    settings.source = source_->currentData().toString().toStdString();
    if (settings.source.empty()) {
        settings.source = "Flatbed";
    }

    add_->setEnabled(false);
    refresh_->setEnabled(false);
    status_->setText(QStringLiteral("Scanning page %1…").arg(pages_.size() + 1U));

    using FramePtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<FramePtr>(this);
    connect(watcher, &QFutureWatcher<FramePtr>::finished, this,
        [this, watcher]() {
            try {
                const auto frame = watcher->result();
                QImage image = image_from_frame(*frame);
                if (image.isNull()) {
                    throw std::runtime_error("Scanner returned an empty document page");
                }
                pages_.push_back(PdfScanPage{
                    .image = std::move(image),
                    .dpi = frame->dpi > 0 ? frame->dpi : 300,
                    .ocr = std::nullopt,
                });
                update_list();
                pages_list_->setCurrentRow(static_cast<int>(pages_.size()) - 1);
                status_->setText(
                    QStringLiteral("Page %1 added — %2 × %3 px")
                        .arg(pages_.size())
                        .arg(frame->width)
                        .arg(frame->height));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scan error: %1").arg(QString::fromUtf8(error.what())));
            }
            add_->setEnabled(scanner_->count() > 0);
            refresh_->setEnabled(true);
            watcher->deleteLater();
        });

    const std::string name = scanner.toStdString();
    watcher->setFuture(QtConcurrent::run([manager = manager_, name, settings]() {
        return std::make_shared<ScanFrame>(manager->scan_backend().scan(name, settings));
    }));
}

void DocumentPage::delete_selected() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        return;
    }
    pages_.erase(pages_.begin() + row);
    update_list();
    if (!pages_.empty()) {
        pages_list_->setCurrentRow(std::min(row, static_cast<int>(pages_.size()) - 1));
    }
}

void DocumentPage::move_selected(const int delta) {
    const int row = pages_list_->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= static_cast<int>(pages_.size())) {
        return;
    }
    std::swap(pages_[static_cast<std::size_t>(row)], pages_[static_cast<std::size_t>(target)]);
    update_list();
    pages_list_->setCurrentRow(target);
}

void DocumentPage::clear_pages() {
    pages_.clear();
    update_list();
    status_->setText(QStringLiteral("Document session cleared"));
}

void DocumentPage::run_ocr_selected() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size()) || !ocr_.available()) {
        return;
    }

    ocr_->setEnabled(false);
    status_->setText(QStringLiteral("OCR page %1…").arg(row + 1));
    const ScanFrame frame = frame_from_image(
        pages_[static_cast<std::size_t>(row)].image,
        pages_[static_cast<std::size_t>(row)].dpi);

    auto* watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this,
        [this, watcher, row]() {
            try {
                if (row < static_cast<int>(pages_.size())) {
                    pages_[static_cast<std::size_t>(row)].ocr = watcher->result();
                    status_->setText(
                        QStringLiteral("OCR page %1 complete — confidence %2%")
                            .arg(row + 1)
                            .arg(watcher->result().mean_confidence));
                    update_list();
                    pages_list_->setCurrentRow(row);
                }
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("OCR error: %1").arg(QString::fromUtf8(error.what())));
            }
            ocr_->setEnabled(ocr_.available() && !pages_.empty());
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([ocr = ocr_, frame]() {
        return ocr.recognize(frame, "fra+eng");
    }));
}

void DocumentPage::export_pdf() {
    if (pages_.empty()) {
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export multipage PDF"),
        QStringLiteral("document.pdf"),
        QStringLiteral("PDF document (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".pdf");
    }

    QString error;
    if (!export_scan_pdf_pages(path, pages_, &error)) {
        QMessageBox::critical(this, QStringLiteral("DocSuite Document"), error);
        return;
    }
    const auto searchable = std::count_if(pages_.begin(), pages_.end(), [](const PdfScanPage& page) {
        return page.ocr.has_value();
    });
    status_->setText(
        QStringLiteral("Exported %1 page(s) to %2 — %3 page(s) searchable")
            .arg(pages_.size())
            .arg(path)
            .arg(searchable));
}

void DocumentPage::update_list() {
    const int previous = pages_list_->currentRow();
    const QSignalBlocker blocker{pages_list_};
    pages_list_->clear();
    for (std::size_t index = 0; index < pages_.size(); ++index) {
        const auto& page = pages_[index];
        QString label = QStringLiteral("Page %1 — %2 × %3 — %4 dpi")
            .arg(index + 1U)
            .arg(page.image.width())
            .arg(page.image.height())
            .arg(page.dpi);
        if (page.ocr.has_value()) {
            label += QStringLiteral(" — OCR %1%").arg(page.ocr->mean_confidence);
        }
        pages_list_->addItem(label);
    }
    if (!pages_.empty()) {
        pages_list_->setCurrentRow(
            std::clamp(previous, 0, static_cast<int>(pages_.size()) - 1));
    }

    const bool has_pages = !pages_.empty();
    delete_->setEnabled(has_pages);
    up_->setEnabled(has_pages);
    down_->setEnabled(has_pages);
    clear_->setEnabled(has_pages);
    export_->setEnabled(has_pages);
    ocr_->setEnabled(has_pages && ocr_.available());
    update_preview();
}

void DocumentPage::update_preview() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        preview_->setPixmap(QPixmap{});
        preview_->setText(QStringLiteral("No document pages yet"));
        return;
    }
    preview_->setText(QString{});
    QSize area = preview_scroll_->viewport()->size() - QSize(24, 24);
    if (area.width() < 100 || area.height() < 100) {
        area = QSize(800, 700);
    }
    preview_->setPixmap(
        QPixmap::fromImage(pages_[static_cast<std::size_t>(row)].image)
            .scaled(area, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace docsuite::desktop
