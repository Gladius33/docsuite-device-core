// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "main_window.hpp"

#include "devices_page.hpp"
#include "print_page.hpp"
#include "scan_page.hpp"

#include <QTabWidget>

namespace docsuite::desktop {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow{parent}, manager_{std::make_shared<DeviceManager>()} {

    setWindowTitle(QStringLiteral("DocSuite Device Center"));
    resize(1280, 860);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(new DevicesPage(manager_, tabs), QStringLiteral("Devices"));
    tabs->addTab(new ScanPage(manager_, tabs), QStringLiteral("Scan / OCR"));
    tabs->addTab(new PrintPage(manager_, tabs), QStringLiteral("Print / Jobs"));
    setCentralWidget(tabs);
}

} // namespace docsuite::desktop
