// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <QWidget>

#include <memory>

class QPlainTextEdit;
class QPushButton;

namespace docsuite::desktop {

class DeviceServiceGateway;

class DiagnosticsPage final : public QWidget {
public:
    explicit DiagnosticsPage(
        std::shared_ptr<DeviceServiceGateway> gateway,
        QWidget* parent = nullptr);

private:
    void run_diagnostics();
    void copy_report() const;
    void save_report();

    std::shared_ptr<DeviceServiceGateway> gateway_;
    QPushButton* run_{nullptr};
    QPushButton* copy_{nullptr};
    QPushButton* save_{nullptr};
    QPlainTextEdit* report_{nullptr};
};

} // namespace docsuite::desktop
