// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "device_service_gateway.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace docsuite::desktop {
namespace {

[[nodiscard]] bool smoke_test_mode() {
    const char* value = std::getenv("DOCSUITE_SMOKE_TEST");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[nodiscard]] QString socket_path() {
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }
    if (runtime.isEmpty()) {
        runtime = QDir::tempPath();
    }
    return QDir(runtime).filePath(QStringLiteral("docsuite-device-core.sock"));
}

[[nodiscard]] std::vector<std::string> string_vector(const QJsonValue& value) {
    std::vector<std::string> result;
    const QJsonArray array = value.toArray();
    result.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& item : array) {
        if (item.isString()) {
            result.push_back(item.toString().toStdString());
        }
    }
    return result;
}

[[nodiscard]] std::vector<int> int_vector(const QJsonValue& value) {
    std::vector<int> result;
    const QJsonArray array = value.toArray();
    result.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& item : array) {
        if (item.isDouble()) {
            result.push_back(item.toInt());
        }
    }
    return result;
}

[[nodiscard]] DeviceState device_state(const QString& value) {
    if (value == QStringLiteral("idle")) return DeviceState::idle;
    if (value == QStringLiteral("processing")) return DeviceState::processing;
    if (value == QStringLiteral("stopped")) return DeviceState::stopped;
    if (value == QStringLiteral("offline")) return DeviceState::offline;
    return DeviceState::unknown;
}

[[nodiscard]] PrintJobState job_state(const QString& value) {
    if (value == QStringLiteral("pending")) return PrintJobState::pending;
    if (value == QStringLiteral("held")) return PrintJobState::held;
    if (value == QStringLiteral("processing")) return PrintJobState::processing;
    if (value == QStringLiteral("stopped")) return PrintJobState::stopped;
    if (value == QStringLiteral("canceled")) return PrintJobState::canceled;
    if (value == QStringLiteral("aborted")) return PrintJobState::aborted;
    if (value == QStringLiteral("completed")) return PrintJobState::completed;
    return PrintJobState::unknown;
}

[[nodiscard]] QJsonObject profile_json(const PrintProfile& profile) {
    return QJsonObject{
        {QStringLiteral("name"), QString::fromStdString(profile.name)},
        {QStringLiteral("media"), QString::fromStdString(profile.media)},
        {QStringLiteral("media_source"), QString::fromStdString(profile.media_source)},
        {QStringLiteral("media_type"), QString::fromStdString(profile.media_type)},
        {QStringLiteral("color_mode"), QString::fromStdString(profile.color_mode)},
        {QStringLiteral("sides"), QString::fromStdString(profile.sides)},
        {QStringLiteral("quality"), profile.quality},
        {QStringLiteral("copies"), profile.copies},
    };
}

} // namespace

DeviceServiceGateway::DeviceServiceGateway(std::shared_ptr<DeviceManager> fallback)
    : fallback_{std::move(fallback)} {
    if (!fallback_) {
        throw std::invalid_argument("DeviceServiceGateway requires a fallback DeviceManager");
    }
}

QJsonObject DeviceServiceGateway::call(
    const QString& method,
    const QJsonObject& params,
    const int timeout_ms) const {

    if (smoke_test_mode()) {
        throw std::runtime_error("device service disabled in GUI smoke-test mode");
    }

    QLocalSocket socket;
    socket.connectToServer(socket_path());
    if (!socket.waitForConnected(timeout_ms)) {
        throw std::runtime_error("device service unavailable");
    }

    static std::atomic<qulonglong> next_id{1};
    const qulonglong id = next_id.fetch_add(1, std::memory_order_relaxed);
    const QJsonObject request{
        {QStringLiteral("id"), static_cast<double>(id)},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };

    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(timeout_ms)) {
        throw std::runtime_error("device service request write failed");
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray response;
    constexpr qsizetype max_response = 1024 * 1024;
    while (!response.contains('\n')) {
        const int remaining = timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining)) {
            throw std::runtime_error("device service response timed out");
        }
        response += socket.readAll();
        if (response.size() > max_response) {
            throw std::runtime_error("device service response exceeds 1 MiB");
        }
    }

    const int newline = response.indexOf('\n');
    const QJsonDocument document = QJsonDocument::fromJson(response.left(newline));
    if (!document.isObject()) {
        throw std::runtime_error("device service returned invalid JSON");
    }
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("ok")).toBool(false)) {
        const QString message = object.value(QStringLiteral("error")).toString(
            QStringLiteral("device service request failed"));
        throw std::runtime_error(message.toStdString());
    }
    return object;
}

DeviceSnapshot DeviceServiceGateway::snapshot() const {
    if (smoke_test_mode()) {
        return {};
    }

    try {
        const auto response = call(QStringLiteral("device.list"));
        const auto result = response.value(QStringLiteral("result")).toObject();
        DeviceSnapshot snapshot;
        for (const auto& value : result.value(QStringLiteral("printers")).toArray()) {
            const auto item = value.toObject();
            snapshot.printers.push_back(PrinterInfo{
                .name = item.value(QStringLiteral("name")).toString().toStdString(),
                .model = item.value(QStringLiteral("model")).toString().toStdString(),
                .uri = item.value(QStringLiteral("uri")).toString().toStdString(),
                .is_default = item.value(QStringLiteral("default")).toBool(false),
            });
        }
        for (const auto& value : result.value(QStringLiteral("scanners")).toArray()) {
            const auto item = value.toObject();
            snapshot.scanners.push_back(ScannerInfo{
                .name = item.value(QStringLiteral("name")).toString().toStdString(),
                .vendor = item.value(QStringLiteral("vendor")).toString().toStdString(),
                .model = item.value(QStringLiteral("model")).toString().toStdString(),
                .type = item.value(QStringLiteral("type")).toString().toStdString(),
                .backend = item.value(QStringLiteral("backend")).toString().toStdString(),
            });
        }
        return snapshot;
    } catch (...) {
        return fallback_->snapshot();
    }
}

PrinterCapabilities DeviceServiceGateway::printer_capabilities(
    const std::string& printer,
    const bool force_refresh) const {

    try {
        const auto response = call(
            QStringLiteral("printer.capabilities"),
            QJsonObject{
                {QStringLiteral("printer"), QString::fromStdString(printer)},
                {QStringLiteral("refresh"), force_refresh},
            },
            12000);
        const auto result = response.value(QStringLiteral("result")).toObject();
        PrinterCapabilities caps;
        caps.printer = printer;
        caps.source = result.value(QStringLiteral("source")).toString().toStdString();
        caps.color_modes = string_vector(result.value(QStringLiteral("color_modes")));
        caps.sides = string_vector(result.value(QStringLiteral("sides")));
        caps.qualities = int_vector(result.value(QStringLiteral("qualities")));
        caps.resolutions_dpi = int_vector(result.value(QStringLiteral("resolutions_dpi")));
        caps.media_sources = string_vector(result.value(QStringLiteral("media_sources")));
        caps.media_types = string_vector(result.value(QStringLiteral("media_types")));
        caps.document_formats = string_vector(result.value(QStringLiteral("document_formats")));
        caps.media = string_vector(result.value(QStringLiteral("media")));
        caps.copies_min = result.value(QStringLiteral("copies_min")).toInt(1);
        caps.copies_max = result.value(QStringLiteral("copies_max")).toInt(1);
        return caps;
    } catch (...) {
        return fallback_->print_backend().capabilities(printer, force_refresh);
    }
}

PrinterStatus DeviceServiceGateway::printer_status(const std::string& printer) const {
    try {
        const auto response = call(
            QStringLiteral("printer.status"),
            QJsonObject{{QStringLiteral("printer"), QString::fromStdString(printer)}},
            8000);
        const auto result = response.value(QStringLiteral("result")).toObject();
        PrinterStatus status;
        status.printer = printer;
        status.source = result.value(QStringLiteral("source")).toString().toStdString();
        status.state = device_state(result.value(QStringLiteral("state")).toString());
        status.accepting_jobs = result.value(QStringLiteral("accepting_jobs")).toBool(false);
        status.reasons = string_vector(result.value(QStringLiteral("reasons")));
        for (const auto& value : result.value(QStringLiteral("supplies")).toArray()) {
            const auto item = value.toObject();
            SupplyStatus supply;
            supply.name = item.value(QStringLiteral("name")).toString().toStdString();
            supply.type = item.value(QStringLiteral("type")).toString().toStdString();
            if (item.contains(QStringLiteral("level_percent")) &&
                item.value(QStringLiteral("level_percent")).isDouble()) {
                supply.level_percent = item.value(QStringLiteral("level_percent")).toInt();
            }
            supply.low = item.value(QStringLiteral("low")).toBool(false);
            status.supplies.push_back(std::move(supply));
        }
        return status;
    } catch (...) {
        return fallback_->print_backend().status(printer);
    }
}

std::vector<PrintJobInfo> DeviceServiceGateway::jobs(
    const std::string& printer,
    const bool active_only) const {

    try {
        const auto response = call(
            QStringLiteral("printer.jobs"),
            QJsonObject{
                {QStringLiteral("printer"), QString::fromStdString(printer)},
                {QStringLiteral("active_only"), active_only},
            });
        const auto result = response.value(QStringLiteral("result")).toObject();
        std::vector<PrintJobInfo> jobs;
        for (const auto& value : result.value(QStringLiteral("jobs")).toArray()) {
            const auto item = value.toObject();
            jobs.push_back(PrintJobInfo{
                .id = item.value(QStringLiteral("id")).toInt(),
                .printer = printer,
                .title = item.value(QStringLiteral("title")).toString().toStdString(),
                .user = item.value(QStringLiteral("user")).toString().toStdString(),
                .format = item.value(QStringLiteral("format")).toString().toStdString(),
                .size_kib = item.value(QStringLiteral("size_kib")).toInt(),
                .state = job_state(item.value(QStringLiteral("state")).toString()),
            });
        }
        return jobs;
    } catch (...) {
        return JobManager{fallback_->print_backend()}.list_jobs(printer, active_only);
    }
}

ScannerCapabilities DeviceServiceGateway::scanner_capabilities(
    const std::string& scanner) const {

    try {
        const auto response = call(
            QStringLiteral("scanner.capabilities"),
            QJsonObject{{QStringLiteral("scanner"), QString::fromStdString(scanner)}},
            8000);
        const auto result = response.value(QStringLiteral("result")).toObject();
        ScannerCapabilities caps;
        caps.scanner = scanner;
        caps.source = result.value(QStringLiteral("source")).toString().toStdString();
        caps.modes = string_vector(result.value(QStringLiteral("modes")));
        caps.resolutions_dpi = int_vector(result.value(QStringLiteral("resolutions_dpi")));
        caps.sources = string_vector(result.value(QStringLiteral("sources")));
        caps.max_width_mm = result.value(QStringLiteral("max_width_mm")).toDouble();
        caps.max_height_mm = result.value(QStringLiteral("max_height_mm")).toDouble();
        return caps;
    } catch (...) {
        return fallback_->scanner_capabilities(scanner);
    }
}

PrintPreflightResult DeviceServiceGateway::preflight(
    const std::string& printer,
    const PrintProfile& profile,
    const bool detailed) const {

    try {
        const auto response = call(
            QStringLiteral("printer.preflight"),
            QJsonObject{
                {QStringLiteral("printer"), QString::fromStdString(printer)},
                {QStringLiteral("profile"), profile_json(profile)},
                {QStringLiteral("detailed"), detailed},
            },
            detailed ? 8000 : 3000);
        const auto result = response.value(QStringLiteral("result")).toObject();
        PrintPreflightResult check;
        check.ok = result.value(QStringLiteral("ok")).toBool(false);
        check.errors = string_vector(result.value(QStringLiteral("errors")));
        check.warnings = string_vector(result.value(QStringLiteral("warnings")));
        return check;
    } catch (...) {
        const auto caps = fallback_->print_backend().capabilities(printer, false);
        return validate_print_profile(profile, caps);
    }
}

bool DeviceServiceGateway::service_available() const noexcept {
    if (smoke_test_mode()) {
        return false;
    }
    try {
        (void)call(QStringLiteral("ping"), {}, 250);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace docsuite::desktop
