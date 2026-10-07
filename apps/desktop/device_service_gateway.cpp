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
        throw std::runtime_error("DocSuite service disabled in GUI smoke-test mode");
    }

    static std::atomic<quint64> next_id{1};
    const quint64 request_id = next_id.fetch_add(1, std::memory_order_relaxed);

    QLocalSocket socket;
    socket.connectToServer(socket_path(), QIODevice::ReadWrite);
    if (!socket.waitForConnected(timeout_ms)) {
        throw std::runtime_error(
            "DocSuite service unavailable: " + socket.errorString().toStdString());
    }

    const QJsonObject request{
        {QStringLiteral("id"), static_cast<qint64>(request_id)},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
    QByteArray wire = QJsonDocument(request).toJson(QJsonDocument::Compact);
    wire.append('\n');
    if (socket.write(wire) != wire.size() || !socket.waitForBytesWritten(timeout_ms)) {
        throw std::runtime_error("Unable to write request to DocSuite service");
    }

    QByteArray response_bytes;
    QElapsedTimer timer;
    timer.start();
    while (response_bytes.indexOf('\n') < 0) {
        const int remaining = timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining)) {
            throw std::runtime_error("Timed out waiting for DocSuite service response");
        }
        response_bytes.append(socket.readAll());
        if (response_bytes.size() > 1024 * 1024) {
            throw std::runtime_error("DocSuite service response exceeded 1 MiB safety limit");
        }
    }

    response_bytes.truncate(response_bytes.indexOf('\n'));
    QJsonParseError parse_error{};
    const QJsonDocument response_document = QJsonDocument::fromJson(response_bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !response_document.isObject()) {
        throw std::runtime_error("Invalid JSON response from DocSuite service");
    }

    const QJsonObject response = response_document.object();
    if (!response.value(QStringLiteral("ok")).toBool(false)) {
        throw std::runtime_error(
            response.value(QStringLiteral("error"))
                .toString(QStringLiteral("DocSuite service request failed"))
                .toStdString());
    }
    if (!response.value(QStringLiteral("result")).isObject()) {
        throw std::runtime_error("DocSuite service response has no object result");
    }
    return response.value(QStringLiteral("result")).toObject();
}

bool DeviceServiceGateway::service_available() const noexcept {
    if (smoke_test_mode()) {
        return false;
    }
    try {
        const auto result = call(QStringLiteral("ping"), {}, 250);
        return result.value(QStringLiteral("service")).toString() ==
            QStringLiteral("docsuite-device-service");
    } catch (...) {
        return false;
    }
}

DeviceSnapshot DeviceServiceGateway::snapshot() const {
    if (smoke_test_mode()) {
        return {};
    }
    try {
        const QJsonObject result = call(QStringLiteral("device.list"));
        DeviceSnapshot snapshot;

        const QJsonArray printers = result.value(QStringLiteral("printers")).toArray();
        snapshot.printers.reserve(static_cast<std::size_t>(printers.size()));
        for (const auto& value : printers) {
            const QJsonObject item = value.toObject();
            snapshot.printers.push_back(PrinterInfo{
                .name = item.value(QStringLiteral("name")).toString().toStdString(),
                .uri = item.value(QStringLiteral("uri")).toString().toStdString(),
                .model = item.value(QStringLiteral("model")).toString().toStdString(),
                .is_default = item.value(QStringLiteral("default")).toBool(false),
                .temporary = false,
            });
        }

        const QJsonArray scanners = result.value(QStringLiteral("scanners")).toArray();
        snapshot.scanners.reserve(static_cast<std::size_t>(scanners.size()));
        for (const auto& value : scanners) {
            const QJsonObject item = value.toObject();
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
    const bool refresh) const {
    try {
        const QJsonObject result = call(
            QStringLiteral("printer.capabilities"),
            QJsonObject{
                {QStringLiteral("printer"), QString::fromStdString(printer)},
                {QStringLiteral("refresh"), refresh},
            });
        PrinterCapabilities caps;
        caps.printer = result.value(QStringLiteral("printer")).toString().toStdString();
        caps.source = result.value(QStringLiteral("source")).toString().toStdString();
        caps.color_modes = string_vector(result.value(QStringLiteral("color_modes")));
        caps.media = string_vector(result.value(QStringLiteral("media")));
        caps.media_types = string_vector(result.value(QStringLiteral("media_types")));
        caps.media_sources = string_vector(result.value(QStringLiteral("media_sources")));
        caps.sides = string_vector(result.value(QStringLiteral("sides")));
        caps.qualities = int_vector(result.value(QStringLiteral("qualities")));
        caps.resolutions_dpi = int_vector(result.value(QStringLiteral("resolutions_dpi")));
        caps.document_formats = string_vector(result.value(QStringLiteral("document_formats")));
        caps.copies_min = result.value(QStringLiteral("copies_min")).toInt(1);
        caps.copies_max = result.value(QStringLiteral("copies_max")).toInt(1);
        caps.fetched_at = std::chrono::system_clock::now();
        return caps;
    } catch (...) {
        return fallback_->print_backend().capabilities(printer, refresh);
    }
}

PrinterStatus DeviceServiceGateway::printer_status(const std::string& printer) const {
    try {
        const QJsonObject result = call(
            QStringLiteral("printer.status"),
            QJsonObject{{QStringLiteral("printer"), QString::fromStdString(printer)}});
        PrinterStatus status;
        status.printer = result.value(QStringLiteral("printer")).toString().toStdString();
        status.source = result.value(QStringLiteral("source")).toString().toStdString();
        status.state = device_state(result.value(QStringLiteral("state")).toString());
        status.accepting_jobs = result.value(QStringLiteral("accepting_jobs")).toBool(false);
        status.reasons = string_vector(result.value(QStringLiteral("reasons")));
        const QJsonArray supplies = result.value(QStringLiteral("supplies")).toArray();
        status.supplies.reserve(static_cast<std::size_t>(supplies.size()));
        for (const auto& value : supplies) {
            const QJsonObject item = value.toObject();
            SupplyLevel supply;
            supply.name = item.value(QStringLiteral("name")).toString().toStdString();
            supply.type = item.value(QStringLiteral("type")).toString().toStdString();
            supply.low_threshold = item.value(QStringLiteral("low_threshold")).toInt(15);
            const QJsonValue percent = item.value(QStringLiteral("percent"));
            if (percent.isDouble()) {
                supply.percent = percent.toInt();
            }
            status.supplies.push_back(std::move(supply));
        }
        return status;
    } catch (...) {
        return fallback_->print_backend().status(printer);
    }
}

ScannerCapabilities DeviceServiceGateway::scanner_capabilities(const std::string& scanner) const {
    try {
        const QJsonObject result = call(
            QStringLiteral("scanner.capabilities"),
            QJsonObject{{QStringLiteral("scanner"), QString::fromStdString(scanner)}});
        ScannerCapabilities caps;
        caps.scanner = result.value(QStringLiteral("scanner")).toString().toStdString();
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

std::vector<PrintJobInfo> DeviceServiceGateway::printer_jobs(
    const std::string& printer,
    const bool include_completed) const {
    try {
        const QJsonObject result = call(
            QStringLiteral("printer.jobs"),
            QJsonObject{
                {QStringLiteral("printer"), QString::fromStdString(printer)},
                {QStringLiteral("include_completed"), include_completed},
            });
        const QJsonArray jobs = result.value(QStringLiteral("jobs")).toArray();
        std::vector<PrintJobInfo> output;
        output.reserve(static_cast<std::size_t>(jobs.size()));
        for (const auto& value : jobs) {
            const QJsonObject item = value.toObject();
            output.push_back(PrintJobInfo{
                .id = item.value(QStringLiteral("id")).toInt(),
                .printer = item.value(QStringLiteral("printer")).toString().toStdString(),
                .title = item.value(QStringLiteral("title")).toString().toStdString(),
                .user = item.value(QStringLiteral("user")).toString().toStdString(),
                .format = item.value(QStringLiteral("format")).toString().toStdString(),
                .state = job_state(item.value(QStringLiteral("state")).toString()),
                .size_kib = item.value(QStringLiteral("size_kib")).toInt(),
                .priority = item.value(QStringLiteral("priority")).toInt(),
            });
        }
        return output;
    } catch (...) {
        return fallback_->job_manager().list_jobs(printer, include_completed);
    }
}

PrintPreflightResult DeviceServiceGateway::printer_preflight(
    const std::string& printer,
    const PrintProfile& profile,
    const bool detailed,
    const bool refresh) const {
    try {
        QJsonObject params = profile_json(profile);
        params.insert(QStringLiteral("printer"), QString::fromStdString(printer));
        params.insert(QStringLiteral("detailed"), detailed);
        params.insert(QStringLiteral("refresh"), refresh);
        const QJsonObject result = call(QStringLiteral("printer.preflight"), params);
        return PrintPreflightResult{
            .ok = result.value(QStringLiteral("ok")).toBool(false),
            .errors = string_vector(result.value(QStringLiteral("errors"))),
            .warnings = string_vector(result.value(QStringLiteral("warnings"))),
        };
    } catch (...) {
        return detailed
            ? fallback_->print_backend().preflight_detailed(printer, profile, refresh)
            : fallback_->print_backend().preflight(printer, profile, refresh);
    }
}

} // namespace docsuite::desktop
