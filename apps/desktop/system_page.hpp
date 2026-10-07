// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/print/system_queue_manager.hpp"

#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace docsuite::desktop {

class SystemPage final : public QWidget {
public:
    explicit SystemPage(std::shared_ptr<DeviceManager> manager, QWidget* parent = nullptr);

private:
    void refresh_devices();
    void update_selection();
    void create_queue();
    void install_profiles();
    void remove_profiles();
    void refresh_profiles();
    [[nodiscard]] QString suggested_queue_name() const;

    std::shared_ptr<DeviceManager> manager_;
    SystemQueueManager queue_manager_{};
    QComboBox* device_{nullptr};
    QLineEdit* queue_name_{nullptr};
    QLabel* selected_uri_{nullptr};
    QLabel* queue_state_{nullptr};
    QLabel* profiles_state_{nullptr};
    QLabel* explanation_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* create_{nullptr};
    QPushButton* install_profiles_{nullptr};
    QPushButton* remove_profiles_{nullptr};
    QPushButton* refresh_profiles_{nullptr};
};

} // namespace docsuite::desktop
