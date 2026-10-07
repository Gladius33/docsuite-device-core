// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "diagnostics_page.hpp"

#include "device_service_gateway.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <chrono>
#include <exception>
#include <sstream>
#include <string>

namespace docsuite::desktop {
namespace {

[[nodiscard]] const char* state_name(const DeviceState state) noexcept {
    switch (state) {
        case DeviceState::idle: return "idle";
        case DeviceState::processing: return "processing";
        case DeviceState::stopped: return "stopped";
        case DeviceState::offline: return "offline";
        case DeviceState::unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] long long elapsed_ms(const std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

[[nodiscard]] QString build_report(const std::shared_ptr<DeviceServiceGateway>& gateway) {
    std::ostringstream out;
    out << "DocSuite Device Center diagnostics\n";
    out << "Generated: "
        << QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString()
        << "\n";
    out << "Qt: " << qVersion() << "\n";

    const auto service_started = std::chrono::steady_clock::now();
    const bool service = gateway->service_available();
    out << "Persistent service: " << (service ? "connected" : "direct fallback")
        << " (" << elapsed_ms(service_started) << " ms)\n\n";

    try {
        const auto discovery_started = std::chrono::steady_clock::now();
        const DeviceSnapshot snapshot = gateway->snapshot();
        out << "Discovery: " << snapshot.printers.size() << " printer(s), "
            << snapshot.scanners.size() << " scanner(s)"
            << " (" << elapsed_ms(discovery_started) << " ms)\n\n";

        out << "PRINTERS\n";
        if (snapshot.printers.empty()) {
            out << "  none\n";
        }
        for (const auto& printer : snapshot.printers) {
            out << "- " << printer.name;
            if (!printer.model.empty()) out << " | " << printer.model;
            if (printer.is_default) out << " | default";
            out << "\n  URI: " << printer.uri << "\n";

            try {
                const auto started = std::chrono::steady_clock::now();
                const auto caps = gateway->printer_capabilities(printer.name, false);
                const auto caps_ms = elapsed_ms(started);
                out << "  Capabilities: source=" << caps.source
                    << ", color_modes=" << caps.color_modes.size()
                    << ", sides=" << caps.sides.size()
                    << ", media=" << caps.media.size()
                    << ", sources=" << caps.media_sources.size()
                    << ", native_formats=" << caps.document_formats.size()
                    << ", copies=" << caps.copies_min << '-' << caps.copies_max
                    << " (" << caps_ms << " ms)\n";
            } catch (const std::exception& error) {
                out << "  Capabilities ERROR: " << error.what() << "\n";
            }

            try {
                const auto started = std::chrono::steady_clock::now();
                const auto status = gateway->printer_status(printer.name);
                const auto status_ms = elapsed_ms(started);
                out << "  Status: source=" << status.source
                    << ", state=" << state_name(status.state)
                    << ", accepting=" << (status.accepting_jobs ? "yes" : "no")
                    << ", reasons=" << status.reasons.size()
                    << ", supplies=" << status.supplies.size()
                    << " (" << status_ms << " ms)\n";
                for (const auto& supply : status.supplies) {
                    out << "    supply " << supply.name << ": ";
                    if (supply.percent.has_value()) {
                        out << *supply.percent << "%";
                    } else {
                        out << "unavailable";
                    }
                    out << "\n";
                }
            } catch (const std::exception& error) {
                out << "  Status ERROR: " << error.what() << "\n";
            }
        }

        out << "\nSCANNERS\n";
        if (snapshot.scanners.empty()) {
            out << "  none\n";
        }
        for (const auto& scanner : snapshot.scanners) {
            out << "- " << scanner.name << "\n"
                << "  " << scanner.vendor << ' ' << scanner.model
                << " | backend=" << scanner.backend << "\n";
            try {
                const auto started = std::chrono::steady_clock::now();
                const auto caps = gateway->scanner_capabilities(scanner.name);
                out << "  Capabilities: source=" << caps.source
                    << ", modes=" << caps.modes.size()
                    << ", resolutions=" << caps.resolutions_dpi.size()
                    << ", sources=" << caps.sources.size()
                    << ", bed=" << caps.max_width_mm << "x" << caps.max_height_mm << " mm"
                    << " (" << elapsed_ms(started) << " ms)\n";
            } catch (const std::exception& error) {
                out << "  Capabilities ERROR: " << error.what() << "\n";
            }
        }
    } catch (const std::exception& error) {
        out << "Discovery ERROR: " << error.what() << "\n";
    }

    out << "\nNo print or scan action was performed by this diagnostic.\n";
    return QString::fromStdString(out.str());
}

} // namespace

DiagnosticsPage::DiagnosticsPage(
    std::shared_ptr<DeviceServiceGateway> gateway,
    QWidget* parent)
    : QWidget{parent}, gateway_{std::move(gateway)} {

    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout();
    run_ = new QPushButton(QStringLiteral("Run diagnostics"), this);
    copy_ = new QPushButton(QStringLiteral("Copy report"), this);
    save_ = new QPushButton(QStringLiteral("Save report…"), this);
    toolbar->addWidget(run_);
    toolbar->addWidget(copy_);
    toolbar->addWidget(save_);
    toolbar->addStretch();

    report_ = new QPlainTextEdit(this);
    report_->setReadOnly(true);
    report_->setLineWrapMode(QPlainTextEdit::NoWrap);
    report_->setPlaceholderText(QStringLiteral(
        "Run a non-destructive diagnostic of the local service, printers and scanners."));

    layout->addLayout(toolbar);
    layout->addWidget(report_, 1);

    connect(run_, &QPushButton::clicked, this, [this]() { run_diagnostics(); });
    connect(copy_, &QPushButton::clicked, this, [this]() { copy_report(); });
    connect(save_, &QPushButton::clicked, this, [this]() { save_report(); });

    run_diagnostics();
}

void DiagnosticsPage::run_diagnostics() {
    run_->setEnabled(false);
    report_->setPlainText(QStringLiteral("Running non-destructive diagnostics…"));

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
        try {
            report_->setPlainText(watcher->result());
        } catch (const std::exception& error) {
            report_->setPlainText(
                QStringLiteral("Diagnostic error: %1").arg(QString::fromUtf8(error.what())));
        }
        run_->setEnabled(true);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([gateway = gateway_]() {
        return build_report(gateway);
    }));
}

void DiagnosticsPage::copy_report() const {
    QApplication::clipboard()->setText(report_->toPlainText());
}

void DiagnosticsPage::save_report() {
    const QString suggested = QStringLiteral("docsuite-diagnostics-%1.txt")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save DocSuite diagnostics"),
        suggested,
        QStringLiteral("Text report (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(
            this,
            QStringLiteral("DocSuite Diagnostics"),
            QStringLiteral("Unable to write %1").arg(path));
        return;
    }
    QTextStream stream(&file);
    stream << report_->toPlainText();
}

} // namespace docsuite::desktop
