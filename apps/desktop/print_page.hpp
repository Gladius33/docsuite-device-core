// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"

#include <QWidget>

#include <memory>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace docsuite::desktop {

class PrintPage final : public QWidget {
public:
    explicit PrintPage(std::shared_ptr<DeviceManager> manager, QWidget* parent = nullptr);
    void refresh_printers();
    void refresh_jobs();

private:
    void browse_file();
    void submit(bool diagnostic);
    void cancel_selected();
    [[nodiscard]] PrintProfile selected_profile() const;

    std::shared_ptr<DeviceManager> manager_;
    QComboBox* printer_{nullptr};
    QLineEdit* file_{nullptr};
    QComboBox* profile_{nullptr};
    QPushButton* refresh_printers_{nullptr};
    QPushButton* print_{nullptr};
    QPushButton* diagnose_{nullptr};
    QPushButton* refresh_jobs_{nullptr};
    QPushButton* cancel_{nullptr};
    QTableWidget* jobs_{nullptr};
    QPlainTextEdit* log_{nullptr};
};

} // namespace docsuite::desktop
