// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "print_page.hpp"

#include "docsuite/print/job_manager.hpp"
#include "docsuite/print/print_validation.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace docsuite::desktop {
namespace {

[[nodiscard]] QString optional_duration(
    const std::optional<std::chrono::milliseconds>& value) {
    return value.has_value()
        ? QStringLiteral("%1 ms").arg(value->count())
        : QStringLiteral("not reported");
}

[[nodiscard]] QString format_trace(const PrintJobTrace& trace) {
    QString text;
    text += QStringLiteral("Job #%1\n").arg(trace.job_id);
    text += QStringLiteral("Printer: %1\n").arg(QString::fromStdString(trace.printer));
    text += QStringLiteral("Final state: %1\n")
        .arg(QString::fromLatin1(print_job_state_name(trace.final_state)));
    text += QStringLiteral("Submit -> CUPS accepted: %1 ms\n\n")
        .arg(trace.submit_to_accept.count());
    text += QStringLiteral("Timeline\n");
    for (const auto& event : trace.events) {
        text += QStringLiteral("  +%1 ms | %2 | %3\n")
            .arg(event.since_submit.count())
            .arg(QString::fromStdString(event.name))
            .arg(QString::fromLatin1(print_job_state_name(event.state)));
    }
    text += QStringLiteral("\nCUPS queue delay: %1\n").arg(optional_duration(trace.queue_delay));
    text += QStringLiteral("CUPS processing duration: %1\n")
        .arg(optional_duration(trace.processing_duration));
    text += QStringLiteral("CUPS total duration: %1\n")
        .arg(optional_duration(trace.total_duration));
    text += QStringLiteral("Timed out: %1\n")
        .arg(trace.timed_out ? QStringLiteral("yes") : QStringLiteral("no"));
    if (!trace.history_path.empty()) {
        text += QStringLiteral("History: %1\n").arg(QString::fromStdString(trace.history_path));
    }
    return text;
}

[[nodiscard]] QString pretty_keyword(const std::string& value) {
    QString text = QString::fromStdString(value);
    text.replace(QLatin1Char('-'), QLatin1Char(' '));
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    return text;
}

void populate_strings(
    QComboBox* combo,
    const std::vector<std::string>& values,
    const QString& preferred,
    const bool allow_automatic) {

    const QSignalBlocker blocker{combo};
    combo->clear();
    if (allow_automatic) {
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
    combo->setEnabled(!values.empty());
}

void select_if_available(QComboBox* combo, const QVariant& value) {
    const int index = combo->findData(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

[[nodiscard]] QString quality_label(const int quality) {
    switch (quality) {
        case 3: return QStringLiteral("Draft");
        case 4: return QStringLiteral("Normal");
        case 5: return QStringLiteral("High");
        default: return QStringLiteral("Quality %1").arg(quality);
    }
}

[[nodiscard]] QString preflight_summary(const PrintPreflightResult& result) {
    QStringList lines;
    lines << (result.ok
        ? QStringLiteral("Ready — selected options are supported by the cached printer capabilities.")
        : QStringLiteral("Blocked — selected options are not supported."));
    for (const auto& error : result.errors) {
        lines << QStringLiteral("Error: %1").arg(QString::fromStdString(error));
    }
    for (const auto& warning : result.warnings) {
        lines << QStringLiteral("Warning: %1").arg(QString::fromStdString(warning));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

PrintPage::PrintPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* layout = new QVBoxLayout(this);

    auto* print_group = new QGroupBox(QStringLiteral("Print"), this);
    auto* form = new QFormLayout(print_group);
    printer_ = new QComboBox(print_group);
    file_ = new QLineEdit(print_group);

    preset_ = new QComboBox(print_group);
    preset_->addItem(QStringLiteral("Standard color"), QStringLiteral("color"));
    preset_->addItem(QStringLiteral("Document monochrome"), QStringLiteral("mono"));
    preset_->addItem(QStringLiteral("Economy mono duplex"), QStringLiteral("economy"));
    preset_->addItem(QStringLiteral("High quality color"), QStringLiteral("high"));
    preset_->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));

    color_ = new QComboBox(print_group);
    sides_ = new QComboBox(print_group);
    quality_ = new QComboBox(print_group);
    media_ = new QComboBox(print_group);
    source_ = new QComboBox(print_group);
    media_type_ = new QComboBox(print_group);
    copies_ = new QSpinBox(print_group);
    copies_->setRange(1, 1);

    auto* file_row = new QWidget(print_group);
    auto* file_row_layout = new QHBoxLayout(file_row);
    file_row_layout->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), file_row);
    file_row_layout->addWidget(file_, 1);
    file_row_layout->addWidget(browse);

    auto* printer_row = new QWidget(print_group);
    auto* printer_row_layout = new QHBoxLayout(printer_row);
    printer_row_layout->setContentsMargins(0, 0, 0, 0);
    refresh_printers_ = new QPushButton(QStringLiteral("Refresh printers"), printer_row);
    refresh_capabilities_ = new QPushButton(QStringLiteral("Refresh capabilities"), printer_row);
    printer_row_layout->addWidget(printer_, 1);
    printer_row_layout->addWidget(refresh_printers_);
    printer_row_layout->addWidget(refresh_capabilities_);

    capabilities_status_ = new QLabel(QStringLiteral("Capabilities not loaded yet"), print_group);
    capabilities_status_->setWordWrap(true);
    preflight_status_ = new QLabel(
        QStringLiteral("Preflight waiting for printer capabilities"),
        print_group);
    preflight_status_->setWordWrap(true);
    preflight_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    form->addRow(QStringLiteral("Printer"), printer_row);
    form->addRow(QStringLiteral("File"), file_row);
    form->addRow(QStringLiteral("Preset"), preset_);
    form->addRow(QStringLiteral("Color"), color_);
    form->addRow(QStringLiteral("Duplex"), sides_);
    form->addRow(QStringLiteral("Quality"), quality_);
    form->addRow(QStringLiteral("Paper"), media_);
    form->addRow(QStringLiteral("Paper source"), source_);
    form->addRow(QStringLiteral("Media type"), media_type_);
    form->addRow(QStringLiteral("Copies"), copies_);
    form->addRow(QStringLiteral("IPP capabilities"), capabilities_status_);
    form->addRow(QStringLiteral("Preflight"), preflight_status_);

    auto* submit_row = new QHBoxLayout();
    print_ = new QPushButton(QStringLiteral("Print"), print_group);
    diagnose_ = new QPushButton(QStringLiteral("Print + diagnose timing"), print_group);
    submit_row->addWidget(print_);
    submit_row->addWidget(diagnose_);
    submit_row->addStretch();
    form->addRow(submit_row);

    auto* jobs_group = new QGroupBox(QStringLiteral("CUPS jobs"), this);
    auto* jobs_layout = new QVBoxLayout(jobs_group);
    auto* jobs_toolbar = new QHBoxLayout();
    refresh_jobs_ = new QPushButton(QStringLiteral("Refresh jobs"), jobs_group);
    cancel_ = new QPushButton(QStringLiteral("Cancel selected"), jobs_group);
    jobs_toolbar->addWidget(refresh_jobs_);
    jobs_toolbar->addWidget(cancel_);
    jobs_toolbar->addStretch();

    jobs_ = new QTableWidget(0, 5, jobs_group);
    jobs_->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("State"), QStringLiteral("Title"),
         QStringLiteral("Size"), QStringLiteral("Format")});
    jobs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobs_->setSelectionMode(QAbstractItemView::SingleSelection);
    jobs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobs_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    jobs_->horizontalHeader()->setStretchLastSection(true);

    jobs_layout->addLayout(jobs_toolbar);
    jobs_layout->addWidget(jobs_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setPlaceholderText(QStringLiteral("Print submissions and timing diagnostics appear here."));
    log_->setMaximumBlockCount(1500);

    layout->addWidget(print_group);
    layout->addWidget(jobs_group, 1);
    layout->addWidget(log_);

    connect(browse, &QPushButton::clicked, this, [this]() { browse_file(); });
    connect(refresh_printers_, &QPushButton::clicked, this, [this]() { refresh_printers(); });
    connect(refresh_capabilities_, &QPushButton::clicked, this,
        [this]() { refresh_capabilities(true); });
    connect(print_, &QPushButton::clicked, this, [this]() { submit(false); });
    connect(diagnose_, &QPushButton::clicked, this, [this]() { submit(true); });
    connect(refresh_jobs_, &QPushButton::clicked, this, [this]() { refresh_jobs(); });
    connect(cancel_, &QPushButton::clicked, this, [this]() { cancel_selected(); });
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int) { apply_preset(); });
    connect(printer_, &QComboBox::currentIndexChanged, this, [this](int) {
        last_capabilities_.reset();
        update_preflight();
        refresh_jobs();
        refresh_capabilities(false);
    });

    for (QComboBox* combo : {color_, sides_, quality_, media_, source_, media_type_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this](int) {
            update_preflight();
        });
    }
    connect(copies_, &QSpinBox::valueChanged, this, [this](int) { update_preflight(); });

    refresh_printers();
}

void PrintPage::refresh_printers() {
    refresh_printers_->setEnabled(false);
    const QString previous = printer_->currentData().toString();

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher, previous]() {
            try {
                const auto snapshot = watcher->result();
                const QSignalBlocker blocker{printer_};
                printer_->clear();
                int restore = -1;
                for (const auto& printer : snapshot.printers) {
                    QString label = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        label += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    if (printer.is_default) {
                        label += QStringLiteral(" [default]");
                    }
                    printer_->addItem(label, QString::fromStdString(printer.name));
                    if (printer_->itemData(printer_->count() - 1).toString() == previous) {
                        restore = printer_->count() - 1;
                    }
                }
                if (restore >= 0) {
                    printer_->setCurrentIndex(restore);
                }
                const bool available = printer_->count() > 0;
                print_->setEnabled(available);
                diagnose_->setEnabled(available);
                refresh_capabilities_->setEnabled(available);
                if (!available) {
                    last_capabilities_.reset();
                    capabilities_status_->setText(QStringLiteral("No printer discovered"));
                    update_preflight();
                }
            } catch (const std::exception& error) {
                log_->appendPlainText(
                    QStringLiteral("Printer discovery error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_printers_->setEnabled(true);
            watcher->deleteLater();
            refresh_jobs();
            refresh_capabilities(false);
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void PrintPage::refresh_capabilities(const bool force_refresh) {
    const QString printer = printer_->currentData().toString();
    if (printer.isEmpty()) {
        return;
    }

    refresh_capabilities_->setEnabled(false);
    capabilities_status_->setText(QStringLiteral("Loading physical IPP capabilities…"));
    const std::string name = printer.toStdString();

    auto* watcher = new QFutureWatcher<PrinterCapabilities>(this);
    connect(watcher, &QFutureWatcher<PrinterCapabilities>::finished, this,
        [this, watcher, printer]() {
            try {
                const auto caps = watcher->result();
                if (printer_->currentData().toString() != printer) {
                    refresh_capabilities_->setEnabled(true);
                    watcher->deleteLater();
                    return;
                }

                populate_strings(color_, caps.color_modes, QStringLiteral("color"), false);
                populate_strings(sides_, caps.sides, QStringLiteral("one-sided"), false);
                populate_strings(media_, caps.media, QStringLiteral("iso_a4_210x297mm"), false);
                populate_strings(source_, caps.media_sources, QString{}, true);
                populate_strings(media_type_, caps.media_types, QString{}, true);

                {
                    const QSignalBlocker blocker{quality_};
                    quality_->clear();
                    for (const int quality : caps.qualities) {
                        quality_->addItem(quality_label(quality), quality);
                    }
                    int normal = quality_->findData(4);
                    if (normal < 0 && quality_->count() > 0) {
                        normal = 0;
                    }
                    if (normal >= 0) {
                        quality_->setCurrentIndex(normal);
                    }
                    quality_->setEnabled(!caps.qualities.empty());
                }

                copies_->setRange(
                    std::max(1, caps.copies_min),
                    std::max(std::max(1, caps.copies_min), caps.copies_max));
                copies_->setValue(1);

                last_capabilities_ = caps;
                capabilities_status_->setText(
                    QStringLiteral("%1 — %2 color mode(s), %3 paper size(s), %4 source(s), copies %5–%6")
                        .arg(QString::fromStdString(caps.source))
                        .arg(caps.color_modes.size())
                        .arg(caps.media.size())
                        .arg(caps.media_sources.size())
                        .arg(caps.copies_min)
                        .arg(caps.copies_max));
                apply_preset();
                update_preflight();
            } catch (const std::exception& error) {
                last_capabilities_.reset();
                capabilities_status_->setText(
                    QStringLiteral("Capability error: %1").arg(QString::fromUtf8(error.what())));
                log_->appendPlainText(capabilities_status_->text());
                update_preflight();
            }
            refresh_capabilities_->setEnabled(printer_->count() > 0);
            watcher->deleteLater();
        });

    watcher->setFuture(QtConcurrent::run([manager = manager_, name, force_refresh]() {
        return manager->print_backend().capabilities(name, force_refresh);
    }));
}

void PrintPage::browse_file() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select a document to print"),
        QString(),
        QStringLiteral("Documents (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.txt);;All files (*)"));
    if (!path.isEmpty()) {
        file_->setText(path);
    }
}

void PrintPage::apply_preset() {
    const QString key = preset_->currentData().toString();
    if (key != QStringLiteral("custom")) {
        if (key == QStringLiteral("mono")) {
            select_if_available(color_, QStringLiteral("monochrome"));
            select_if_available(sides_, QStringLiteral("one-sided"));
            select_if_available(quality_, 4);
        } else if (key == QStringLiteral("economy")) {
            select_if_available(color_, QStringLiteral("monochrome"));
            select_if_available(sides_, QStringLiteral("two-sided-long-edge"));
            select_if_available(quality_, 3);
        } else if (key == QStringLiteral("high")) {
            select_if_available(color_, QStringLiteral("color"));
            select_if_available(sides_, QStringLiteral("one-sided"));
            select_if_available(quality_, 5);
        } else {
            select_if_available(color_, QStringLiteral("color"));
            select_if_available(sides_, QStringLiteral("one-sided"));
            select_if_available(quality_, 4);
        }
    }
    update_preflight();
}

PrintProfile PrintPage::selected_profile() const {
    PrintProfile result;
    result.name = preset_->currentText().toStdString();
    if (media_->currentIndex() >= 0) {
        result.media = media_->currentData().toString().toStdString();
    }
    result.media_source = source_->currentData().toString().toStdString();
    result.media_type = media_type_->currentData().toString().toStdString();
    if (color_->currentIndex() >= 0) {
        result.color_mode = color_->currentData().toString().toStdString();
    }
    if (sides_->currentIndex() >= 0) {
        result.sides = sides_->currentData().toString().toStdString();
    }
    if (quality_->currentIndex() >= 0) {
        result.quality = quality_->currentData().toInt();
    }
    result.copies = copies_->value();
    return result;
}

void PrintPage::update_preflight() {
    const bool printer_available = !printer_->currentData().toString().isEmpty();
    if (!last_capabilities_.has_value() ||
        last_capabilities_->printer != printer_->currentData().toString().toStdString()) {
        preflight_status_->setText(
            printer_available
                ? QStringLiteral("Waiting for cached printer capabilities; submission will validate again in PrintCore.")
                : QStringLiteral("No printer selected."));
        print_->setEnabled(printer_available);
        diagnose_->setEnabled(printer_available);
        return;
    }

    const auto result = validate_print_profile(
        *last_capabilities_,
        selected_profile());
    preflight_status_->setText(preflight_summary(result));
    print_->setEnabled(printer_available && result.ok);
    diagnose_->setEnabled(printer_available && result.ok);
}

void PrintPage::submit(const bool diagnostic) {
    const QString printer = printer_->currentData().toString();
    const QString path = file_->text().trimmed();
    if (printer.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("DocSuite Print"), QStringLiteral("No printer selected."));
        return;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("DocSuite Print"),
            QStringLiteral("Choose an existing file first."));
        return;
    }

    if (last_capabilities_.has_value()) {
        const auto result = validate_print_profile(*last_capabilities_, selected_profile());
        if (!result.ok) {
            preflight_status_->setText(preflight_summary(result));
            QMessageBox::warning(
                this,
                QStringLiteral("DocSuite Print"),
                QStringLiteral("The selected print options failed preflight. Fix them before submitting."));
            return;
        }
    }

    print_->setEnabled(false);
    diagnose_->setEnabled(false);
    log_->setPlainText(
        diagnostic ? QStringLiteral("Submitting and tracing print job…")
                   : QStringLiteral("Submitting print job with dynamic IPP options…"));

    const std::string printer_name = printer.toStdString();
    const std::string file_path = path.toStdString();
    const PrintProfile profile = selected_profile();

    if (diagnostic) {
        auto* watcher = new QFutureWatcher<PrintJobTrace>(this);
        connect(watcher, &QFutureWatcher<PrintJobTrace>::finished, this,
            [this, watcher]() {
                try {
                    log_->setPlainText(format_trace(watcher->result()));
                } catch (const std::exception& error) {
                    log_->setPlainText(
                        QStringLiteral("Diagnostic error: %1").arg(QString::fromUtf8(error.what())));
                }
                update_preflight();
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
        return;
    }

    auto* watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this,
        [this, watcher]() {
            try {
                log_->setPlainText(QStringLiteral("Submitted CUPS job #%1").arg(watcher->result()));
            } catch (const std::exception& error) {
                log_->setPlainText(
                    QStringLiteral("Print error: %1").arg(QString::fromUtf8(error.what())));
            }
            update_preflight();
            refresh_jobs();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [manager = manager_, printer_name, file_path, profile]() {
            return manager->print_backend().print_file_advanced(
                printer_name,
                file_path,
                "DocSuite GUI print",
                profile);
        }));
}

void PrintPage::refresh_jobs() {
    const QString printer = printer_->currentData().toString();
    if (printer.isEmpty()) {
        jobs_->setRowCount(0);
        return;
    }
    refresh_jobs_->setEnabled(false);
    const std::string name = printer.toStdString();

    auto* watcher = new QFutureWatcher<std::vector<PrintJobInfo>>(this);
    connect(watcher, &QFutureWatcher<std::vector<PrintJobInfo>>::finished, this,
        [this, watcher, printer]() {
            try {
                const auto result = watcher->result();
                if (printer_->currentData().toString() != printer) {
                    refresh_jobs_->setEnabled(true);
                    watcher->deleteLater();
                    return;
                }

                jobs_->setRowCount(static_cast<int>(result.size()));
                for (int row = 0; row < static_cast<int>(result.size()); ++row) {
                    const auto& job = result[static_cast<std::size_t>(row)];
                    auto* id = new QTableWidgetItem(QString::number(job.id));
                    id->setData(Qt::UserRole, job.id);
                    jobs_->setItem(row, 0, id);
                    jobs_->setItem(row, 1,
                        new QTableWidgetItem(QString::fromLatin1(print_job_state_name(job.state))));
                    jobs_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(job.title)));
                    jobs_->setItem(row, 3,
                        new QTableWidgetItem(QStringLiteral("%1 KiB").arg(job.size_kib)));
                    jobs_->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(job.format)));
                }
                jobs_->resizeColumnToContents(0);
                jobs_->resizeColumnToContents(1);
                jobs_->resizeColumnToContents(3);
            } catch (const std::exception& error) {
                log_->appendPlainText(
                    QStringLiteral("Job refresh error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_jobs_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_, name]() {
        return manager->job_manager().list_jobs(name, true);
    }));
}

void PrintPage::cancel_selected() {
    const auto selection = jobs_->selectedRanges();
    if (selection.isEmpty()) {
        return;
    }
    const int row = selection.first().topRow();
    const auto* item = jobs_->item(row, 0);
    if (item == nullptr) {
        return;
    }
    const int job_id = item->data(Qt::UserRole).toInt();
    const std::string printer = printer_->currentData().toString().toStdString();
    try {
        const bool ok = manager_->job_manager().cancel(printer, job_id);
        log_->appendPlainText(
            ok ? QStringLiteral("Canceled job #%1").arg(job_id)
               : QStringLiteral("Unable to cancel job #%1").arg(job_id));
        refresh_jobs();
    } catch (const std::exception& error) {
        log_->appendPlainText(
            QStringLiteral("Cancel error: %1").arg(QString::fromUtf8(error.what())));
    }
}

} // namespace docsuite::desktop
