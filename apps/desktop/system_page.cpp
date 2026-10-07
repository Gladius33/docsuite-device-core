// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "system_page.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>

namespace docsuite::desktop {
namespace {

[[nodiscard]] bool is_ipp_uri(const QString& uri) {
    return uri.startsWith(QStringLiteral("ipp://"), Qt::CaseInsensitive) ||
        uri.startsWith(QStringLiteral("ipps://"), Qt::CaseInsensitive);
}

[[nodiscard]] QString sanitize_queue_name(QString value) {
    value = value.trimmed();
    QString result;
    result.reserve(value.size() + 9);
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
            ch == QLatin1Char('.')) {
            result += ch;
        } else if (!result.endsWith(QLatin1Char('_'))) {
            result += QLatin1Char('_');
        }
    }
    while (result.endsWith(QLatin1Char('_'))) {
        result.chop(1);
    }
    if (result.isEmpty()) {
        result = QStringLiteral("printer");
    }
    return QStringLiteral("docsuite_") + result.left(110);
}

} // namespace

SystemPage::SystemPage(std::shared_ptr<DeviceManager> manager, QWidget* parent)
    : QWidget{parent}, manager_{std::move(manager)} {

    auto* root = new QVBoxLayout(this);
    auto* group = new QGroupBox(QStringLiteral("Linux / CUPS integration"), this);
    auto* form = new QFormLayout(group);

    device_ = new QComboBox(group);
    queue_name_ = new QLineEdit(group);
    selected_uri_ = new QLabel(group);
    selected_uri_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    selected_uri_->setWordWrap(true);
    queue_state_ = new QLabel(QStringLiteral("No queue inspected yet"), group);
    queue_state_->setWordWrap(true);

    form->addRow(QStringLiteral("IPP Everywhere device"), device_);
    form->addRow(QStringLiteral("Local queue name"), queue_name_);
    form->addRow(QStringLiteral("Device URI"), selected_uri_);
    form->addRow(QStringLiteral("State"), queue_state_);

    auto* actions = new QHBoxLayout();
    refresh_ = new QPushButton(QStringLiteral("Refresh"), group);
    create_ = new QPushButton(QStringLiteral("Create / reuse driverless queue"), group);
    actions->addWidget(refresh_);
    actions->addWidget(create_);
    actions->addStretch();
    form->addRow(actions);

    explanation_ = new QLabel(
        QStringLiteral(
            "This uses the CUPS Create-Local-Printer operation, not lpadmin. "
            "The queue exposes standard IPP capabilities to Linux applications. "
            "CUPS local queues created this way are temporary and can disappear after a CUPS or system restart; "
            "making a queue permanently shared remains an administrator action."),
        this);
    explanation_->setWordWrap(true);

    root->addWidget(group);
    root->addWidget(explanation_);
    root->addStretch();

    connect(refresh_, &QPushButton::clicked, this, [this]() { refresh_devices(); });
    connect(device_, &QComboBox::currentIndexChanged, this, [this](int) { update_selection(); });
    connect(queue_name_, &QLineEdit::textChanged, this, [this](const QString&) { update_selection(); });
    connect(create_, &QPushButton::clicked, this, [this]() { create_queue(); });

    refresh_devices();
}

QString SystemPage::suggested_queue_name() const {
    const QString name = device_->currentData(Qt::UserRole + 1).toString();
    return sanitize_queue_name(name.isEmpty() ? device_->currentText() : name);
}

void SystemPage::refresh_devices() {
    refresh_->setEnabled(false);
    create_->setEnabled(false);
    queue_state_->setText(QStringLiteral("Discovering IPP destinations…"));
    const QString previous_uri = device_->currentData().toString();

    auto* watcher = new QFutureWatcher<DeviceSnapshot>(this);
    connect(watcher, &QFutureWatcher<DeviceSnapshot>::finished, this,
        [this, watcher, previous_uri]() {
            try {
                const auto snapshot = watcher->result();
                const QSignalBlocker blocker{device_};
                device_->clear();
                int restore = -1;
                for (const auto& printer : snapshot.printers) {
                    const QString uri = QString::fromStdString(printer.uri);
                    if (!is_ipp_uri(uri)) {
                        continue;
                    }
                    QString label = QString::fromStdString(printer.name);
                    if (!printer.model.empty()) {
                        label += QStringLiteral(" — ") + QString::fromStdString(printer.model);
                    }
                    device_->addItem(label, uri);
                    device_->setItemData(
                        device_->count() - 1,
                        QString::fromStdString(printer.name),
                        Qt::UserRole + 1);
                    if (uri == previous_uri) {
                        restore = device_->count() - 1;
                    }
                }
                if (restore >= 0) {
                    device_->setCurrentIndex(restore);
                }
                if (queue_name_->text().trimmed().isEmpty() && device_->count() > 0) {
                    queue_name_->setText(suggested_queue_name());
                }
                queue_state_->setText(
                    device_->count() > 0
                        ? QStringLiteral("Direct IPP destination available")
                        : QStringLiteral("No ipp:// or ipps:// destination discovered"));
            } catch (const std::exception& error) {
                queue_state_->setText(
                    QStringLiteral("Discovery error: %1").arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            watcher->deleteLater();
            update_selection();
        });
    watcher->setFuture(QtConcurrent::run([manager = manager_]() { return manager->snapshot(); }));
}

void SystemPage::update_selection() {
    const QString uri = device_->currentData().toString();
    selected_uri_->setText(uri.isEmpty() ? QStringLiteral("—") : uri);
    const QString name = queue_name_->text().trimmed();
    create_->setEnabled(is_ipp_uri(uri) && !name.isEmpty());

    if (name.isEmpty()) {
        return;
    }

    try {
        const auto state = queue_manager_.inspect(name.toStdString());
        if (!state.exists) {
            queue_state_->setText(QStringLiteral("Queue does not exist yet"));
        } else {
            queue_state_->setText(
                QStringLiteral("Existing queue — %1 — %2")
                    .arg(state.driverless ? QStringLiteral("driverless IPP")
                                          : QStringLiteral("non-driverless"))
                    .arg(QString::fromStdString(state.device_uri)));
        }
    } catch (const std::exception& error) {
        queue_state_->setText(
            QStringLiteral("Queue name error: %1").arg(QString::fromUtf8(error.what())));
        create_->setEnabled(false);
    }
}

void SystemPage::create_queue() {
    const QString uri = device_->currentData().toString();
    const QString name = queue_name_->text().trimmed();
    if (!is_ipp_uri(uri) || name.isEmpty()) {
        return;
    }

    create_->setEnabled(false);
    refresh_->setEnabled(false);
    queue_state_->setText(QStringLiteral("Creating local IPP Everywhere queue through CUPS…"));

    auto* watcher = new QFutureWatcher<SystemQueueInfo>(this);
    connect(watcher, &QFutureWatcher<SystemQueueInfo>::finished, this,
        [this, watcher]() {
            try {
                const auto result = watcher->result();
                queue_state_->setText(
                    QStringLiteral("Ready: %1 → %2%3")
                        .arg(QString::fromStdString(result.name))
                        .arg(QString::fromStdString(result.device_uri))
                        .arg(result.temporary ? QStringLiteral(" [temporary]") : QString{}));
            } catch (const std::exception& error) {
                queue_state_->setText(
                    QStringLiteral("CUPS integration error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            refresh_->setEnabled(true);
            update_selection();
            watcher->deleteLater();
        });

    const std::string queue = name.toStdString();
    const std::string device_uri = uri.toStdString();
    watcher->setFuture(QtConcurrent::run([queue, device_uri]() {
        SystemQueueManager manager;
        return manager.ensure_temporary_driverless(
            queue,
            device_uri,
            "DocSuite IPP Everywhere printer");
    }));
}

} // namespace docsuite::desktop
