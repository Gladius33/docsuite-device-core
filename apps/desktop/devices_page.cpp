// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "devices_page.hpp"

#include "device_service_gateway.hpp"

#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>
#include <QtConcurrent>

#include <exception>
#include <string>
#include <vector>

namespace docsuite::desktop {
namespace {

struct PrinterDetails {
    PrinterCapabilities capabilities;
    PrinterStatus status;
};

[[nodiscard]] QString state_name(const DeviceState state) {
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
    QStringList result;
    for (const auto& value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result.isEmpty() ? QStringLiteral("Not reported") : result.join(QStringLiteral(", "));
}

[[nodiscard]] QString join_ints(const std::vector<int>& values) {
    QStringList result;
    for (const int value : values) {
        result.push_back(QString::number(value));
    }
    return result.isEmpty() ? QStringLiteral("Not reported") : result.join(QStringLiteral(", "));
}

[[nodiscard]] QString format_details(const PrinterDetails& details) {
    QString text;
    text += QStringLiteral("Status source: %1\n")
        .arg(QString::fromStdString(details.status.source));
    text += QStringLiteral("Capability source: %1\n")
        .arg(QString::fromStdString(details.capabilities.source));
    text += QStringLiteral("State: %1\n").arg(state_name(details.status.state));
    text += QStringLiteral("Accepting jobs: %1\n")
        .arg(details.status.accepting_jobs ? QStringLiteral("yes") : QStringLiteral("no"));
    text += QStringLiteral("Reasons: %1\n\n").arg(join_strings(details.status.reasons));

    text += QStringLiteral("Capabilities\n");
    text += QStringLiteral("  Color: %1\n").arg(join_strings(details.capabilities.color_modes));
    text += QStringLiteral("  Duplex: %1\n").arg(join_strings(details.capabilities.sides));
    text += QStringLiteral("  Quality: %1\n").arg(join_ints(details.capabilities.qualities));
    text += QStringLiteral("  Resolution DPI: %1\n")
        .arg(join_ints(details.capabilities.resolutions_dpi));
    text += QStringLiteral("  Copies: %1-%2\n")
        .arg(details.capabilities.copies_min)
        .arg(details.capabilities.copies_max);
    text += QStringLiteral("  Sources: %1\n")
        .arg(join_strings(details.capabilities.media_sources));
    text += QStringLiteral("  Media types: %1\n")
        .arg(join_strings(details.capabilities.media_types));
    text += QStringLiteral("  Native formats: %1\n")
        .arg(join_strings(details.capabilities.document_formats));
    text += QStringLiteral("  Media sizes: %1\n\n").arg(details.capabilities.media.size());

    text += QStringLiteral("Supplies\n");
    if (details.status.supplies.empty()) {
        text += QStringLiteral("  Not reported by device\n");
    }
    for (const auto& supply : details.status.supplies) {
        text += QStringLiteral("  %1: ").arg(QString::fromStdString(supply.name));
        if (supply.percent.has_value()) {
            text += QStringLiteral("%1%").arg(*supply.percent);
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

} // namespace

DevicesPage::DevicesPage(
    std::shared_ptr<DeviceServiceGateway> gateway,
    QWidget* parent)
    : QWidget{parent}, gateway_{std::move(gateway)} {

    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout();
    summary_ = new QLabel(QStringLiteral("Discovering devices…"), this);
    refresh_button_ = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar->addWidget(summary_, 1);
    toolbar->addWidget(refresh_button_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    printers_ = new QListWidget(left);
    scanners_ = new QListWidget(left);
    left_layout->addWidget(new QLabel(QStringLiteral("Printers"), left));
    left_layout->addWidget(printers_);
    left_layout->addWidget(new QLabel(QStringLiteral("Scanners"), left));
    left_layout->addWidget(scanners_);

    details_ = new QTextEdit(splitter);
    details_->setReadOnly(true);
    details_->setPlaceholderText(
        QStringLiteral("Select a printer to query physical IPP capabilities and status."));

    splitter->addWidget(left);
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    layout->addLayout(toolbar);
    layout->addWidget(splitter, 1);

    connect(refresh_button_, &QPushButton::clicked, this, [this]() { refresh(true); });
    connect(printers_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current, QListWidgetItem*) {
            if (current == nullptr) {
                details_printer_.clear();
                pending_details_printer_.clear();
                details_->clear();
                return;
            }
            const QString printer = current->data(Qt::UserRole).toString();
            if (printer != details_printer_) {
                load_printer_details(printer);
            }
        });

    refresh(true);
}

void DevicesPage::refresh(const bool refresh_selected_details) {
    if (refresh_in_progress_) {
        return;
    }
    refresh_in_progress_ = true;
    refresh_button_->setEnabled(false);
    summary_->setText(QStringLiteral("Discovering devices…"));

    const QString previous_printer = printers_->currentItem() != nullptr
        ? printers_->currentItem()->data(Qt::UserRole).toString()
        : QString{};

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher, previous_printer, refresh_selected_details]() {
            try {
                const auto snapshot = watcher->result();

                printers_->blockSignals(true);
                printers_->clear();
                scanners_->clear();

                int restore_row = -1;
                for (const auto& printer : snapshot.printers) {
                    QString label = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        label += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    if (printer.is_default) {
                        label += QStringLiteral(" [default]");
                    }
                    auto* item = new QListWidgetItem(label, printers_);
                    const QString printer_name = QString::fromStdString(printer.name);
                    item->setData(Qt::UserRole, printer_name);
                    if (printer_name == previous_printer) {
                        restore_row = printers_->count() - 1;
                    }
                }

                for (const auto& scanner : snapshot.scanners) {
                    const QString label = QStringLiteral("%1 %2 — %3")
                        .arg(QString::fromStdString(scanner.vendor))
                        .arg(QString::fromStdString(scanner.model))
                        .arg(QString::fromStdString(scanner.backend));
                    auto* item = new QListWidgetItem(label, scanners_);
                    item->setToolTip(QString::fromStdString(scanner.name));
                }

                summary_->setText(
                    QStringLiteral("%1 printer(s), %2 scanner(s)")
                        .arg(snapshot.printers.size())
                        .arg(snapshot.scanners.size()));

                if (restore_row < 0 && printers_->count() > 0) {
                    restore_row = 0;
                }
                if (restore_row >= 0) {
                    printers_->setCurrentRow(restore_row);
                }
                printers_->blockSignals(false);

                if (restore_row >= 0) {
                    const QString selected = printers_->item(restore_row)
                        ->data(Qt::UserRole).toString();
                    if (refresh_selected_details || selected != details_printer_) {
                        load_printer_details(selected);
                    }
                } else {
                    details_printer_.clear();
                    pending_details_printer_.clear();
                    details_->clear();
                }
            } catch (const std::exception& error) {
                summary_->setText(
                    QStringLiteral("Discovery error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_in_progress_ = false;
            refresh_button_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([gateway = gateway_]() { return gateway->snapshot(); }));
}

void DevicesPage::refresh_selected_details() {
    if (printers_->currentItem() == nullptr) {
        return;
    }
    load_printer_details(printers_->currentItem()->data(Qt::UserRole).toString());
}

void DevicesPage::load_printer_details(const QString& printer_name) {
    if (printer_name.isEmpty()) {
        return;
    }
    if (details_refresh_in_progress_) {
        pending_details_printer_ = printer_name;
        return;
    }

    details_refresh_in_progress_ = true;
    pending_details_printer_.clear();
    details_printer_ = printer_name;
    details_->setPlainText(QStringLiteral("Loading physical printer data…"));

    auto* watcher = new QFutureWatcher<PrinterDetails>(this);
    connect(watcher, &QFutureWatcher<PrinterDetails>::finished, this,
        [this, watcher, printer_name]() {
            try {
                const auto result = watcher->result();
                const auto* current = printers_->currentItem();
                if (current != nullptr &&
                    current->data(Qt::UserRole).toString() == printer_name) {
                    details_->setPlainText(format_details(result));
                }
            } catch (const std::exception& error) {
                if (details_printer_ == printer_name) {
                    details_->setPlainText(
                        QStringLiteral("Printer query error: %1")
                            .arg(QString::fromUtf8(error.what())));
                }
            }

            details_refresh_in_progress_ = false;
            const QString pending = pending_details_printer_;
            pending_details_printer_.clear();
            watcher->deleteLater();

            if (!pending.isEmpty()) {
                load_printer_details(pending);
            }
        });

    const std::string name = printer_name.toStdString();
    watcher->setFuture(QtConcurrent::run([gateway = gateway_, name]() {
        return PrinterDetails{
            .capabilities = gateway->printer_capabilities(name),
            .status = gateway->printer_status(name),
        };
    }));
}

} // namespace docsuite::desktop
