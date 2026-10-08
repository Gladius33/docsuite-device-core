// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <QWidget>

#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
class QTextEdit;

namespace docsuite::desktop {

class DeviceServiceGateway;

class DevicesPage final : public QWidget {
public:
    explicit DevicesPage(
        std::shared_ptr<DeviceServiceGateway> gateway,
        QWidget* parent = nullptr);
    void refresh(bool refresh_selected_details = false);
    void refresh_selected_details();

private:
    void load_printer_details(const QString& printer_name);

    std::shared_ptr<DeviceServiceGateway> gateway_;
    QLabel* summary_{nullptr};
    QPushButton* refresh_button_{nullptr};
    QListWidget* printers_{nullptr};
    QListWidget* scanners_{nullptr};
    QTextEdit* details_{nullptr};
    bool refresh_in_progress_{false};
    bool details_refresh_in_progress_{false};
    QString details_printer_{};
    QString pending_details_printer_{};
};

} // namespace docsuite::desktop
