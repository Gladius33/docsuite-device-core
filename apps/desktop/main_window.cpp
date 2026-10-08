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
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace docsuite::desktop {
namespace {

[[nodiscard]] QWidget* placeholder(const QString& text, QWidget* parent) {
    auto* widget = new QWidget(parent);
    auto* layout = new QVBoxLayout(widget);
    auto* label = new QLabel(text, widget);
    label->setAlignment(Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(label);
    layout->addStretch();
    return widget;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow{parent},
      manager_{std::make_shared<DeviceManager>()},
      gateway_{std::make_shared<DeviceServiceGateway>(manager_)} {

    setWindowTitle(QStringLiteral("DocSuite Device Center"));
    resize(1280, 860);

    tabs_ = new QTabWidget(this);
    devices_page_ = new DevicesPage(gateway_, tabs_);
    tabs_->addTab(devices_page_, QStringLiteral("Devices"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize scanner tools."), tabs_), QStringLiteral("Scan / OCR"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize document tools."), tabs_), QStringLiteral("Document"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize copy tools."), tabs_), QStringLiteral("Copy"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize printing tools."), tabs_), QStringLiteral("Print / Jobs"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize diagnostics."), tabs_), QStringLiteral("Diagnostics"));
    tabs_->addTab(placeholder(QStringLiteral("Open this tab to initialize system integration."), tabs_), QStringLiteral("System"));
    setCentralWidget(tabs_);

    connect(tabs_, &QTabWidget::currentChanged, this, [this](const int index) {
        const bool was_loaded =
            index >= 0 && index < static_cast<int>(tab_loaded_.size()) && tab_loaded_[index];
        ensure_tab_loaded(index);
        if (was_loaded) {
            refresh_active_tab(index);
        }
        QSettings{}.setValue(QStringLiteral("window/tab"), index);
    });

    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const int saved_tab = settings.value(QStringLiteral("window/tab"), 0).toInt();
    if (saved_tab >= 0 && saved_tab < tabs_->count() && saved_tab != 0) {
        tabs_->setCurrentIndex(saved_tab);
        ensure_tab_loaded(saved_tab);
    }

    service_status_ = new QLabel(QStringLiteral("Service: checking…"), this);
    service_status_->setToolTip(QStringLiteral(
        "The GUI prefers the persistent DocSuite service for read-only device data and falls back to direct access if unavailable."));
    statusBar()->addPermanentWidget(service_status_);

    service_timer_ = new QTimer(this);
    service_timer_->setInterval(30000);
    connect(service_timer_, &QTimer::timeout, this, [this]() { refresh_service_status(); });
    service_timer_->start();
    refresh_service_status();

    discovery_timer_ = new QTimer(this);
    discovery_timer_->setInterval(15000);
    connect(discovery_timer_, &QTimer::timeout, this, [this]() {
        if (tabs_->currentIndex() == 0 && devices_page_ != nullptr) {
            devices_page_->refresh();
        }
    });
    discovery_timer_->start();
}

MainWindow::~MainWindow() {
    if (service_timer_ != nullptr) {
        service_timer_->stop();
    }
    if (discovery_timer_ != nullptr) {
        discovery_timer_->stop();
    }

    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (tabs_ != nullptr) {
        settings.setValue(QStringLiteral("window/tab"), tabs_->currentIndex());
    }
}

void MainWindow::ensure_tab_loaded(const int index) {
    if (index < 0 || index >= static_cast<int>(tab_loaded_.size()) || tab_loaded_[index]) {
        return;
    }

    QWidget* replacement = nullptr;
    switch (index) {
        case 1: replacement = new ScanPage(manager_, tabs_); break;
        case 2: replacement = new DocumentPage(manager_, tabs_); break;
        case 3: replacement = new CopyPage(manager_, tabs_); break;
        case 4: replacement = new PrintPage(manager_, tabs_); break;
        case 5: replacement = new DiagnosticsPage(gateway_, tabs_); break;
        case 6: replacement = new SystemPage(manager_, tabs_); break;
        default: return;
    }

    tab_loaded_[index] = true;
    QWidget* old = tabs_->widget(index);
    const QString title = tabs_->tabText(index);
    const QSignalBlocker blocker{tabs_};
    tabs_->removeTab(index);
    tabs_->insertTab(index, replacement, title);
    tabs_->setCurrentIndex(index);
    if (old != nullptr) {
        old->deleteLater();
    }
}

void MainWindow::refresh_active_tab(const int index) {
    if (index == 0 && devices_page_ != nullptr) {
        devices_page_->refresh();
        return;
    }

    if (index < 0 || index >= static_cast<int>(tab_loaded_.size()) || !tab_loaded_[index]) {
        return;
    }

    if (index == 1) {
        if (auto* page = dynamic_cast<ScanPage*>(tabs_->widget(index)); page != nullptr) {
            page->refresh_scanners();
        }
    } else if (index == 3) {
        if (auto* page = dynamic_cast<CopyPage*>(tabs_->widget(index)); page != nullptr) {
            page->refresh_devices();
        }
    } else if (index == 4) {
        if (auto* page = dynamic_cast<PrintPage*>(tabs_->widget(index)); page != nullptr) {
            page->refresh_printers();
            page->refresh_jobs();
        }
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
