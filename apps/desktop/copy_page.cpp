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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

[[nodiscard]] QString pretty_keyword(const std::string& value) {
    QString text = QString::fromStdString(value);
    text.replace(QLatin1Char('-'), QLatin1Char(' '));
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    return text;
}

void populate_strings(
    QComboBox* combo,
    const std::vector<std::string>& values,
    const QString& preferred = {},
    const bool allow_auto = false) {

    const QSignalBlocker blocker{combo};
    combo->clear();
    if (allow_auto) {
        combo->addItem(QStringLiteral("Automatic"), QString{});
    }
    for (const auto& value : values) {
        combo->addItem(pretty_keyword(value), QString::fromStdString(value));
    }
    int index = preferred.isEmpty() ? -1 : combo->findData(preferred);
    if (index < 0 && combo->count() > 0) {
        index = 0;
    }
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
    combo->setEnabled(!values.empty() || allow_auto);
}

[[nodiscard]] QString quality_label(const int quality) {
    switch (quality) {
        case 3: return QStringLiteral("Draft");
        case 4: return QStringLiteral("Normal");
        case 5: return QStringLiteral("High");
        default: return QStringLiteral("Quality %1").arg(quality);
    }
}

} // namespace

CopyPage::CopyPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* root = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("Scan → Print"), this);
    auto* form = new QFormLayout(group);

    scanner_ = new QComboBox(group);
    printer_ = new QComboBox(group);
    scan_mode_ = new QComboBox(group);
    dpi_ = new QComboBox(group);
    scan_source_ = new QComboBox(group);
    print_mode_ = new QComboBox(group);
    duplex_ = new QComboBox(group);
    quality_ = new QComboBox(group);
    media_ = new QComboBox(group);
    print_source_ = new QComboBox(group);
    media_type_ = new QComboBox(group);
    copies_ = new QSpinBox(group);
    copies_->setRange(1, 1);
    copies_->setValue(1);

    form->addRow(QStringLiteral("Scanner"), scanner_);
    form->addRow(QStringLiteral("Printer"), printer_);
    form->addRow(QStringLiteral("Scan mode"), scan_mode_);
    form->addRow(QStringLiteral("Scan resolution"), dpi_);
    form->addRow(QStringLiteral("Scan source"), scan_source_);
    form->addRow(QStringLiteral("Print mode"), print_mode_);
    form->addRow(QStringLiteral("Duplex"), duplex_);
    form->addRow(QStringLiteral("Print quality"), quality_);
    form->addRow(QStringLiteral("Paper"), media_);
    form->addRow(QStringLiteral("Paper source"), print_source_);
    form->addRow(QStringLiteral("Media type"), media_type_);
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
    connect(scanner_, &QComboBox::currentIndexChanged, this,
        [this](int) { refresh_scanner_capabilities(); });
    connect(printer_, &QComboBox::currentIndexChanged, this,
        [this](int) { refresh_printer_capabilities(); });
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
                const QSignalBlocker scanner_blocker{scanner_};
                const QSignalBlocker printer_blocker{printer_};
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
                    if (printer.is_default) {
                        label += QStringLiteral(" [default]");
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
                        ? QStringLiteral("Devices ready — loading SANE and IPP capabilities…")
                        : QStringLiteral("A scanner and a printer are required."));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Device discovery error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            watcher->deleteLater();
            refresh_scanner_capabilities();
            refresh_printer_capabilities();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void CopyPage::refresh_scanner_capabilities() {
    const QString scanner = scanner_->currentData().toString();
    if (scanner.isEmpty()) {
        return;
    }
    const std::string name = scanner.toStdString();
    auto* watcher = new QFutureWatcher<ScannerCapabilities>(this);
    connect(watcher, &QFutureWatcher<ScannerCapabilities>::finished, this,
        [this, watcher, scanner]() {
            try {
                if (scanner_->currentData().toString() != scanner) {
                    watcher->deleteLater();
                    return;
                }
                const auto caps = watcher->result();
                populate_strings(scan_mode_, caps.modes, QStringLiteral("Color"));
                populate_strings(scan_source_, caps.sources, QStringLiteral("Flatbed"));
                {
                    const QSignalBlocker blocker{dpi_};
                    dpi_->clear();
                    for (const int dpi : caps.resolutions_dpi) {
                        dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
                    }
                    int preferred = dpi_->findData(300);
                    if (preferred < 0 && dpi_->count() > 0) {
                        preferred = 0;
                    }
                    if (preferred >= 0) {
                        dpi_->setCurrentIndex(preferred);
                    }
                    dpi_->setEnabled(!caps.resolutions_dpi.empty());
                }
                status_->setText(QStringLiteral("Scanner capabilities loaded"));
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

void CopyPage::refresh_printer_capabilities() {
    const QString printer = printer_->currentData().toString();
    if (printer.isEmpty()) {
        return;
    }
    const std::string name = printer.toStdString();
    auto* watcher = new QFutureWatcher<PrinterCapabilities>(this);
    connect(watcher, &QFutureWatcher<PrinterCapabilities>::finished, this,
        [this, watcher, printer]() {
            try {
                if (printer_->currentData().toString() != printer) {
                    watcher->deleteLater();
                    return;
                }
                const auto caps = watcher->result();
                populate_strings(print_mode_, caps.color_modes, QStringLiteral("color"));
                populate_strings(duplex_, caps.sides, QStringLiteral("one-sided"));
                populate_strings(media_, caps.media, QStringLiteral("iso_a4_210x297mm"));
                populate_strings(print_source_, caps.media_sources, {}, true);
                populate_strings(media_type_, caps.media_types, {}, true);

                {
                    const QSignalBlocker blocker{quality_};
                    quality_->clear();
                    for (const int quality : caps.qualities) {
                        quality_->addItem(quality_label(quality), quality);
                    }
                    int preferred = quality_->findData(4);
                    if (preferred < 0 && quality_->count() > 0) {
                        preferred = 0;
                    }
                    if (preferred >= 0) {
                        quality_->setCurrentIndex(preferred);
                    }
                    quality_->setEnabled(!caps.qualities.empty());
                }

                copies_->setRange(
                    std::max(1, caps.copies_min),
                    std::max(std::max(1, caps.copies_min), caps.copies_max));
                copies_->setValue(std::clamp(1, copies_->minimum(), copies_->maximum()));
                status_->setText(
                    QStringLiteral("Printer capabilities loaded — copies %1–%2")
                        .arg(caps.copies_min)
                        .arg(caps.copies_max));
            } catch (const std::exception& error) {
                status_->setText(
                    QStringLiteral("Printer capability error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_, name]() {
        return manager->print_backend().capabilities(name, false);
    }));
}

void CopyPage::start_copy() {
    const QString scanner = scanner_->currentData().toString();
    const QString printer = printer_->currentData().toString();
    if (scanner.isEmpty() || printer.isEmpty()) {
        return;
    }

    ScanSettings settings;
    settings.dpi = dpi_->currentData().toInt();
    if (settings.dpi <= 0) {
        settings.dpi = 300;
    }
    settings.mode = scan_mode_->currentData().toString().toStdString();
    if (settings.mode.empty()) {
        settings.mode = "Color";
    }
    settings.source = scan_source_->currentData().toString().toStdString();
    if (settings.source.empty()) {
        settings.source = "Flatbed";
    }

    PrintProfile profile;
    profile.name = "Copy";
    if (print_mode_->currentIndex() >= 0) {
        profile.color_mode = print_mode_->currentData().toString().toStdString();
    }
    if (duplex_->currentIndex() >= 0) {
        profile.sides = duplex_->currentData().toString().toStdString();
    }
    if (quality_->currentIndex() >= 0) {
        profile.quality = quality_->currentData().toInt();
    }
    if (media_->currentIndex() >= 0) {
        profile.media = media_->currentData().toString().toStdString();
    }
    profile.media_source = print_source_->currentData().toString().toStdString();
    profile.media_type = media_type_->currentData().toString().toStdString();
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
