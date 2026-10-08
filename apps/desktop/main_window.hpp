// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"

#include <QMainWindow>

#include <array>
#include <memory>

class QLabel;
class QTabWidget;
class QTimer;

namespace docsuite::desktop {

class DeviceServiceGateway;
class DevicesPage;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void refresh_service_status();
    void ensure_tab_loaded(int index);
    void refresh_active_tab(int index);

    std::shared_ptr<DeviceManager> manager_;
    std::shared_ptr<DeviceServiceGateway> gateway_;
    QTabWidget* tabs_{nullptr};
    DevicesPage* devices_page_{nullptr};
    QLabel* service_status_{nullptr};
    QTimer* service_timer_{nullptr};
    QTimer* discovery_timer_{nullptr};
    std::array<bool, 7> tab_loaded_{{true, false, false, false, false, false, false}};
};

} // namespace docsuite::desktop
