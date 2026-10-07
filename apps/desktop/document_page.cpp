// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "document_page.hpp"
#include "selection_preview.hpp"

#include <QCheckBox>
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
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTransform>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>

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

[[nodiscard]] ScanFrame automatic_cleanup(ScanFrame frame, const bool deskew_enabled) {
    ImageProcessor processor;
    if (deskew_enabled && processor.deskew_available()) {
        frame = processor.deskew(frame);
    }
    const ImageRect content = processor.detect_content(frame);
    const bool useful_crop = content.x > 0 || content.y > 0 ||
        content.width < frame.width || content.height < frame.height;
    if (useful_crop) {
        frame = processor.crop(frame, content);
    }
    return frame;
}

void remember_undo(PdfScanPage& page) {
    page.undo_image = page.image;
    page.undo_dpi = page.dpi;
    page.undo_ocr = page.ocr;
    page.has_undo = true;
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
    auto_process_ = new QCheckBox(QStringLiteral("Deskew + crop automatically"), settings);
    auto_process_->setChecked(true);
    skip_blank_ = new QCheckBox(QStringLiteral("Skip blank pages"), settings);
    skip_blank_->setChecked(true);

    form->addRow(QStringLiteral("Scanner"), scanner_);
    form->addRow(QStringLiteral("Mode"), mode_);
    form->addRow(QStringLiteral("Resolution"), dpi_);
    form->addRow(QStringLiteral("Source"), source_);
    form->addRow(QStringLiteral("Auto cleanup"), auto_process_);
    form->addRow(QStringLiteral("Blank pages"), skip_blank_);
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

    auto* process_group = new QGroupBox(QStringLiteral("Selected page processing"), left);
    auto* process_layout = new QVBoxLayout(process_group);
    auto_crop_ = new QPushButton(QStringLiteral("Auto crop content"), process_group);
    crop_selection_ = new QPushButton(QStringLiteral("Crop selection"), process_group);
    crop_selection_->setToolTip(QStringLiteral(
        "Drag a rectangle over the page preview, then crop to that exact source area."));

    auto* rotate_row = new QHBoxLayout();
    rotate_left_ = new QPushButton(QStringLiteral("↶ Rotate left"), process_group);
    rotate_right_ = new QPushButton(QStringLiteral("Rotate right ↷"), process_group);
    rotate_row->addWidget(rotate_left_);
    rotate_row->addWidget(rotate_right_);

    enhance_ = new QPushButton(QStringLiteral("Enhance document contrast"), process_group);
    binarize_ = new QPushButton(QStringLiteral("Black && white document"), process_group);
    deskew_ = new QPushButton(QStringLiteral("Automatic deskew"), process_group);
    deskew_->setEnabled(processor_.deskew_available());
    if (!processor_.deskew_available()) {
        deskew_->setToolTip(QStringLiteral("Deskew is available when DocSuite is built with OpenCV."));
    }
    undo_ = new QPushButton(QStringLiteral("Undo last page transform"), process_group);

    process_layout->addWidget(auto_crop_);
    process_layout->addWidget(crop_selection_);
    process_layout->addLayout(rotate_row);
    process_layout->addWidget(enhance_);
    process_layout->addWidget(binarize_);
    process_layout->addWidget(deskew_);
    process_layout->addWidget(undo_);
    left_layout->addWidget(process_group);

    ocr_button_ = new QPushButton(QStringLiteral("OCR selected page (fra+eng)"), left);
    export_ = new QPushButton(QStringLiteral("Export multipage PDF"), left);
    left_layout->addWidget(ocr_button_);
    left_layout->addWidget(export_);

    preview_scroll_ = new QScrollArea(splitter);
    preview_scroll_->setWidgetResizable(true);
    preview_ = new SelectionPreview(preview_scroll_);
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
    connect(auto_crop_, &QPushButton::clicked, this, [this]() { transform_selected("crop"); });
    connect(crop_selection_, &QPushButton::clicked, this, [this]() { crop_selection(); });
    connect(rotate_left_, &QPushButton::clicked, this, [this]() { transform_selected("rotate-left"); });
    connect(rotate_right_, &QPushButton::clicked, this, [this]() { transform_selected("rotate-right"); });
    connect(undo_, &QPushButton::clicked, this, [this]() { undo_selected(); });
    connect(enhance_, &QPushButton::clicked, this, [this]() { transform_selected("enhance"); });
    connect(binarize_, &QPushButton::clicked, this, [this]() { transform_selected("binarize"); });
    connect(deskew_, &QPushButton::clicked, this, [this]() { transform_selected("deskew"); });
    connect(ocr_button_, &QPushButton::clicked, this, [this]() { run_ocr_selected(); });
    connect(export_, &QPushButton::clicked, this, [this]() { export_pdf(); });
    connect(scanner_, &QComboBox::currentIndexChanged, this,
        [this](int) { refresh_capabilities(); });
    connect(pages_list_, &QListWidget::currentRowChanged, this,
        [this](const int row) {
            update_preview();
            const bool can_undo = row >= 0 && row < static_cast<int>(pages_.size()) &&
                pages_[static_cast<std::size_t>(row)].has_undo;
            undo_->setEnabled(can_undo);
        });

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
                    QStringLiteral("Capabilities loaded — max bed %1 × %2 mm — deskew %3")
                        .arg(caps.max_width_mm, 0, 'f', 1)
                        .arg(caps.max_height_mm, 0, 'f', 1)
                        .arg(processor_.deskew_available() ? QStringLiteral("available")
                                                          : QStringLiteral("unavailable")));
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

    const bool auto_process = auto_process_->isChecked();
    const bool skip_blank = skip_blank_->isChecked();
    const bool can_deskew = processor_.deskew_available();

    add_->setEnabled(false);
    refresh_->setEnabled(false);
    status_->setText(QStringLiteral("Scanning page %1…").arg(pages_.size() + 1U));

    using FramePtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<FramePtr>(this);
    connect(watcher, &QFutureWatcher<FramePtr>::finished, this,
        [this, watcher]() {
            try {
                const auto frame = watcher->result();
                if (!frame) {
                    status_->setText(QStringLiteral("Blank page detected — page skipped"));
                } else {
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
                }
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scan/process error: %1").arg(QString::fromUtf8(error.what())));
            }
            add_->setEnabled(scanner_->count() > 0);
            refresh_->setEnabled(true);
            watcher->deleteLater();
        });

    const std::string name = scanner.toStdString();
    watcher->setFuture(QtConcurrent::run(
        [manager = manager_, name, settings, auto_process, skip_blank, can_deskew]() -> FramePtr {
            ScanFrame frame = manager->scan_backend().scan(name, settings);
            ImageProcessor processor;
            if (skip_blank && processor.is_blank(frame)) {
                return {};
            }
            if (auto_process) {
                frame = automatic_cleanup(std::move(frame), can_deskew);
            }
            return std::make_shared<ScanFrame>(std::move(frame));
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

void DocumentPage::set_page_editing_busy(const bool busy) {
    const bool has_pages = !pages_.empty();
    pages_list_->setEnabled(!busy);
    delete_->setEnabled(!busy && has_pages);
    up_->setEnabled(!busy && has_pages);
    down_->setEnabled(!busy && has_pages);
    clear_->setEnabled(!busy && has_pages);
    auto_crop_->setEnabled(!busy && has_pages);
    crop_selection_->setEnabled(!busy && has_pages);
    rotate_left_->setEnabled(!busy && has_pages);
    rotate_right_->setEnabled(!busy && has_pages);
    enhance_->setEnabled(!busy && has_pages);
    binarize_->setEnabled(!busy && has_pages);
    deskew_->setEnabled(!busy && has_pages && processor_.deskew_available());
    ocr_button_->setEnabled(!busy && has_pages && ocr_engine_.available());
    export_->setEnabled(!busy && has_pages);

    const int row = pages_list_->currentRow();
    const bool can_undo = !busy && row >= 0 && row < static_cast<int>(pages_.size()) &&
        pages_[static_cast<std::size_t>(row)].has_undo;
    undo_->setEnabled(can_undo);
}

void DocumentPage::run_ocr_selected() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size()) || !ocr_engine_.available()) {
        return;
    }

    set_page_editing_busy(true);
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
            set_page_editing_busy(false);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([frame]() {
        TesseractOcr engine;
        return engine.recognize(frame, "fra+eng");
    }));
}

void DocumentPage::transform_selected(const std::string& operation) {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        return;
    }
    if (operation == "deskew" && !processor_.deskew_available()) {
        status_->setText(QStringLiteral("Deskew requires an OpenCV-enabled build"));
        return;
    }

    const ScanFrame input = frame_from_image(
        pages_[static_cast<std::size_t>(row)].image,
        pages_[static_cast<std::size_t>(row)].dpi);

    set_page_editing_busy(true);
    status_->setText(
        QStringLiteral("Processing page %1: %2…")
            .arg(row + 1)
            .arg(QString::fromStdString(operation)));

    using FramePtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<FramePtr>(this);
    connect(watcher, &QFutureWatcher<FramePtr>::finished, this,
        [this, watcher, row, operation]() {
            try {
                const auto frame = watcher->result();
                if (!frame) {
                    throw std::runtime_error("Image processor returned no frame");
                }
                if (row < static_cast<int>(pages_.size())) {
                    QImage image = image_from_frame(*frame);
                    if (image.isNull()) {
                        throw std::runtime_error("Processed frame is empty");
                    }
                    auto& page = pages_[static_cast<std::size_t>(row)];
                    remember_undo(page);
                    page.image = std::move(image);
                    page.dpi = frame->dpi > 0 ? frame->dpi : page.dpi;
                    page.ocr.reset();
                    update_list();
                    pages_list_->setCurrentRow(row);
                    status_->setText(
                        QStringLiteral("Page %1 processed (%2) — Undo available")
                            .arg(row + 1)
                            .arg(QString::fromStdString(operation)));
                }
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Image processing error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            set_page_editing_busy(false);
            watcher->deleteLater();
        });

    watcher->setFuture(QtConcurrent::run([input, operation]() -> FramePtr {
        ImageProcessor processor;
        ScanFrame output;
        if (operation == "crop") {
            output = processor.crop(input, processor.detect_content(input));
        } else if (operation == "enhance") {
            output = processor.enhance_document(input, false);
        } else if (operation == "binarize") {
            output = processor.enhance_document(input, true);
        } else if (operation == "deskew") {
            output = processor.deskew(input);
        } else if (operation == "rotate-left" || operation == "rotate-right") {
            const QImage source = image_from_frame(input);
            if (source.isNull()) {
                throw std::runtime_error("Unable to build image for rotation");
            }
            QTransform transform;
            transform.rotate(operation == "rotate-left" ? -90.0 : 90.0);
            output = frame_from_image(source.transformed(transform), input.dpi);
        } else {
            throw std::runtime_error("Unknown document transform: " + operation);
        }
        return std::make_shared<ScanFrame>(std::move(output));
    }));
}

void DocumentPage::crop_selection() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        return;
    }

    const auto selected = preview_->selected_image_rect();
    if (!selected.has_value()) {
        status_->setText(QStringLiteral(
            "Drag a rectangle over the preview first, then click Crop selection."));
        return;
    }

    const ScanFrame input = frame_from_image(
        pages_[static_cast<std::size_t>(row)].image,
        pages_[static_cast<std::size_t>(row)].dpi);
    const ImageRect rect{
        .x = selected->x(),
        .y = selected->y(),
        .width = selected->width(),
        .height = selected->height(),
    };

    set_page_editing_busy(true);
    status_->setText(
        QStringLiteral("Cropping page %1 to %2 × %3 px…")
            .arg(row + 1)
            .arg(rect.width)
            .arg(rect.height));

    using FramePtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<FramePtr>(this);
    connect(watcher, &QFutureWatcher<FramePtr>::finished, this,
        [this, watcher, row]() {
            try {
                const auto frame = watcher->result();
                if (!frame) {
                    throw std::runtime_error("Manual crop returned no frame");
                }
                if (row < static_cast<int>(pages_.size())) {
                    QImage image = image_from_frame(*frame);
                    if (image.isNull()) {
                        throw std::runtime_error("Manual crop produced an empty image");
                    }
                    auto& page = pages_[static_cast<std::size_t>(row)];
                    remember_undo(page);
                    page.image = std::move(image);
                    page.dpi = frame->dpi > 0 ? frame->dpi : page.dpi;
                    page.ocr.reset();
                    update_list();
                    pages_list_->setCurrentRow(row);
                    status_->setText(
                        QStringLiteral("Page %1 cropped to selection — Undo available")
                            .arg(row + 1));
                }
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Manual crop error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            set_page_editing_busy(false);
            watcher->deleteLater();
        });

    watcher->setFuture(QtConcurrent::run([input, rect]() -> FramePtr {
        ImageProcessor processor;
        return std::make_shared<ScanFrame>(processor.crop(input, rect));
    }));
}

void DocumentPage::undo_selected() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        return;
    }

    auto& page = pages_[static_cast<std::size_t>(row)];
    if (!page.has_undo || page.undo_image.isNull()) {
        status_->setText(QStringLiteral("Nothing to undo on this page"));
        return;
    }

    page.image = std::move(page.undo_image);
    page.dpi = page.undo_dpi;
    page.ocr = std::move(page.undo_ocr);
    page.undo_image = {};
    page.undo_ocr.reset();
    page.has_undo = false;
    update_list();
    pages_list_->setCurrentRow(row);
    status_->setText(QStringLiteral("Restored page %1 before its last transform").arg(row + 1));
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
        if (page.has_undo) {
            label += QStringLiteral(" — undo");
        }
        pages_list_->addItem(label);
    }
    if (!pages_.empty()) {
        pages_list_->setCurrentRow(
            std::clamp(previous, 0, static_cast<int>(pages_.size()) - 1));
    }

    const bool has_pages = !pages_.empty();
    pages_list_->setEnabled(true);
    delete_->setEnabled(has_pages);
    up_->setEnabled(has_pages);
    down_->setEnabled(has_pages);
    clear_->setEnabled(has_pages);
    export_->setEnabled(has_pages);
    auto_crop_->setEnabled(has_pages);
    crop_selection_->setEnabled(has_pages);
    rotate_left_->setEnabled(has_pages);
    rotate_right_->setEnabled(has_pages);
    enhance_->setEnabled(has_pages);
    binarize_->setEnabled(has_pages);
    deskew_->setEnabled(has_pages && processor_.deskew_available());
    ocr_button_->setEnabled(has_pages && ocr_engine_.available());

    const int row = pages_list_->currentRow();
    undo_->setEnabled(
        row >= 0 && row < static_cast<int>(pages_.size()) &&
        pages_[static_cast<std::size_t>(row)].has_undo);
    update_preview();
}

void DocumentPage::update_preview() {
    const int row = pages_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(pages_.size())) {
        preview_->clear_image();
        return;
    }
    preview_->set_image(pages_[static_cast<std::size_t>(row)].image);
}

} // namespace docsuite::desktop
