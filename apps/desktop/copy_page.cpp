// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "copy_page.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

namespace docsuite::desktop {
namespace {

[[nodiscard]] QImage image_from_frame(const ScanFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        throw std::runtime_error("Copy scan returned an empty image");
    }
    const bool rgb = frame.format == ScanPixelFormat::rgb24;
    const int channels = rgb ? 3 : 1;
    const QImage view(
        static_cast<const uchar*>(frame.pixels.data()),
        frame.width,
        frame.height,
        frame.width * channels,
        rgb ? QImage::Format_RGB888 : QImage::Format_Grayscale8);
    return view.copy();
}

struct CopyResult {
    int job_id{0};
    int copies_requested{0};
    int width{0};
    int height{0};
};

} // namespace

CopyPage::CopyPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* root = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("Scan → Print"), this);
    auto* form = new QFormLayout(group);

    scanner_ = new QComboBox(group);
    printer_ = new QComboBox(group);
    scan_mode_ = new QComboBox(group);
    scan_mode_->addItem(QStringLiteral("Color"), QStringLiteral("Color"));
    scan_mode_->addItem(QStringLiteral("Grayscale"), QStringLiteral("Gray"));
    dpi_ = new QComboBox(group);
    for (const int dpi : {150, 300, 600}) {
        dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
    }
    dpi_->setCurrentIndex(1);
    print_mode_ = new QComboBox(group);
    print_mode_->addItem(QStringLiteral("Color"), QStringLiteral("color"));
    print_mode_->addItem(QStringLiteral("Monochrome"), QStringLiteral("monochrome"));
    duplex_ = new QComboBox(group);
    duplex_->addItem(QStringLiteral("Single-sided"), QStringLiteral("one-sided"));
    duplex_->addItem(QStringLiteral("Duplex long edge"), QStringLiteral("two-sided-long-edge"));
    duplex_->addItem(QStringLiteral("Duplex short edge"), QStringLiteral("two-sided-short-edge"));
    copies_ = new QSpinBox(group);
    copies_->setRange(1, 99);
    copies_->setValue(1);

    form->addRow(QStringLiteral("Scanner"), scanner_);
    form->addRow(QStringLiteral("Printer"), printer_);
    form->addRow(QStringLiteral("Scan mode"), scan_mode_);
    form->addRow(QStringLiteral("Resolution"), dpi_);
    form->addRow(QStringLiteral("Print mode"), print_mode_);
    form->addRow(QStringLiteral("Duplex"), duplex_);
    form->addRow(QStringLiteral("Copies"), copies_);

    auto* actions = new QHBoxLayout();
    refresh_ = new QPushButton(QStringLiteral("Refresh devices"), group);
    copy_ = new QPushButton(QStringLiteral("Copy now"), group);
    actions->addWidget(refresh_);
    actions->addWidget(copy_);
    actions->addStretch();
    form->addRow(actions);

    status_ = new QLabel(QStringLiteral("Ready"), this);
    status_->setWordWrap(true);
    root->addWidget(group);
    root->addWidget(status_);
    root->addStretch();

    connect(refresh_, &QPushButton::clicked, this, [this]() { refresh_devices(); });
    connect(copy_, &QPushButton::clicked, this, [this]() { start_copy(); });
    connect(scan_mode_, &QComboBox::currentIndexChanged, this, [this](int) {
        const bool gray = scan_mode_->currentData().toString().contains(
            QStringLiteral("gray"), Qt::CaseInsensitive);
        if (gray) {
            const int mono = print_mode_->findData(QStringLiteral("monochrome"));
            if (mono >= 0) {
                print_mode_->setCurrentIndex(mono);
            }
        }
    });

    refresh_devices();
}

void CopyPage::refresh_devices() {
    refresh_->setEnabled(false);
    copy_->setEnabled(false);
    status_->setText(QStringLiteral("Discovering scanners and printers…"));
    const QString old_scanner = scanner_->currentData().toString();
    const QString old_printer = printer_->currentData().toString();

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher, old_scanner, old_printer]() {
            try {
                const auto snapshot = watcher->result();
                scanner_->clear();
                printer_->clear();

                for (const auto& scanner : snapshot.scanners) {
                    scanner_->addItem(
                        QStringLiteral("%1 %2 — %3")
                            .arg(QString::fromStdString(scanner.vendor))
                            .arg(QString::fromStdString(scanner.model))
                            .arg(QString::fromStdString(scanner.backend)),
                        QString::fromStdString(scanner.name));
                }
                for (const auto& printer : snapshot.printers) {
                    QString label = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        label += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    printer_->addItem(label, QString::fromStdString(printer.name));
                }

                const int scanner_index = scanner_->findData(old_scanner);
                if (scanner_index >= 0) scanner_->setCurrentIndex(scanner_index);
                const int printer_index = printer_->findData(old_printer);
                if (printer_index >= 0) printer_->setCurrentIndex(printer_index);

                const bool ready = scanner_->count() > 0 && printer_->count() > 0;
                copy_->setEnabled(ready);
                status_->setText(
                    ready
                        ? QStringLiteral("Ready — the source is scanned once and submitted as one CUPS job with the requested copy count.")
                        : QStringLiteral("A scanner and a printer are required."));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Device discovery error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void CopyPage::start_copy() {
    const QString scanner = scanner_->currentData().toString();
    const QString printer = printer_->currentData().toString();
    if (scanner.isEmpty() || printer.isEmpty()) {
        return;
    }

    ScanSettings settings;
    settings.dpi = dpi_->currentData().toInt();
    settings.mode = scan_mode_->currentData().toString().toStdString();
    settings.source = "Flatbed";

    PrintProfile profile;
    profile.name = "Copy";
    profile.color_mode = print_mode_->currentData().toString().toStdString();
    profile.sides = duplex_->currentData().toString().toStdString();
    profile.quality = 4;
    profile.copies = copies_->value();

    copy_->setEnabled(false);
    refresh_->setEnabled(false);
    status_->setText(QStringLiteral("Scanning source page…"));

    auto* watcher = new QFutureWatcher<CopyResult>(this);
    connect(watcher, &QFutureWatcher<CopyResult>::finished, this,
        [this, watcher]() {
            try {
                const auto result = watcher->result();
                status_->setText(
                    QStringLiteral("Copy submitted — job #%1, %2 copy/copies, source %3 × %4 px")
                        .arg(result.job_id)
                        .arg(result.copies_requested)
                        .arg(result.width)
                        .arg(result.height));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Copy error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            copy_->setEnabled(scanner_->count() > 0 && printer_->count() > 0);
            watcher->deleteLater();
        });

    const std::string scanner_name = scanner.toStdString();
    const std::string printer_name = printer.toStdString();
    watcher->setFuture(QtConcurrent::run(
        [manager = manager_, scanner_name, printer_name, settings, profile]() {
            const ScanFrame frame = manager->scan_backend().scan(scanner_name, settings);
            const QImage image = image_from_frame(frame);

            QTemporaryDir directory;
            if (!directory.isValid()) {
                throw std::runtime_error("Unable to create temporary copy directory");
            }
            const QString path = directory.filePath(QStringLiteral("copy.png"));
            if (!image.save(path, "PNG")) {
                throw std::runtime_error("Unable to encode temporary copy image");
            }

            CopyResult result;
            result.width = frame.width;
            result.height = frame.height;
            result.copies_requested = profile.copies;
            result.job_id = manager->print_backend().print_file_advanced(
                printer_name,
                path.toStdString(),
                "DocSuite copy",
                profile);
            return result;
        }));
}

} // namespace docsuite::desktop
