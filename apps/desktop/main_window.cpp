// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "main_window.hpp"

#include "copy_page.hpp"
#include "device_service_gateway.hpp"
#include "devices_page.hpp"
#include "diagnostics_page.hpp"
#include "document_page.hpp"
#include "print_page.hpp"
#include "scan_page.hpp"
#include "system_page.hpp"

#include <QFutureWatcher>
#include <QLabel>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QtConcurrent>

namespace docsuite::desktop {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow{parent},
      manager_{std::make_shared<DeviceManager>()},
      gateway_{std::make_shared<DeviceServiceGateway>(manager_)} {

    setWindowTitle(QStringLiteral("DocSuite Device Center"));
    resize(1280, 860);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(new DevicesPage(gateway_, tabs_), QStringLiteral("Devices"));
    tabs_->addTab(new ScanPage(manager_, tabs_), QStringLiteral("Scan / OCR"));
    tabs_->addTab(new DocumentPage(manager_, tabs_), QStringLiteral("Document"));
    tabs_->addTab(new CopyPage(manager_, tabs_), QStringLiteral("Copy"));
    tabs_->addTab(new PrintPage(manager_, tabs_), QStringLiteral("Print / Jobs"));
    tabs_->addTab(new DiagnosticsPage(gateway_, tabs_), QStringLiteral("Diagnostics"));
    tabs_->addTab(new SystemPage(manager_, tabs_), QStringLiteral("System"));
    setCentralWidget(tabs_);

    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const int saved_tab = settings.value(QStringLiteral("window/tab"), 0).toInt();
    if (saved_tab >= 0 && saved_tab < tabs_->count()) {
        tabs_->setCurrentIndex(saved_tab);
    }
    connect(tabs_, &QTabWidget::currentChanged, this, [](const int index) {
        QSettings{}.setValue(QStringLiteral("window/tab"), index);
    });

    service_status_ = new QLabel(QStringLiteral("Service: checking…"), this);
    service_status_->setToolTip(QStringLiteral(
        "The GUI prefers the persistent DocSuite service for read-only device data and falls back to direct access if unavailable."));
    statusBar()->addPermanentWidget(service_status_);

    service_timer_ = new QTimer(this);
    service_timer_->setInterval(30000);
    connect(service_timer_, &QTimer::timeout, this, [this]() { refresh_service_status(); });
    service_timer_->start();
    refresh_service_status();
}

MainWindow::~MainWindow() {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (tabs_ != nullptr) {
        settings.setValue(QStringLiteral("window/tab"), tabs_->currentIndex());
    }
}

void MainWindow::refresh_service_status() {
    service_status_->setText(QStringLiteral("Service: checking…"));
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher]() {
        const bool available = watcher->result();
        service_status_->setText(
            available
                ? QStringLiteral("Service: connected")
                : QStringLiteral("Service: direct fallback"));
        service_status_->setToolTip(
            available
                ? QStringLiteral("Persistent service connected; device capability cache is shared with MCP and other clients.")
                : QStringLiteral("Persistent service is not running; GUI remains fully functional through direct DeviceManager access."));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([gateway = gateway_]() {
        return gateway->service_available();
    }));
}

} // namespace docsuite::desktop
