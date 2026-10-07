// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <exception>
#include <memory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("DocSuite Device Center");
    window.resize(900, 560);

    auto* tabs = new QTabWidget(&window);
    auto* devices_page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(devices_page);
    auto* summary = new QLabel("Native CUPS/IPP and SANE device discovery", devices_page);
    auto* refresh = new QPushButton("Refresh", devices_page);
    auto* printers = new QListWidget(devices_page);
    auto* scanners = new QListWidget(devices_page);

    layout->addWidget(summary);
    layout->addWidget(refresh);
    layout->addWidget(new QLabel("Printers", devices_page));
    layout->addWidget(printers);
    layout->addWidget(new QLabel("Scanners", devices_page));
    layout->addWidget(scanners);

    tabs->addTab(devices_page, "Devices");
    window.setCentralWidget(tabs);

    auto manager = std::make_shared<docsuite::DeviceManager>();

    const auto reload = [manager, printers, scanners, summary]() {
        printers->clear();
        scanners->clear();
        try {
            const auto snapshot = manager->snapshot();
            for (const auto& printer : snapshot.printers) {
                QString text = QString::fromStdString(printer.name);
                if (!printer.model.empty()) {
                    text += " — " + QString::fromStdString(printer.model);
                }
                if (printer.is_default) {
                    text += " [default]";
                }
                printers->addItem(text);
            }
            for (const auto& scanner : snapshot.scanners) {
                scanners->addItem(
                    QString::fromStdString(scanner.name + " — " + scanner.vendor + " " + scanner.model));
            }
            summary->setText(
                QString("%1 printer(s), %2 scanner(s)")
                    .arg(snapshot.printers.size())
                    .arg(snapshot.scanners.size()));
        } catch (const std::exception& error) {
            summary->setText(QString("Discovery error: %1").arg(error.what()));
        }
    };

    QObject::connect(refresh, &QPushButton::clicked, &window, reload);
    reload();

    window.show();
    return app.exec();
}
