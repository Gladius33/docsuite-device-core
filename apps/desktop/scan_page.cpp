// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "scan_page.hpp"

#include "pdf_export.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTransform>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>

namespace docsuite::desktop {
namespace {

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

[[nodiscard]] QString human_mode(const std::string& mode) {
    const QString value = QString::fromStdString(mode);
    if (value.compare(QStringLiteral("Gray"), Qt::CaseInsensitive) == 0 ||
        value.contains(QStringLiteral("grey"), Qt::CaseInsensitive) ||
        value.contains(QStringLiteral("gray"), Qt::CaseInsensitive)) {
        return QStringLiteral("Grayscale");
    }
    return value;
}

} // namespace

ScanPage::ScanPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* layout = new QVBoxLayout(this);
    auto* settings_group = new QGroupBox(QStringLiteral("Acquisition"), this);
    auto* settings_layout = new QHBoxLayout(settings_group);
    auto* form = new QFormLayout();

    scanner_ = new QComboBox(settings_group);
    source_ = new QComboBox(settings_group);
    mode_ = new QComboBox(settings_group);
    dpi_ = new QComboBox(settings_group);
    capabilities_ = new QLabel(QStringLiteral("Capabilities not loaded"), settings_group);
    capabilities_->setWordWrap(true);

    form->addRow(QStringLiteral("Scanner"), scanner_);
    form->addRow(QStringLiteral("Source"), source_);
    form->addRow(QStringLiteral("Mode"), mode_);
    form->addRow(QStringLiteral("Resolution"), dpi_);
    form->addRow(QStringLiteral("Device"), capabilities_);
    settings_layout->addLayout(form, 1);

    auto* acquire_buttons = new QVBoxLayout();
    refresh_ = new QPushButton(QStringLiteral("Refresh scanners"), settings_group);
    preview_ = new QPushButton(QStringLiteral("Preview"), settings_group);
    scan_ = new QPushButton(QStringLiteral("Scan"), settings_group);
    acquire_buttons->addWidget(refresh_);
    acquire_buttons->addWidget(preview_);
    acquire_buttons->addWidget(scan_);
    acquire_buttons->addStretch();
    settings_layout->addLayout(acquire_buttons);

    auto* image_tools = new QVBoxLayout();
    rotate_left_ = new QPushButton(QStringLiteral("Rotate left"), settings_group);
    rotate_right_ = new QPushButton(QStringLiteral("Rotate right"), settings_group);
    grayscale_ = new QPushButton(QStringLiteral("Convert to grayscale"), settings_group);
    image_tools->addWidget(rotate_left_);
    image_tools->addWidget(rotate_right_);
    image_tools->addWidget(grayscale_);
    settings_layout->addLayout(image_tools);

    auto* export_buttons = new QVBoxLayout();
    save_png_ = new QPushButton(QStringLiteral("Save PNG"), settings_group);
    save_jpeg_ = new QPushButton(QStringLiteral("Save JPEG"), settings_group);
    save_pdf_ = new QPushButton(QStringLiteral("Save PDF"), settings_group);
    export_buttons->addWidget(save_png_);
    export_buttons->addWidget(save_jpeg_);
    export_buttons->addWidget(save_pdf_);
    settings_layout->addLayout(export_buttons);

    status_ = new QLabel(QStringLiteral("Ready"), this);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    image_label_ = new QLabel(scroll_);
    image_label_->setAlignment(Qt::AlignCenter);
    image_label_->setText(QStringLiteral("No scan yet"));
    scroll_->setWidget(image_label_);

    auto* ocr_group = new QGroupBox(QStringLiteral("OCR"), this);
    auto* ocr_layout = new QVBoxLayout(ocr_group);
    auto* ocr_buttons = new QHBoxLayout();
    ocr_button_ = new QPushButton(QStringLiteral("Recognize fra+eng"), ocr_group);
    copy_ocr_ = new QPushButton(QStringLiteral("Copy text"), ocr_group);
    save_ocr_ = new QPushButton(QStringLiteral("Save text"), ocr_group);
    ocr_buttons->addWidget(ocr_button_);
    ocr_buttons->addWidget(copy_ocr_);
    ocr_buttons->addWidget(save_ocr_);
    ocr_buttons->addStretch();
    ocr_text_ = new QPlainTextEdit(ocr_group);
    ocr_text_->setReadOnly(true);
    ocr_text_->setPlaceholderText(
        QStringLiteral("Run OCR to create searchable text and a searchable PDF layer."));
    ocr_text_->setMaximumHeight(180);
    ocr_layout->addLayout(ocr_buttons);
    ocr_layout->addWidget(ocr_text_);

    layout->addWidget(settings_group);
    layout->addWidget(status_);
    layout->addWidget(scroll_, 1);
    layout->addWidget(ocr_group);

    for (auto* button : {rotate_left_, rotate_right_, grayscale_, save_png_, save_jpeg_,
                         save_pdf_, ocr_button_, copy_ocr_, save_ocr_}) {
        button->setEnabled(false);
    }
    preview_->setEnabled(false);
    scan_->setEnabled(false);
    if (!ocr_.available()) {
        ocr_button_->setToolTip(
            QStringLiteral("Tesseract was not available when DocSuite was built."));
    }

    connect(refresh_, &QPushButton::clicked, this, [this]() { refresh_scanners(); });
    connect(scanner_, &QComboBox::currentIndexChanged, this,
        [this](int) { load_capabilities(); });
    connect(preview_, &QPushButton::clicked, this, [this]() { start_scan(true); });
    connect(scan_, &QPushButton::clicked, this, [this]() { start_scan(false); });
    connect(rotate_left_, &QPushButton::clicked, this, [this]() { rotate(-90); });
    connect(rotate_right_, &QPushButton::clicked, this, [this]() { rotate(90); });
    connect(grayscale_, &QPushButton::clicked, this, [this]() { convert_to_grayscale(); });
    connect(save_png_, &QPushButton::clicked, this,
        [this]() { save_image(QByteArrayLiteral("PNG")); });
    connect(save_jpeg_, &QPushButton::clicked, this,
        [this]() { save_image(QByteArrayLiteral("JPEG")); });
    connect(save_pdf_, &QPushButton::clicked, this, [this]() { save_pdf(); });
    connect(ocr_button_, &QPushButton::clicked, this, [this]() { run_ocr(); });
    connect(copy_ocr_, &QPushButton::clicked, this, [this]() { copy_ocr_text(); });
    connect(save_ocr_, &QPushButton::clicked, this, [this]() { save_ocr_text(); });

    refresh_scanners();
}

void ScanPage::refresh_scanners() {
    refresh_->setEnabled(false);
    preview_->setEnabled(false);
    scan_->setEnabled(false);
    status_->setText(QStringLiteral("Discovering scanners…"));

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher]() {
            try {
                const QString previous = scanner_->currentData().toString();
                const auto snapshot = watcher->result();
                scanner_->blockSignals(true);
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
                scanner_->blockSignals(false);

                const bool available = scanner_->count() > 0;
                status_->setText(
                    available
                        ? QStringLiteral("%1 scanner(s) discovered — loading capabilities…")
                              .arg(scanner_->count())
                        : QStringLiteral("No scanner discovered"));
                if (available) {
                    load_capabilities();
                } else {
                    source_->clear();
                    mode_->clear();
                    dpi_->clear();
                    capabilities_->setText(QStringLiteral("No scanner"));
                }
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scanner discovery error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void ScanPage::load_capabilities() {
    const QString scanner_name = scanner_->currentData().toString();
    if (scanner_name.isEmpty()) {
        return;
    }

    preview_->setEnabled(false);
    scan_->setEnabled(false);
    capabilities_->setText(QStringLiteral("Loading…"));
    const std::string name = scanner_name.toStdString();

    auto* watcher = new QFutureWatcher<ScannerCapabilities>(this);
    connect(watcher, &QFutureWatcher<ScannerCapabilities>::finished, this,
        [this, watcher, scanner_name]() {
            try {
                if (scanner_->currentData().toString() != scanner_name) {
                    watcher->deleteLater();
                    return;
                }
                const auto caps = watcher->result();

                const QString old_source = source_->currentData().toString();
                const QString old_mode = mode_->currentData().toString();
                const int old_dpi = dpi_->currentData().toInt();

                source_->clear();
                mode_->clear();
                dpi_->clear();

                for (const auto& source : caps.sources) {
                    source_->addItem(QString::fromStdString(source), QString::fromStdString(source));
                }
                if (source_->count() == 0) {
                    source_->addItem(QStringLiteral("Flatbed"), QStringLiteral("Flatbed"));
                }

                for (const auto& mode : caps.modes) {
                    mode_->addItem(human_mode(mode), QString::fromStdString(mode));
                }
                if (mode_->count() == 0) {
                    mode_->addItem(QStringLiteral("Color"), QStringLiteral("Color"));
                }

                for (const int dpi : caps.resolutions_dpi) {
                    dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
                }
                if (dpi_->count() == 0) {
                    for (const int dpi : {150, 300, 600}) {
                        dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
                    }
                }

                const int source_index = source_->findData(old_source);
                if (source_index >= 0) {
                    source_->setCurrentIndex(source_index);
                }
                const int mode_index = mode_->findData(old_mode);
                if (mode_index >= 0) {
                    mode_->setCurrentIndex(mode_index);
                } else {
                    const int color = mode_->findData(QStringLiteral("Color"));
                    if (color >= 0) {
                        mode_->setCurrentIndex(color);
                    }
                }
                int dpi_index = dpi_->findData(old_dpi > 0 ? old_dpi : 300);
                if (dpi_index < 0) {
                    dpi_index = dpi_->findData(300);
                }
                if (dpi_index >= 0) {
                    dpi_->setCurrentIndex(dpi_index);
                }

                capabilities_->setText(
                    QStringLiteral("SANE • bed up to %1 × %2 mm • %3 mode(s) • %4 resolution(s)")
                        .arg(caps.max_width_mm, 0, 'f', 1)
                        .arg(caps.max_height_mm, 0, 'f', 1)
                        .arg(caps.modes.size())
                        .arg(caps.resolutions_dpi.size()));
                status_->setText(QStringLiteral("Scanner ready"));
                preview_->setEnabled(true);
                scan_->setEnabled(true);
            } catch (const std::exception& error) {
                capabilities_->setText(QStringLiteral("Capabilities unavailable"));
                status_->setText(
                    QStringLiteral("Capability query error: %1")
                        .arg(QString::fromUtf8(error.what())));
                preview_->setEnabled(true);
                scan_->setEnabled(true);
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_, name]() {
        return manager->scan_backend().capabilities(name);
    }));
}

void ScanPage::start_scan(const bool preview) {
    const QString scanner_name = scanner_->currentData().toString();
    if (scanner_name.isEmpty()) {
        return;
    }

    ScanSettings settings;
    const int selected_dpi = dpi_->currentData().toInt();
    if (preview) {
        int preview_dpi = selected_dpi > 0 ? selected_dpi : 150;
        for (int index = 0; index < dpi_->count(); ++index) {
            const int candidate = dpi_->itemData(index).toInt();
            if (candidate > 0 && candidate <= 150) {
                preview_dpi = std::max(preview_dpi > 150 ? 0 : preview_dpi, candidate);
            }
        }
        settings.dpi = preview_dpi > 0 ? preview_dpi : selected_dpi;
    } else {
        settings.dpi = selected_dpi;
    }
    settings.mode = mode_->currentData().toString().toStdString();
    settings.source = source_->currentData().toString().toStdString();

    refresh_->setEnabled(false);
    preview_->setEnabled(false);
    scan_->setEnabled(false);
    status_->setText(
        QStringLiteral("Scanning at %1 dpi in %2…")
            .arg(settings.dpi)
            .arg(mode_->currentText()));

    using FramePtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<FramePtr>(this);
    connect(watcher, &QFutureWatcher<FramePtr>::finished, this,
        [this, watcher]() {
            try {
                const auto frame = watcher->result();
                set_scan_frame(frame);
                status_->setText(
                    QStringLiteral("Scan complete — %1 × %2 px — %3 dpi — %4 MiB")
                        .arg(frame->width)
                        .arg(frame->height)
                        .arg(frame->dpi)
                        .arg(static_cast<double>(frame->pixels.size()) / (1024.0 * 1024.0), 0, 'f', 1));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Scan error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            preview_->setEnabled(scanner_->count() > 0);
            scan_->setEnabled(scanner_->count() > 0);
            watcher->deleteLater();
        });

    const std::string device = scanner_name.toStdString();
    watcher->setFuture(QtConcurrent::run([manager = manager_, device, settings]() {
        return std::make_shared<ScanFrame>(manager->scan_backend().scan(device, settings));
    }));
}

void ScanPage::set_scan_frame(const std::shared_ptr<ScanFrame>& frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->pixels.empty()) {
        throw std::runtime_error("Scanner returned an empty frame");
    }

    const bool rgb = frame->format == ScanPixelFormat::rgb24;
    const int channels = rgb ? 3 : 1;
    frame_backing_ = frame;
    image_ = QImage(
        static_cast<const uchar*>(frame->pixels.data()),
        frame->width,
        frame->height,
        frame->width * channels,
        rgb ? QImage::Format_RGB888 : QImage::Format_Grayscale8);
    image_dpi_ = frame->dpi;
    ocr_result_.reset();
    ocr_text_->clear();

    for (auto* button : {rotate_left_, rotate_right_, grayscale_, save_png_, save_jpeg_, save_pdf_}) {
        button->setEnabled(true);
    }
    ocr_button_->setEnabled(ocr_.available());
    copy_ocr_->setEnabled(false);
    save_ocr_->setEnabled(false);
    update_preview();
}

void ScanPage::update_preview() {
    if (image_.isNull()) {
        return;
    }
    QSize area = scroll_->viewport()->size() - QSize(24, 24);
    if (area.width() < 100 || area.height() < 100) {
        area = QSize(900, 650);
    }
    image_label_->setPixmap(
        QPixmap::fromImage(image_).scaled(area, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ScanPage::rotate(const int degrees) {
    if (image_.isNull()) {
        return;
    }
    QTransform transform;
    transform.rotate(degrees);
    image_ = image_.transformed(transform, Qt::SmoothTransformation);
    frame_backing_.reset();
    ocr_result_.reset();
    ocr_text_->clear();
    copy_ocr_->setEnabled(false);
    save_ocr_->setEnabled(false);
    update_preview();
    status_->setText(QStringLiteral("Image rotated — rerun OCR before searchable PDF export."));
}

void ScanPage::convert_to_grayscale() {
    if (image_.isNull()) {
        return;
    }
    image_ = image_.convertToFormat(QImage::Format_Grayscale8);
    frame_backing_.reset();
    ocr_result_.reset();
    ocr_text_->clear();
    copy_ocr_->setEnabled(false);
    save_ocr_->setEnabled(false);
    update_preview();
    status_->setText(QStringLiteral("Image converted to grayscale."));
}

void ScanPage::save_image(const QByteArray& format) {
    if (image_.isNull()) {
        return;
    }
    const bool jpeg = format == QByteArrayLiteral("JPEG");
    const QString extension = jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    const QString filter = jpeg
        ? QStringLiteral("JPEG image (*.jpg *.jpeg)")
        : QStringLiteral("PNG image (*.png)");
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save scanned image"),
        QStringLiteral("scan.%1").arg(extension),
        filter);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".") + extension;
    }
    if (!image_.save(path, format.constData(), jpeg ? 92 : -1)) {
        QMessageBox::critical(
            this,
            QStringLiteral("DocSuite Scan"),
            QStringLiteral("Unable to save %1").arg(path));
        return;
    }
    status_->setText(QStringLiteral("Saved %1").arg(path));
}

void ScanPage::save_pdf() {
    if (image_.isNull()) {
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save PDF"),
        QStringLiteral("scan.pdf"),
        QStringLiteral("PDF document (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".pdf");
    }

    QString error;
    if (!export_scan_pdf(path, image_, image_dpi_, ocr_result_, &error)) {
        QMessageBox::critical(this, QStringLiteral("DocSuite PDF"), error);
        return;
    }
    status_->setText(
        ocr_result_.has_value()
            ? QStringLiteral("Saved searchable PDF %1").arg(path)
            : QStringLiteral("Saved image PDF %1 (run OCR first for searchable text)").arg(path));
}

ScanFrame ScanPage::frame_for_ocr() const {
    if (frame_backing_ &&
        frame_backing_->width == image_.width() && frame_backing_->height == image_.height()) {
        return *frame_backing_;
    }
    return frame_from_image(image_, image_dpi_);
}

void ScanPage::run_ocr() {
    if (image_.isNull() || !ocr_.available()) {
        return;
    }
    ocr_button_->setEnabled(false);
    ocr_text_->setPlainText(QStringLiteral("OCR running…"));
    status_->setText(QStringLiteral("Recognizing text with Tesseract fra+eng…"));

    const ScanFrame frame = frame_for_ocr();
    auto* watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this,
        [this, watcher]() {
            try {
                ocr_result_ = watcher->result();
                ocr_text_->setPlainText(QString::fromStdString(ocr_result_->text));
                copy_ocr_->setEnabled(true);
                save_ocr_->setEnabled(true);
                status_->setText(
                    QStringLiteral("OCR complete — confidence %1% — %2 words — PDF export is searchable")
                        .arg(ocr_result_->mean_confidence)
                        .arg(ocr_result_->words.size()));
            } catch (const std::exception& error) {
                ocr_result_.reset();
                ocr_text_->setPlainText(
                    QStringLiteral("OCR error: %1").arg(QString::fromUtf8(error.what())));
                status_->setText(QStringLiteral("OCR failed"));
            }
            ocr_button_->setEnabled(ocr_.available() && !image_.isNull());
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([ocr = ocr_, frame]() {
        return ocr.recognize(frame, "fra+eng");
    }));
}

void ScanPage::copy_ocr_text() {
    if (!ocr_result_.has_value()) {
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(ocr_result_->text));
    status_->setText(QStringLiteral("OCR text copied to clipboard."));
}

void ScanPage::save_ocr_text() {
    if (!ocr_result_.has_value()) {
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save OCR text"),
        QStringLiteral("scan.txt"),
        QStringLiteral("Text file (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".txt");
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("DocSuite OCR"),
            QStringLiteral("Unable to create %1").arg(path));
        return;
    }
    file.write(QByteArray::fromStdString(ocr_result_->text));
    file.close();
    status_->setText(QStringLiteral("Saved OCR text %1").arg(path));
}

void ScanPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update_preview();
}

} // namespace docsuite::desktop
