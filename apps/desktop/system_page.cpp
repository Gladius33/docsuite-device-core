// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "system_page.hpp"

#include "docsuite/print/print_validation.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] bool has_value(
    const std::vector<std::string>& values,
    const std::string& wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

[[nodiscard]] std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string first_matching(
    const std::vector<std::string>& values,
    const std::vector<std::string>& needles) {

    for (const auto& value : values) {
        const std::string lowered = lower_copy(value);
        for (const auto& needle : needles) {
            if (lowered.find(needle) != std::string::npos) {
                return value;
            }
        }
    }
    return {};
}

[[nodiscard]] std::string preferred_media(const PrinterCapabilities& caps) {
    if (has_value(caps.media, "iso_a4_210x297mm")) {
        return "iso_a4_210x297mm";
    }
    return caps.media.empty() ? "iso_a4_210x297mm" : caps.media.front();
}

[[nodiscard]] std::vector<std::pair<std::string, PrintProfile>> built_in_profiles(
    const PrinterCapabilities& caps) {

    const std::string media = preferred_media(caps);

    PrintProfile monochrome;
    monochrome.name = "Noir et blanc";
    monochrome.media = media;
    monochrome.color_mode = "monochrome";
    monochrome.sides = "one-sided";
    monochrome.quality = 4;

    PrintProfile draft = monochrome;
    draft.name = "Brouillon rapide";
    draft.quality = 3;

    PrintProfile duplex = monochrome;
    duplex.name = "Noir et blanc recto-verso";
    duplex.sides = "two-sided-long-edge";

    PrintProfile photo;
    photo.name = "Photo haute qualité";
    photo.media = first_matching(
        caps.media,
        {"4x6", "10x15", "photo", "index"});
    if (photo.media.empty()) {
        photo.media = media;
    }
    photo.color_mode = "color";
    photo.sides = "one-sided";
    photo.quality = 5;
    photo.media_type = first_matching(
        caps.media_types,
        {"photographic", "photo", "gloss"});
    if (has_value(caps.media_sources, "rear")) {
        photo.media_source = "rear";
    }

    // Build the vector explicitly. GCC 16 can emit a false-positive
    // -Wfree-nonheap-object when an initializer_list contains moved profile
    // objects and the vector destructor is aggressively inlined.
    std::vector<std::pair<std::string, PrintProfile>> profiles;
    profiles.reserve(4U);
    profiles.emplace_back("mono", std::move(monochrome));
    profiles.emplace_back("draft", std::move(draft));
    profiles.emplace_back("duplex", std::move(duplex));
    profiles.emplace_back("photo", std::move(photo));
    return profiles;
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

    auto* profiles_group = new QGroupBox(QStringLiteral("Per-user CUPS profiles"), this);
    auto* profiles_layout = new QVBoxLayout(profiles_group);
    profiles_state_ = new QLabel(QStringLiteral("No saved profiles inspected yet"), profiles_group);
    profiles_state_->setWordWrap(true);
    auto* profile_actions = new QHBoxLayout();
    install_profiles_ = new QPushButton(
        QStringLiteral("Install validated profiles"),
        profiles_group);
    remove_profiles_ = new QPushButton(
        QStringLiteral("Remove DocSuite profiles"),
        profiles_group);
    refresh_profiles_ = new QPushButton(QStringLiteral("Refresh profiles"), profiles_group);
    profile_actions->addWidget(install_profiles_);
    profile_actions->addWidget(remove_profiles_);
    profile_actions->addWidget(refresh_profiles_);
    profile_actions->addStretch();
    profiles_layout->addWidget(profiles_state_);
    profiles_layout->addLayout(profile_actions);

    explanation_ = new QLabel(
        QStringLiteral(
            "The driverless queue uses the CUPS Create-Local-Printer operation, not lpadmin. "
            "Saved profiles are CUPS destination instances in the current user's lpoptions, not extra physical printers. "
            "Profiles are created only when their exact standard IPP values are supported. "
            "Local queues created through Create-Local-Printer are temporary and can disappear after a CUPS or system restart; "
            "making a queue permanently shared remains an administrator action."),
        this);
    explanation_->setWordWrap(true);

    root->addWidget(group);
    root->addWidget(profiles_group);
    root->addWidget(explanation_);
    root->addStretch();

    connect(refresh_, &QPushButton::clicked, this, [this]() { refresh_devices(); });
    connect(device_, &QComboBox::currentIndexChanged, this, [this](int) { update_selection(); });
    connect(queue_name_, &QLineEdit::textChanged, this, [this](const QString&) { update_selection(); });
    connect(create_, &QPushButton::clicked, this, [this]() { create_queue(); });
    connect(install_profiles_, &QPushButton::clicked, this, [this]() { install_profiles(); });
    connect(remove_profiles_, &QPushButton::clicked, this, [this]() { remove_profiles(); });
    connect(refresh_profiles_, &QPushButton::clicked, this, [this]() { refresh_profiles(); });

    refresh_devices();
}

QString SystemPage::suggested_queue_name() const {
    const QString name = device_->currentData(Qt::UserRole + 1).toString();
    return sanitize_queue_name(name.isEmpty() ? device_->currentText() : name);
}

void SystemPage::refresh_devices() {
    refresh_->setEnabled(false);
    create_->setEnabled(false);
    install_profiles_->setEnabled(false);
    remove_profiles_->setEnabled(false);
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
    install_profiles_->setEnabled(false);
    remove_profiles_->setEnabled(false);
    refresh_profiles_->setEnabled(false);

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
            install_profiles_->setEnabled(state.driverless);
            remove_profiles_->setEnabled(state.driverless);
            refresh_profiles_->setEnabled(true);
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
            refresh_profiles();
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

void SystemPage::install_profiles() {
    const QString queue_text = queue_name_->text().trimmed();
    if (queue_text.isEmpty()) {
        return;
    }

    install_profiles_->setEnabled(false);
    remove_profiles_->setEnabled(false);
    refresh_profiles_->setEnabled(false);
    profiles_state_->setText(
        QStringLiteral("Validating capabilities and saving user profiles…"));

    const std::string queue = queue_text.toStdString();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
        [this, watcher]() {
            try {
                profiles_state_->setText(watcher->result());
            } catch (const std::exception& error) {
                profiles_state_->setText(
                    QStringLiteral("Profile installation error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            update_selection();
            watcher->deleteLater();
        });

    watcher->setFuture(QtConcurrent::run([manager = manager_, queue]() {
        const auto caps = manager->print_backend().capabilities(queue, false);
        SystemQueueManager system;
        QStringList installed;
        QStringList skipped;

        for (const auto& [instance, profile] : built_in_profiles(caps)) {
            const auto validation = validate_print_profile(caps, profile);
            if (!validation.ok) {
                skipped << QString::fromStdString(instance);
                continue;
            }
            (void)system.save_user_profile(queue, instance, profile);
            installed << QString::fromStdString(instance);
        }

        QString summary = QStringLiteral("Installed: %1")
            .arg(installed.isEmpty() ? QStringLiteral("none") : installed.join(QStringLiteral(", ")));
        if (!skipped.isEmpty()) {
            summary += QStringLiteral(". Skipped as unsupported: %1").arg(
                skipped.join(QStringLiteral(", ")));
        }
        return summary;
    }));
}

void SystemPage::remove_profiles() {
    const QString queue_text = queue_name_->text().trimmed();
    if (queue_text.isEmpty()) {
        return;
    }

    install_profiles_->setEnabled(false);
    remove_profiles_->setEnabled(false);
    refresh_profiles_->setEnabled(false);
    profiles_state_->setText(QStringLiteral("Removing DocSuite user profiles…"));

    const std::string queue = queue_text.toStdString();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
        [this, watcher]() {
            try {
                profiles_state_->setText(watcher->result());
            } catch (const std::exception& error) {
                profiles_state_->setText(
                    QStringLiteral("Profile removal error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            update_selection();
            watcher->deleteLater();
        });

    watcher->setFuture(QtConcurrent::run([queue]() {
        SystemQueueManager system;
        int removed = 0;
        for (const char* instance : {"mono", "draft", "duplex", "photo"}) {
            if (system.remove_user_profile(queue, instance)) {
                ++removed;
            }
        }
        return QStringLiteral("Removed %1 DocSuite profile(s)").arg(removed);
    }));
}

void SystemPage::refresh_profiles() {
    const QString queue_text = queue_name_->text().trimmed();
    if (queue_text.isEmpty()) {
        return;
    }

    refresh_profiles_->setEnabled(false);
    const std::string queue = queue_text.toStdString();
    auto* watcher = new QFutureWatcher<std::vector<UserPrintProfileInfo>>(this);
    connect(watcher, &QFutureWatcher<std::vector<UserPrintProfileInfo>>::finished, this,
        [this, watcher]() {
            try {
                const auto profiles = watcher->result();
                QStringList names;
                for (const auto& profile : profiles) {
                    names << QString::fromStdString(profile.instance);
                }
                profiles_state_->setText(
                    names.isEmpty()
                        ? QStringLiteral("No per-user profile saved for this queue")
                        : QStringLiteral("Saved instances: %1").arg(
                              names.join(QStringLiteral(", "))));
            } catch (const std::exception& error) {
                profiles_state_->setText(
                    QStringLiteral("Profile inspection error: %1")
                        .arg(QString::fromUtf8(error.what())));
            }
            update_selection();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run([queue]() {
        SystemQueueManager system;
        return system.list_user_profiles(queue);
    }));
}

} // namespace docsuite::desktop