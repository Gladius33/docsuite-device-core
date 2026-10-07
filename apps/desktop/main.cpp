// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <QApplication>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <QtConcurrent>

#include <exception>
#include <memory>
#include <string>

namespace {

struct PrinterDetails {
    docsuite::PrinterCapabilities capabilities;
    docsuite::PrinterStatus status;
};

QString state_name(const docsuite::DeviceState state) {
    switch (state) {
        case docsuite::DeviceState::idle: return "Idle";
        case docsuite::DeviceState::processing: return "Printing";
        case docsuite::DeviceState::stopped: return "Stopped";
        case docsuite::DeviceState::offline: return "Offline";
        case docsuite::DeviceState::unknown: return "Unknown";
    }
    return "Unknown";
}

QString join_strings(const std::vector<std::string>& values) {
    QStringList list;
    for (const auto& value : values) {
        list.push_back(QString::fromStdString(value));
    }
    return list.isEmpty() ? QStringLiteral("Not reported") : list.join(", ");
}

QString join_ints(const std::vector<int>& values) {
    QStringList list;
    for (const int value : values) {
        list.push_back(QString::number(value));
    }
    return list.isEmpty() ? QStringLiteral("Not reported") : list.join(", ");
}

QString format_details(const PrinterDetails& details) {
    const auto& caps = details.capabilities;
    const auto& status = details.status;

    QString text;
    text += QString("Status source: %1\n").arg(QString::fromStdString(status.source));
    text += QString("Capability source: %1\n").arg(QString::fromStdString(caps.source));
    text += QString("State: %1\n").arg(state_name(status.state));
    text += QString("Accepting jobs: %1\n").arg(status.accepting_jobs ? "yes" : "no");
    text += QString("Reasons: %1\n\n").arg(join_strings(status.reasons));

    text += "Capabilities\n";
    text += QString("  Color: %1\n").arg(join_strings(caps.color_modes));
    text += QString("  Duplex: %1\n").arg(join_strings(caps.sides));
    text += QString("  Quality: %1\n").arg(join_ints(caps.qualities));
    text += QString("  Resolution DPI: %1\n").arg(join_ints(caps.resolutions_dpi));
    text += QString("  Copies: %1-%2\n").arg(caps.copies_min).arg(caps.copies_max);
    text += QString("  Sources: %1\n").arg(join_strings(caps.media_sources));
    text += QString("  Media types: %1\n").arg(join_strings(caps.media_types));
    text += QString("  Document formats: %1\n").arg(join_strings(caps.document_formats));
    text += QString("  Media sizes: %1 advertised\n\n").arg(caps.media.size());

    text += "Supplies\n";
    if (status.supplies.empty()) {
        text += "  Not reported by device\n";
    }
    for (const auto& supply : status.supplies) {
        text += "  " + QString::fromStdString(supply.name) + ": ";
        if (supply.percent.has_value()) {
            text += QString::number(*supply.percent) + "%";
            if (*supply.percent <= supply.low_threshold) {
                text += " (low)";
            }
        } else {
            text += "level unavailable";
        }
        text += '\n';
    }

    return text;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("DocSuite Device Center");
    window.resize(1050, 650);

    auto* tabs = new QTabWidget(&window);
    auto* devices_page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(devices_page);
    auto* summary = new QLabel("Native CUPS/IPP, SANE and eSCL device discovery", devices_page);
    auto* refresh = new QPushButton("Refresh devices", devices_page);
    auto* splitter = new QSplitter(Qt::Horizontal, devices_page);

    auto* lists_widget = new QWidget(splitter);
    auto* lists_layout = new QVBoxLayout(lists_widget);
    auto* printers = new QListWidget(lists_widget);
    auto* scanners = new QListWidget(lists_widget);
    lists_layout->addWidget(new QLabel("Printers", lists_widget));
    lists_layout->addWidget(printers);
    lists_layout->addWidget(new QLabel("Scanners", lists_widget));
    lists_layout->addWidget(scanners);

    auto* details = new QTextEdit(splitter);
    details->setReadOnly(true);
    details->setPlaceholderText("Select a printer to load status and capabilities.");

    splitter->addWidget(lists_widget);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    layout->addWidget(summary);
    layout->addWidget(refresh);
    layout->addWidget(splitter);

    tabs->addTab(devices_page, "Devices");
    window.setCentralWidget(tabs);

    auto manager = std::make_shared<docsuite::DeviceManager>();

    QObject::connect(
        printers,
        &QListWidget::currentItemChanged,
        &window,
        [manager, printers, details, &window](QListWidgetItem* current, QListWidgetItem*) {
            if (current == nullptr) {
                details->clear();
                return;
            }

            const QString printer_qname = current->data(Qt::UserRole).toString();
            const std::string printer_name = printer_qname.toStdString();
            details->setPlainText("Loading printer status and capabilities…");

            auto* watcher = new QFutureWatcher<PrinterDetails>(&window);
            QObject::connect(
                watcher,
                &QFutureWatcher<PrinterDetails>::finished,
                &window,
                [watcher, printers, details, printer_qname]() {
                    try {
                        const PrinterDetails result = watcher->result();
                        const auto* selected = printers->currentItem();
                        if (selected != nullptr &&
                            selected->data(Qt::UserRole).toString() == printer_qname) {
                            details->setPlainText(format_details(result));
                        }
                    } catch (const std::exception& error) {
                        details->setPlainText(QString("Printer query error: %1").arg(error.what()));
                    }
                    watcher->deleteLater();
                });

            watcher->setFuture(QtConcurrent::run([manager, printer_name]() {
                return PrinterDetails{
                    .capabilities = manager->print_backend().capabilities(printer_name),
                    .status = manager->print_backend().status(printer_name),
                };
            }));
        });

    const auto reload = [manager, printers, scanners, details, summary, refresh, &window]() {
        refresh->setEnabled(false);
        summary->setText("Discovering devices…");
        details->clear();

        auto* watcher = new QFutureWatcher<docsuite::DeviceSnapshot>(&window);
        QObject::connect(
            watcher,
            &QFutureWatcher<docsuite::DeviceSnapshot>::finished,
            &window,
            [watcher, printers, scanners, summary, refresh]() {
                printers->clear();
                scanners->clear();
                try {
                    const auto snapshot = watcher->result();
                    for (const auto& printer : snapshot.printers) {
                        QString text = QString::fromStdString(printer.name);
                        if (!printer.model.empty()) {
                            text += " — " + QString::fromStdString(printer.model);
                        }
                        if (printer.is_default) {
                            text += " [default]";
                        }

                        auto* item = new QListWidgetItem(text, printers);
                        item->setData(Qt::UserRole, QString::fromStdString(printer.name));
                    }

                    for (const auto& scanner : snapshot.scanners) {
                        QString text = QString::fromStdString(
                            scanner.name + " — " + scanner.vendor + " " + scanner.model);
                        if (!scanner.backend.empty()) {
                            text += " [" + QString::fromStdString(scanner.backend) + "]";
                        }
                        scanners->addItem(text);
                    }

                    summary->setText(
                        QString("%1 printer(s), %2 scanner(s)")
                            .arg(snapshot.printers.size())
                            .arg(snapshot.scanners.size()));

                    if (printers->count() > 0) {
                        printers->setCurrentRow(0);
                    }
                } catch (const std::exception& error) {
                    summary->setText(QString("Discovery error: %1").arg(error.what()));
                }
                refresh->setEnabled(true);
                watcher->deleteLater();
            });

        watcher->setFuture(QtConcurrent::run([manager]() {
            return manager->snapshot();
        }));
    };

    QObject::connect(refresh, &QPushButton::clicked, &window, reload);
    reload();

    window.show();
    return app.exec();
}
