// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"

#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace docsuite::desktop {

class CopyPage final : public QWidget {
public:
    explicit CopyPage(std::shared_ptr<DeviceManager> manager, QWidget* parent = nullptr);
    void refresh_devices();

private:
    void refresh_scanner_capabilities();
    void refresh_printer_capabilities();
    void start_copy();

    std::shared_ptr<DeviceManager> manager_;
    QComboBox* scanner_{nullptr};
    QComboBox* printer_{nullptr};
    QComboBox* scan_mode_{nullptr};
    QComboBox* dpi_{nullptr};
    QComboBox* scan_source_{nullptr};
    QComboBox* print_mode_{nullptr};
    QComboBox* duplex_{nullptr};
    QComboBox* quality_{nullptr};
    QComboBox* media_{nullptr};
    QComboBox* print_source_{nullptr};
    QComboBox* media_type_{nullptr};
    QSpinBox* copies_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* copy_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace docsuite::desktop
