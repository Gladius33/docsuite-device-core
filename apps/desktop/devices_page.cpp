// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "devices_page.hpp"

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

DevicesPage::DevicesPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

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

    connect(refresh_button_, &QPushButton::clicked, this, [this]() { refresh(); });
    connect(printers_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current, QListWidgetItem*) {
            if (current == nullptr) {
                details_->clear();
                return;
            }
            load_printer_details(current->data(Qt::UserRole).toString());
        });

    refresh();
}

void DevicesPage::refresh() {
    refresh_button_->setEnabled(false);
    summary_->setText(QStringLiteral("Discovering devices…"));

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher]() {
            try {
                const auto snapshot = watcher->result();
                printers_->clear();
                scanners_->clear();

                for (const auto& printer : snapshot.printers) {
                    QString label = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        label += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    if (printer.is_default) {
                        label += QStringLiteral(" [default]");
                    }
                    auto* item = new QListWidgetItem(label, printers_);
                    item->setData(Qt::UserRole, QString::fromStdString(printer.name));
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
                if (printers_->count() > 0) {
                    printers_->setCurrentRow(0);
                }
            } catch (const std::exception& error) {
                summary_->setText(
                    QStringLiteral("Discovery error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_button_->setEnabled(true);
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void DevicesPage::load_printer_details(const QString& printer_name) {
    if (printer_name.isEmpty()) {
        return;
    }
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
                details_->setPlainText(
                    QStringLiteral("Printer query error: %1")
                        .arg(QString::fromUtf8(error.what())));
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

} // namespace docsuite::desktop
