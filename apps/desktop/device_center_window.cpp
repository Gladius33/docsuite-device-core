// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "device_center_window.hpp"

#include "docsuite/print/job_manager.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTextEdit>
#include <QTransform>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace docsuite::desktop {
namespace {

struct PrinterDetails {
    PrinterCapabilities capabilities;
    PrinterStatus status;
};

[[nodiscard]] QString device_state_name(const DeviceState state) {
    switch (state) {
        case DeviceState::idle: return QStringLiteral("Idle");
        case DeviceState::processing: return QStringLiteral("Printing");
        case DeviceState::stopped: return QStringLiteral("Stopped");
        case DeviceState::offline: return QStringLiteral("Offline");
        case DeviceState::unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] QString join_strings(const std::vector<std::string>& values) {
    QStringList list;
    for (const auto& value : values) {
        list.push_back(QString::fromStdString(value));
    }
    return list.isEmpty() ? QStringLiteral("Not reported") : list.join(QStringLiteral(", "));
}

[[nodiscard]] QString join_ints(const std::vector<int>& values) {
    QStringList list;
    for (const int value : values) {
        list.push_back(QString::number(value));
    }
    return list.isEmpty() ? QStringLiteral("Not reported") : list.join(QStringLiteral(", "));
}

[[nodiscard]] QString printer_details_text(const PrinterDetails& details) {
    const auto& caps = details.capabilities;
    const auto& status = details.status;

    QString text;
    text += QStringLiteral("Status source: %1\n").arg(QString::fromStdString(status.source));
    text += QStringLiteral("Capability source: %1\n").arg(QString::fromStdString(caps.source));
    text += QStringLiteral("State: %1\n").arg(device_state_name(status.state));
    text += QStringLiteral("Accepting jobs: %1\n")
        .arg(status.accepting_jobs ? QStringLiteral("yes") : QStringLiteral("no"));
    text += QStringLiteral("Reasons: %1\n\n").arg(join_strings(status.reasons));

    text += QStringLiteral("Capabilities\n");
    text += QStringLiteral("  Color: %1\n").arg(join_strings(caps.color_modes));
    text += QStringLiteral("  Duplex: %1\n").arg(join_strings(caps.sides));
    text += QStringLiteral("  Quality: %1\n").arg(join_ints(caps.qualities));
    text += QStringLiteral("  Resolution DPI: %1\n").arg(join_ints(caps.resolutions_dpi));
    text += QStringLiteral("  Copies: %1-%2\n").arg(caps.copies_min).arg(caps.copies_max);
    text += QStringLiteral("  Sources: %1\n").arg(join_strings(caps.media_sources));
    text += QStringLiteral("  Media types: %1\n").arg(join_strings(caps.media_types));
    text += QStringLiteral("  Document formats: %1\n").arg(join_strings(caps.document_formats));
    text += QStringLiteral("  Media sizes: %1 advertised\n\n").arg(caps.media.size());

    text += QStringLiteral("Supplies\n");
    if (status.supplies.empty()) {
        text += QStringLiteral("  Not reported by device\n");
    }
    for (const auto& supply : status.supplies) {
        text += QStringLiteral("  ") + QString::fromStdString(supply.name) + QStringLiteral(": ");
        if (supply.percent.has_value()) {
            text += QString::number(*supply.percent) + QStringLiteral("%");
            if (*supply.percent <= supply.low_threshold) {
                text += QStringLiteral(" (low)");
            }
        } else {
            text += QStringLiteral("level unavailable");
        }
        text += QLatin1Char('\n');
    }

    return text;
}

[[nodiscard]] QString optional_duration_text(
    const std::optional<std::chrono::milliseconds>& duration) {
    if (!duration.has_value()) {
        return QStringLiteral("not reported");
    }
    return QStringLiteral("%1 ms").arg(duration->count());
}

[[nodiscard]] QString trace_text(const PrintJobTrace& trace) {
    QString text;
    text += QStringLiteral("Job #%1\n").arg(trace.job_id);
    text += QStringLiteral("Printer: %1\n").arg(QString::fromStdString(trace.printer));
    text += QStringLiteral("Final state: %1\n")
        .arg(QString::fromLatin1(print_job_state_name(trace.final_state)));
    text += QStringLiteral("Submit -> CUPS accepted: %1 ms\n\n").arg(trace.submit_to_accept.count());
    text += QStringLiteral("Timeline\n");
    for (const auto& event : trace.events) {
        text += QStringLiteral("  +%1 ms | %2 | %3\n")
            .arg(event.since_submit.count())
            .arg(QString::fromStdString(event.name))
            .arg(QString::fromLatin1(print_job_state_name(event.state)));
    }
    text += QStringLiteral("\nCUPS queue delay: %1\n").arg(optional_duration_text(trace.queue_delay));
    text += QStringLiteral("CUPS processing duration: %1\n")
        .arg(optional_duration_text(trace.processing_duration));
    text += QStringLiteral("CUPS total duration: %1\n")
        .arg(optional_duration_text(trace.total_duration));
    text += QStringLiteral("Timed out: %1\n")
        .arg(trace.timed_out ? QStringLiteral("yes") : QStringLiteral("no"));
    if (!trace.history_path.empty()) {
        text += QStringLiteral("History: %1\n").arg(QString::fromStdString(trace.history_path));
    }
    return text;
}

[[nodiscard]] ScanFrame frame_from_qimage(const QImage& source, const int dpi) {
    const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
    ScanFrame frame;
    frame.width = rgb.width();
    frame.height = rgb.height();
    frame.dpi = dpi;
    frame.format = ScanPixelFormat::rgb24;
    const int packed_stride = frame.width * 3;
    frame.pixels.resize(
        static_cast<std::size_t>(packed_stride) * static_cast<std::size_t>(frame.height));

    for (int y = 0; y < frame.height; ++y) {
        const auto* source_line = rgb.constScanLine(y);
        auto* destination = frame.pixels.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(packed_stride);
        std::copy_n(source_line, packed_stride, destination);
    }
    return frame;
}

} // namespace

DeviceCenterWindow::DeviceCenterWindow(QWidget* parent)
    : QMainWindow{parent}, manager_{std::make_shared<DeviceManager>()} {
    build_ui();
    reload_devices();
}

void DeviceCenterWindow::build_ui() {
    setWindowTitle(QStringLiteral("DocSuite Device Center"));
    resize(1240, 820);

    tabs_ = new QTabWidget(this);
    setCentralWidget(tabs_);

    // Devices tab
    auto* devices_page = new QWidget(tabs_);
    auto* devices_layout = new QVBoxLayout(devices_page);
    auto* devices_toolbar = new QHBoxLayout();
    device_summary_ = new QLabel(QStringLiteral("Discovering devices…"), devices_page);
    device_refresh_ = new QPushButton(QStringLiteral("Refresh devices"), devices_page);
    devices_toolbar->addWidget(device_summary_, 1);
    devices_toolbar->addWidget(device_refresh_);

    auto* devices_splitter = new QSplitter(Qt::Horizontal, devices_page);
    auto* lists_widget = new QWidget(devices_splitter);
    auto* lists_layout = new QVBoxLayout(lists_widget);
    device_printers_ = new QListWidget(lists_widget);
    device_scanners_ = new QListWidget(lists_widget);
    lists_layout->addWidget(new QLabel(QStringLiteral("Printers"), lists_widget));
    lists_layout->addWidget(device_printers_);
    lists_layout->addWidget(new QLabel(QStringLiteral("Scanners"), lists_widget));
    lists_layout->addWidget(device_scanners_);

    device_details_ = new QTextEdit(devices_splitter);
    device_details_->setReadOnly(true);
    device_details_->setPlaceholderText(
        QStringLiteral("Select a printer to load physical IPP status and capabilities."));
    devices_splitter->addWidget(lists_widget);
    devices_splitter->addWidget(device_details_);
    devices_splitter->setStretchFactor(0, 1);
    devices_splitter->setStretchFactor(1, 2);

    devices_layout->addLayout(devices_toolbar);
    devices_layout->addWidget(devices_splitter, 1);
    tabs_->addTab(devices_page, QStringLiteral("Devices"));

    // Scan tab
    auto* scan_page = new QWidget(tabs_);
    auto* scan_layout = new QVBoxLayout(scan_page);
    auto* scan_controls = new QGroupBox(QStringLiteral("Scan settings"), scan_page);
    auto* scan_controls_layout = new QHBoxLayout(scan_controls);
    auto* scan_form = new QFormLayout();
    scan_scanner_ = new QComboBox(scan_controls);
    scan_mode_ = new QComboBox(scan_controls);
    scan_mode_->addItem(QStringLiteral("Color"), QStringLiteral("Color"));
    scan_mode_->addItem(QStringLiteral("Grayscale"), QStringLiteral("Gray"));
    scan_dpi_ = new QComboBox(scan_controls);
    for (const int dpi : {150, 300, 600}) {
        scan_dpi_->addItem(QStringLiteral("%1 dpi").arg(dpi), dpi);
    }
    scan_dpi_->setCurrentIndex(1);
    scan_form->addRow(QStringLiteral("Scanner"), scan_scanner_);
    scan_form->addRow(QStringLiteral("Mode"), scan_mode_);
    scan_form->addRow(QStringLiteral("Resolution"), scan_dpi_);
    scan_controls_layout->addLayout(scan_form, 1);

    auto* scan_actions = new QVBoxLayout();
    scan_preview_button_ = new QPushButton(QStringLiteral("Preview (150 dpi)"), scan_controls);
    scan_acquire_button_ = new QPushButton(QStringLiteral("Scan"), scan_controls);
    scan_actions->addWidget(scan_preview_button_);
    scan_actions->addWidget(scan_acquire_button_);
    scan_actions->addStretch();
    scan_controls_layout->addLayout(scan_actions);

    auto* scan_export = new QVBoxLayout();
    scan_save_png_ = new QPushButton(QStringLiteral("Save PNG"), scan_controls);
    scan_save_jpeg_ = new QPushButton(QStringLiteral("Save JPEG"), scan_controls);
    scan_save_pdf_ = new QPushButton(QStringLiteral("Save PDF"), scan_controls);
    scan_export->addWidget(scan_save_png_);
    scan_export->addWidget(scan_save_jpeg_);
    scan_export->addWidget(scan_save_pdf_);
    scan_controls_layout->addLayout(scan_export);

    auto* scan_tools = new QVBoxLayout();
    scan_rotate_left_ = new QPushButton(QStringLiteral("Rotate left"), scan_controls);
    scan_rotate_right_ = new QPushButton(QStringLiteral("Rotate right"), scan_controls);
    scan_ocr_button_ = new QPushButton(QStringLiteral("OCR (fra+eng)"), scan_controls);
    scan_tools->addWidget(scan_rotate_left_);
    scan_tools->addWidget(scan_rotate_right_);
    scan_tools->addWidget(scan_ocr_button_);
    scan_controls_layout->addLayout(scan_tools);

    scan_status_ = new QLabel(QStringLiteral("Ready"), scan_page);
    scan_scroll_ = new QScrollArea(scan_page);
    scan_scroll_->setWidgetResizable(true);
    scan_image_ = new QLabel(scan_scroll_);
    scan_image_->setAlignment(Qt::AlignCenter);
    scan_image_->setText(QStringLiteral("No scan yet"));
    scan_scroll_->setWidget(scan_image_);

    scan_ocr_text_ = new QPlainTextEdit(scan_page);
    scan_ocr_text_->setReadOnly(true);
    scan_ocr_text_->setPlaceholderText(QStringLiteral("OCR text will appear here."));
    scan_ocr_text_->setMaximumHeight(180);

    scan_layout->addWidget(scan_controls);
    scan_layout->addWidget(scan_status_);
    scan_layout->addWidget(scan_scroll_, 1);
    scan_layout->addWidget(scan_ocr_text_);
    tabs_->addTab(scan_page, QStringLiteral("Scan / OCR"));

    // Print / jobs tab
    auto* print_page = new QWidget(tabs_);
    auto* print_layout = new QVBoxLayout(print_page);
    auto* print_group = new QGroupBox(QStringLiteral("Print"), print_page);
    auto* print_form = new QFormLayout(print_group);
    print_printer_ = new QComboBox(print_group);
    print_file_ = new QLineEdit(print_group);
    auto* file_row = new QWidget(print_group);
    auto* file_row_layout = new QHBoxLayout(file_row);
    file_row_layout->setContentsMargins(0, 0, 0, 0);
    auto* browse_button = new QPushButton(QStringLiteral("Browse…"), file_row);
    file_row_layout->addWidget(print_file_, 1);
    file_row_layout->addWidget(browse_button);
    print_profile_ = new QComboBox(print_group);
    print_profile_->addItem(QStringLiteral("Standard color"), QStringLiteral("color"));
    print_profile_->addItem(QStringLiteral("Document monochrome"), QStringLiteral("mono"));
    print_profile_->addItem(QStringLiteral("Economy mono duplex"), QStringLiteral("economy"));
    print_profile_->addItem(QStringLiteral("High quality color"), QStringLiteral("high"));
    print_form->addRow(QStringLiteral("Printer"), print_printer_);
    print_form->addRow(QStringLiteral("File"), file_row);
    print_form->addRow(QStringLiteral("Profile"), print_profile_);

    auto* print_buttons = new QHBoxLayout();
    print_submit_ = new QPushButton(QStringLiteral("Print"), print_group);
    print_diagnose_ = new QPushButton(QStringLiteral("Print + diagnose timing"), print_group);
    print_buttons->addWidget(print_submit_);
    print_buttons->addWidget(print_diagnose_);
    print_buttons->addStretch();
    print_form->addRow(print_buttons);

    auto* jobs_group = new QGroupBox(QStringLiteral("Jobs"), print_page);
    auto* jobs_layout = new QVBoxLayout(jobs_group);
    auto* jobs_buttons = new QHBoxLayout();
    jobs_refresh_ = new QPushButton(QStringLiteral("Refresh jobs"), jobs_group);
    jobs_cancel_ = new QPushButton(QStringLiteral("Cancel selected"), jobs_group);
    jobs_buttons->addWidget(jobs_refresh_);
    jobs_buttons->addWidget(jobs_cancel_);
    jobs_buttons->addStretch();
    jobs_table_ = new QTableWidget(0, 5, jobs_group);
    jobs_table_->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("State"), QStringLiteral("Title"),
         QStringLiteral("Size"), QStringLiteral("Format")});
    jobs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobs_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    jobs_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobs_table_->horizontalHeader()->setStretchLastSection(true);
    jobs_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    jobs_layout->addLayout(jobs_buttons);
    jobs_layout->addWidget(jobs_table_);

    print_log_ = new QPlainTextEdit(print_page);
    print_log_->setReadOnly(true);
    print_log_->setPlaceholderText(QStringLiteral("Print diagnostics and errors appear here."));
    print_log_->setMaximumBlockCount(1000);

    print_layout->addWidget(print_group);
    print_layout->addWidget(jobs_group, 1);
    print_layout->addWidget(print_log_);
    tabs_->addTab(print_page, QStringLiteral("Print / Jobs"));

    scan_save_png_->setEnabled(false);
    scan_save_jpeg_->setEnabled(false);
    scan_save_pdf_->setEnabled(false);
    scan_rotate_left_->setEnabled(false);
    scan_rotate_right_->setEnabled(false);
    scan_ocr_button_->setEnabled(false);
    if (!ocr_.available()) {
        scan_ocr_button_->setToolTip(
            QStringLiteral("Rebuild with libtesseract-dev to enable OCR."));
    }

    connect(device_refresh_, &QPushButton::clicked, this, [this]() { reload_devices(); });
    connect(device_printers_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current, QListWidgetItem*) {
            if (current == nullptr) {
                device_details_->clear();
                return;
            }
            load_printer_details(current->data(Qt::UserRole).toString());
        });

    connect(scan_preview_button_, &QPushButton::clicked, this, [this]() { start_scan(true); });
    connect(scan_acquire_button_, &QPushButton::clicked, this, [this]() { start_scan(false); });
    connect(scan_save_png_, &QPushButton::clicked, this,
        [this]() { save_scan_image(QByteArrayLiteral("PNG")); });
    connect(scan_save_jpeg_, &QPushButton::clicked, this,
        [this]() { save_scan_image(QByteArrayLiteral("JPEG")); });
    connect(scan_save_pdf_, &QPushButton::clicked, this, [this]() { save_scan_pdf(); });
    connect(scan_rotate_left_, &QPushButton::clicked, this, [this]() { rotate_scan(-90); });
    connect(scan_rotate_right_, &QPushButton::clicked, this, [this]() { rotate_scan(90); });
    connect(scan_ocr_button_, &QPushButton::clicked, this, [this]() { run_ocr(); });

    connect(browse_button, &QPushButton::clicked, this, [this]() { browse_print_file(); });
    connect(print_submit_, &QPushButton::clicked, this, [this]() { submit_print(false); });
    connect(print_diagnose_, &QPushButton::clicked, this, [this]() { submit_print(true); });
    connect(jobs_refresh_, &QPushButton::clicked, this, [this]() { refresh_jobs(); });
    connect(jobs_cancel_, &QPushButton::clicked, this, [this]() { cancel_selected_job(); });
    connect(print_printer_, &QComboBox::currentTextChanged, this,
        [this](const QString&) { refresh_jobs(); });
}

void DeviceCenterWindow::reload_devices() {
    device_refresh_->setEnabled(false);
    device_summary_->setText(QStringLiteral("Discovering devices…"));

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher]() {
            try {
                const auto snapshot = watcher->result();
                const QString old_scan = scan_scanner_->currentData().toString();
                const QString old_print = print_printer_->currentData().toString();

                device_printers_->clear();
                device_scanners_->clear();
                scan_scanner_->clear();
                print_printer_->clear();

                int scan_restore = -1;
                int print_restore = -1;

                for (const auto& printer : snapshot.printers) {
                    QString text = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        text += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    if (printer.is_default) {
                        text += QStringLiteral(" [default]");
                    }
                    auto* item = new QListWidgetItem(text, device_printers_);
                    item->setData(Qt::UserRole, QString::fromStdString(printer.name));

                    print_printer_->addItem(text, QString::fromStdString(printer.name));
                    if (print_printer_->itemData(print_printer_->count() - 1).toString() == old_print) {
                        print_restore = print_printer_->count() - 1;
                    }
                }

                for (const auto& scanner : snapshot.scanners) {
                    QString text = QString::fromStdString(
                        scanner.vendor + " " + scanner.model + " — " + scanner.backend);
                    auto* item = new QListWidgetItem(text, device_scanners_);
                    item->setData(Qt::UserRole, QString::fromStdString(scanner.name));

                    scan_scanner_->addItem(text, QString::fromStdString(scanner.name));
                    if (scan_scanner_->itemData(scan_scanner_->count() - 1).toString() == old_scan) {
                        scan_restore = scan_scanner_->count() - 1;
                    }
                }

                if (scan_restore >= 0) {
                    scan_scanner_->setCurrentIndex(scan_restore);
                }
                if (print_restore >= 0) {
                    print_printer_->setCurrentIndex(print_restore);
                }
                if (device_printers_->count() > 0) {
                    device_printers_->setCurrentRow(0);
                }

                device_summary_->setText(
                    QStringLiteral("%1 printer(s), %2 scanner(s)")
                        .arg(snapshot.printers.size())
                        .arg(snapshot.scanners.size()));
                scan_status_->setText(
                    scan_scanner_->count() > 0
                        ? QStringLiteral("Scanner ready")
                        : QStringLiteral("No scanner discovered"));
                scan_preview_button_->setEnabled(scan_scanner_->count() > 0);
                scan_acquire_button_->setEnabled(scan_scanner_->count() > 0);
                print_submit_->setEnabled(print_printer_->count() > 0);
                print_diagnose_->setEnabled(print_printer_->count() > 0);
                refresh_jobs();
            } catch (const std::exception& error) {
                device_summary_->setText(
                    QStringLiteral("Discovery error: %1").arg(QString::fromUtf8(error.what())));
            }
            device_refresh_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void DeviceCenterWindow::load_printer_details(const QString& printer_name) {
    if (printer_name.isEmpty()) {
        return;
    }
    device_details_->setPlainText(QStringLiteral("Loading physical printer data…"));
    auto* watcher = new QFutureWatcher<PrinterDetails>(this);
    connect(watcher, &QFutureWatcher<PrinterDetails>::finished, this,
        [this, watcher, printer_name]() {
            try {
                const auto result = watcher->result();
                const auto* selected = device_printers_->currentItem();
                if (selected != nullptr &&
                    selected->data(Qt::UserRole).toString() == printer_name) {
                    device_details_->setPlainText(printer_details_text(result));
                }
            } catch (const std::exception& error) {
                device_details_->setPlainText(
                    QStringLiteral("Printer query error: %1").arg(QString::fromUtf8(error.what())));
            }
            watcher->deleteLater();
        });

    const std::string name = printer_name.toStdString();
    watcher->setFuture(QtConcurrent::run([manager = manager_, name]() {
        return PrinterDetails{
            .capabilities = manager->print_backend().capabilities(name),
            .status = manager->print_backend().status(name),
        };
    }));
}

void DeviceCenterWindow::start_scan(const bool preview) {
    const QString scanner_name = scan_scanner_->currentData().toString();
    if (scanner_name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("DocSuite Scan"), QStringLiteral("No scanner selected."));
        return;
    }

    ScanSettings settings;
    settings.dpi = preview ? 150 : scan_dpi_->currentData().toInt();
    settings.mode = scan_mode_->currentData().toString().toStdString();
    settings.source = "Flatbed";
    scan_dpi_value_ = settings.dpi;

    scan_preview_button_->setEnabled(false);
    scan_acquire_button_->setEnabled(false);
    scan_status_->setText(
        QStringLiteral("Scanning %1 dpi %2…")
            .arg(settings.dpi)
            .arg(scan_mode_->currentText()));

    using ScanPtr = std::shared_ptr<ScanFrame>;
    auto* watcher = new QFutureWatcher<ScanPtr>(this);
    connect(watcher, &QFutureWatcher<ScanPtr>::finished, this,
        [this, watcher]() {
            try {
                const auto frame = watcher->result();
                show_scan_frame(frame);
                scan_status_->setText(
                    QStringLiteral("Scan complete: %1 × %2 px, %3 dpi, %4 MiB")
                        .arg(frame->width)
                        .arg(frame->height)
                        .arg(frame->dpi)
                        .arg(static_cast<double>(frame->pixels.size()) / (1024.0 * 1024.0), 0, 'f', 1));
            } catch (const std::exception& error) {
                scan_status_->setText(
                    QStringLiteral("Scan error: %1").arg(QString::fromUtf8(error.what())));
            }
            scan_preview_button_->setEnabled(scan_scanner_->count() > 0);
            scan_acquire_button_->setEnabled(scan_scanner_->count() > 0);
            watcher->deleteLater();
        });

    const std::string name = scanner_name.toStdString();
    watcher->setFuture(QtConcurrent::run([manager = manager_, name, settings]() {
        return std::make_shared<ScanFrame>(manager->scan_backend().scan(name, settings));
    }));
}

void DeviceCenterWindow::show_scan_frame(const std::shared_ptr<ScanFrame>& frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->pixels.empty()) {
        throw std::runtime_error("Empty scan frame");
    }

    const int channels = frame->format == ScanPixelFormat::rgb24 ? 3 : 1;
    const auto image_format = frame->format == ScanPixelFormat::rgb24
        ? QImage::Format_RGB888
        : QImage::Format_Grayscale8;

    scan_frame_backing_ = frame;
    scan_image_data_ = QImage(
        static_cast<const uchar*>(frame->pixels.data()),
        frame->width,
        frame->height,
        frame->width * channels,
        image_format);
    scan_dpi_value_ = frame->dpi;

    scan_save_png_->setEnabled(true);
    scan_save_jpeg_->setEnabled(true);
    scan_save_pdf_->setEnabled(true);
    scan_rotate_left_->setEnabled(true);
    scan_rotate_right_->setEnabled(true);
    scan_ocr_button_->setEnabled(ocr_.available());
    scan_ocr_text_->clear();
    refresh_scan_preview();
}

void DeviceCenterWindow::refresh_scan_preview() {
    if (scan_image_data_.isNull() || scan_scroll_ == nullptr || scan_image_ == nullptr) {
        return;
    }
    QSize available = scan_scroll_->viewport()->size() - QSize(24, 24);
    if (available.width() < 100 || available.height() < 100) {
        available = QSize(800, 600);
    }
    const auto pixmap = QPixmap::fromImage(scan_image_data_).scaled(
        available,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
    scan_image_->setPixmap(pixmap);
}

void DeviceCenterWindow::save_scan_image(const QByteArray& format) {
    if (scan_image_data_.isNull()) {
        return;
    }
    const bool jpeg = format == QByteArrayLiteral("JPEG");
    const QString extension = jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    const QString filter = jpeg
        ? QStringLiteral("JPEG image (*.jpg *.jpeg)")
        : QStringLiteral("PNG image (*.png)");
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save scan"),
        QStringLiteral("scan.%1").arg(extension),
        filter);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".") + extension;
    }
    const int quality = jpeg ? 92 : -1;
    if (!scan_image_data_.save(path, format.constData(), quality)) {
        QMessageBox::critical(this, QStringLiteral("DocSuite Scan"),
            QStringLiteral("Unable to save %1").arg(path));
        return;
    }
    scan_status_->setText(QStringLiteral("Saved %1").arg(path));
}

void DeviceCenterWindow::save_scan_pdf() {
    if (scan_image_data_.isNull()) {
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

    QPdfWriter writer(path);
    writer.setCreator(QStringLiteral("DocSuite Device Center"));
    writer.setTitle(QStringLiteral("Scanned document"));
    writer.setResolution(std::max(scan_dpi_value_, 150));
    writer.setPageSize(QPageSize(QPageSize::A4));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        QMessageBox::critical(this, QStringLiteral("DocSuite Scan"),
            QStringLiteral("Unable to create PDF."));
        return;
    }
    QRect target = painter.viewport();
    QSize scaled = scan_image_data_.size();
    scaled.scale(target.size(), Qt::KeepAspectRatio);
    target.setSize(scaled);
    target.moveCenter(painter.viewport().center());
    painter.drawImage(target, scan_image_data_);
    painter.end();
    scan_status_->setText(QStringLiteral("Saved %1").arg(path));
}

void DeviceCenterWindow::rotate_scan(const int degrees) {
    if (scan_image_data_.isNull()) {
        return;
    }
    QTransform transform;
    transform.rotate(degrees);
    scan_image_data_ = scan_image_data_.transformed(transform, Qt::SmoothTransformation);
    scan_frame_backing_.reset();
    refresh_scan_preview();
}

void DeviceCenterWindow::run_ocr() {
    if (scan_image_data_.isNull() || !ocr_.available()) {
        return;
    }
    scan_ocr_button_->setEnabled(false);
    scan_ocr_text_->setPlainText(QStringLiteral("OCR running…"));

    const ScanFrame frame = scan_frame_backing_
        ? *scan_frame_backing_
        : frame_from_qimage(scan_image_data_, scan_dpi_value_);

    auto* watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this,
        [this, watcher]() {
            try {
                const auto result = watcher->result();
                scan_ocr_text_->setPlainText(QString::fromStdString(result.text));
                scan_status_->setText(
                    QStringLiteral("OCR complete — confidence %1% — %2")
                        .arg(result.mean_confidence)
                        .arg(QString::fromStdString(result.language)));
            } catch (const std::exception& error) {
                scan_ocr_text_->setPlainText(
                    QStringLiteral("OCR error: %1").arg(QString::fromUtf8(error.what())));
            }
            scan_ocr_button_->setEnabled(ocr_.available() && !scan_image_data_.isNull());
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([ocr = ocr_, frame]() {
        return ocr.recognize(frame, "fra+eng");
    }));
}

void DeviceCenterWindow::browse_print_file() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select document to print"),
        QString(),
        QStringLiteral("Documents (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.txt);;All files (*)"));
    if (!path.isEmpty()) {
        print_file_->setText(path);
    }
}

PrintProfile DeviceCenterWindow::selected_print_profile() const {
    PrintProfile profile;
    const QString key = print_profile_->currentData().toString();
    if (key == QStringLiteral("mono")) {
        profile.name = "Document monochrome";
        profile.color_mode = "monochrome";
    } else if (key == QStringLiteral("economy")) {
        profile.name = "Economy mono duplex";
        profile.color_mode = "monochrome";
        profile.sides = "two-sided-long-edge";
        profile.quality = 3;
    } else if (key == QStringLiteral("high")) {
        profile.name = "High quality color";
        profile.color_mode = "color";
        profile.quality = 5;
    } else {
        profile.name = "Standard color";
        profile.color_mode = "color";
        profile.quality = 4;
    }
    return profile;
}

void DeviceCenterWindow::submit_print(const bool diagnostic) {
    const QString printer = print_printer_->currentData().toString();
    const QString path = print_file_->text().trimmed();
    if (printer.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("DocSuite Print"), QStringLiteral("No printer selected."));
        return;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("DocSuite Print"),
            QStringLiteral("Choose an existing file to print."));
        return;
    }

    print_submit_->setEnabled(false);
    print_diagnose_->setEnabled(false);
    print_log_->setPlainText(
        diagnostic ? QStringLiteral("Submitting and tracing print job…")
                   : QStringLiteral("Submitting print job…"));

    const std::string printer_name = printer.toStdString();
    const std::string file_path = path.toStdString();
    const auto profile = selected_print_profile();

    if (diagnostic) {
        auto* watcher = new QFutureWatcher<PrintJobTrace>(this);
        connect(watcher, &QFutureWatcher<PrintJobTrace>::finished, this,
            [this, watcher]() {
                try {
                    const auto trace = watcher->result();
                    print_log_->setPlainText(trace_text(trace));
                } catch (const std::exception& error) {
                    print_log_->setPlainText(
                        QStringLiteral("Print diagnostic error: %1").arg(QString::fromUtf8(error.what())));
                }
                print_submit_->setEnabled(print_printer_->count() > 0);
                print_diagnose_->setEnabled(print_printer_->count() > 0);
                refresh_jobs();
                watcher->deleteLater();
            });
        watcher->setFuture(QtConcurrent::run(
            [manager = manager_, printer_name, file_path, profile]() {
                return manager->job_manager().diagnose_print(
                    printer_name,
                    file_path,
                    "DocSuite GUI diagnostic print",
                    profile);
            }));
    } else {
        auto* watcher = new QFutureWatcher<int>(this);
        connect(watcher, &QFutureWatcher<int>::finished, this,
            [this, watcher]() {
                try {
                    const int id = watcher->result();
                    print_log_->setPlainText(QStringLiteral("Submitted CUPS job #%1").arg(id));
                } catch (const std::exception& error) {
                    print_log_->setPlainText(
                        QStringLiteral("Print error: %1").arg(QString::fromUtf8(error.what())));
                }
                print_submit_->setEnabled(print_printer_->count() > 0);
                print_diagnose_->setEnabled(print_printer_->count() > 0);
                refresh_jobs();
                watcher->deleteLater();
            });
        watcher->setFuture(QtConcurrent::run(
            [manager = manager_, printer_name, file_path, profile]() {
                return manager->print_backend().print_file(
                    printer_name,
                    file_path,
                    "DocSuite GUI print",
                    profile);
            }));
    }
}

void DeviceCenterWindow::refresh_jobs() {
    const QString printer = print_printer_->currentData().toString();
    if (printer.isEmpty()) {
        jobs_table_->setRowCount(0);
        return;
    }
    jobs_refresh_->setEnabled(false);
    const std::string printer_name = printer.toStdString();

    auto* watcher = new QFutureWatcher<std::vector<PrintJobInfo>>(this);
    connect(watcher, &QFutureWatcher<std::vector<PrintJobInfo>>::finished, this,
        [this, watcher, printer]() {
            try {
                const auto jobs = watcher->result();
                if (print_printer_->currentData().toString() != printer) {
                    watcher->deleteLater();
                    jobs_refresh_->setEnabled(true);
                    return;
                }
                jobs_table_->setRowCount(static_cast<int>(jobs.size()));
                for (int row = 0; row < static_cast<int>(jobs.size()); ++row) {
                    const auto& job = jobs[static_cast<std::size_t>(row)];
                    auto* id_item = new QTableWidgetItem(QString::number(job.id));
                    id_item->setData(Qt::UserRole, job.id);
                    jobs_table_->setItem(row, 0, id_item);
                    jobs_table_->setItem(row, 1,
                        new QTableWidgetItem(QString::fromLatin1(print_job_state_name(job.state))));
                    jobs_table_->setItem(row, 2,
                        new QTableWidgetItem(QString::fromStdString(job.title)));
                    jobs_table_->setItem(row, 3,
                        new QTableWidgetItem(QStringLiteral("%1 KiB").arg(job.size_kib)));
                    jobs_table_->setItem(row, 4,
                        new QTableWidgetItem(QString::fromStdString(job.format)));
                }
                jobs_table_->resizeColumnToContents(0);
                jobs_table_->resizeColumnToContents(1);
                jobs_table_->resizeColumnToContents(3);
            } catch (const std::exception& error) {
                print_log_->appendPlainText(
                    QStringLiteral("Job refresh error: %1").arg(QString::fromUtf8(error.what())));
            }
            jobs_refresh_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_, printer_name]() {
        return manager->job_manager().list_jobs(printer_name, true);
    }));
}

void DeviceCenterWindow::cancel_selected_job() {
    const auto ranges = jobs_table_->selectedRanges();
    if (ranges.isEmpty()) {
        return;
    }
    const int row = ranges.first().topRow();
    const auto* item = jobs_table_->item(row, 0);
    if (item == nullptr) {
        return;
    }
    const int job_id = item->data(Qt::UserRole).toInt();
    const std::string printer = print_printer_->currentData().toString().toStdString();
    try {
        const bool ok = manager_->job_manager().cancel(printer, job_id);
        print_log_->appendPlainText(
            ok ? QStringLiteral("Canceled job #%1").arg(job_id)
               : QStringLiteral("Unable to cancel job #%1").arg(job_id));
        refresh_jobs();
    } catch (const std::exception& error) {
        print_log_->appendPlainText(
            QStringLiteral("Cancel error: %1").arg(QString::fromUtf8(error.what())));
    }
}

void DeviceCenterWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    refresh_scan_preview();
}

} // namespace docsuite::desktop
