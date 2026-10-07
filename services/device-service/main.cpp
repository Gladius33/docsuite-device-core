// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/device/device_manager.hpp"
#include "docsuite/print/job_manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QUuid>

#include <pwd.h>
#include <unistd.h>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr qsizetype kMaxRpcRequestBytes = 64 * 1024;

[[nodiscard]] QString runtime_directory() {
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }
    if (runtime.isEmpty()) {
        runtime = QDir::tempPath();
    }
    return runtime;
}

[[nodiscard]] QString socket_path() {
    return QDir(runtime_directory()).filePath(
        QStringLiteral("docsuite-device-core.sock"));
}

[[nodiscard]] QString scan_output_directory() {
    const QString root = QDir(runtime_directory()).filePath(
        QStringLiteral("docsuite-device-core/scans"));
    if (!QDir{}.mkpath(root)) {
        throw std::runtime_error(
            "Unable to create private DocSuite scan output directory");
    }
    if (!QFile::setPermissions(
            root,
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        throw std::runtime_error(
            "Unable to secure private DocSuite scan output directory");
    }
    return root;
}

[[nodiscard]] std::string current_username() {
    if (const passwd* entry = getpwuid(geteuid());
        entry != nullptr && entry->pw_name != nullptr) {
        return entry->pw_name;
    }
    return {};
}

[[nodiscard]] bool terminal_job(const docsuite::PrintJobState state) noexcept {
    return state == docsuite::PrintJobState::completed ||
        state == docsuite::PrintJobState::canceled ||
        state == docsuite::PrintJobState::aborted ||
        state == docsuite::PrintJobState::stopped;
}

[[nodiscard]] QJsonArray strings(const std::vector<std::string>& values) {
    QJsonArray result;
    for (const auto& value : values) {
        result.append(QString::fromStdString(value));
    }
    return result;
}

[[nodiscard]] QJsonArray integers(const std::vector<int>& values) {
    QJsonArray result;
    for (const int value : values) {
        result.append(value);
    }
    return result;
}

[[nodiscard]] QJsonObject supply_json(const docsuite::SupplyLevel& supply) {
    QJsonObject result{
        {QStringLiteral("name"), QString::fromStdString(supply.name)},
        {QStringLiteral("type"), QString::fromStdString(supply.type)},
        {QStringLiteral("low_threshold"), supply.low_threshold},
    };
    result.insert(
        QStringLiteral("percent"),
        supply.percent.has_value() ? QJsonValue{*supply.percent} : QJsonValue::Null);
    return result;
}

[[nodiscard]] QJsonObject printer_status_json(const docsuite::PrinterStatus& status) {
    QJsonArray supplies;
    for (const auto& supply : status.supplies) {
        supplies.append(supply_json(supply));
    }

    QString state = QStringLiteral("unknown");
    switch (status.state) {
        case docsuite::DeviceState::idle: state = QStringLiteral("idle"); break;
        case docsuite::DeviceState::processing: state = QStringLiteral("processing"); break;
        case docsuite::DeviceState::stopped: state = QStringLiteral("stopped"); break;
        case docsuite::DeviceState::offline: state = QStringLiteral("offline"); break;
        case docsuite::DeviceState::unknown: break;
    }

    return QJsonObject{
        {QStringLiteral("printer"), QString::fromStdString(status.printer)},
        {QStringLiteral("source"), QString::fromStdString(status.source)},
        {QStringLiteral("state"), state},
        {QStringLiteral("accepting_jobs"), status.accepting_jobs},
        {QStringLiteral("reasons"), strings(status.reasons)},
        {QStringLiteral("supplies"), supplies},
    };
}

[[nodiscard]] QJsonObject capabilities_json(const docsuite::PrinterCapabilities& caps) {
    return QJsonObject{
        {QStringLiteral("printer"), QString::fromStdString(caps.printer)},
        {QStringLiteral("source"), QString::fromStdString(caps.source)},
        {QStringLiteral("color_modes"), strings(caps.color_modes)},
        {QStringLiteral("media"), strings(caps.media)},
        {QStringLiteral("media_types"), strings(caps.media_types)},
        {QStringLiteral("media_sources"), strings(caps.media_sources)},
        {QStringLiteral("sides"), strings(caps.sides)},
        {QStringLiteral("qualities"), integers(caps.qualities)},
        {QStringLiteral("resolutions_dpi"), integers(caps.resolutions_dpi)},
        {QStringLiteral("document_formats"), strings(caps.document_formats)},
        {QStringLiteral("copies_min"), caps.copies_min},
        {QStringLiteral("copies_max"), caps.copies_max},
    };
}

[[nodiscard]] QJsonObject preflight_json(const docsuite::PrintPreflightResult& result) {
    return QJsonObject{
        {QStringLiteral("ok"), result.ok},
        {QStringLiteral("errors"), strings(result.errors)},
        {QStringLiteral("warnings"), strings(result.warnings)},
    };
}

[[nodiscard]] QJsonObject scanner_capabilities_json(
    const docsuite::ScannerCapabilities& caps) {

    return QJsonObject{
        {QStringLiteral("scanner"), QString::fromStdString(caps.scanner)},
        {QStringLiteral("source"), QString::fromStdString(caps.source)},
        {QStringLiteral("modes"), strings(caps.modes)},
        {QStringLiteral("resolutions_dpi"), integers(caps.resolutions_dpi)},
        {QStringLiteral("sources"), strings(caps.sources)},
        {QStringLiteral("max_width_mm"), caps.max_width_mm},
        {QStringLiteral("max_height_mm"), caps.max_height_mm},
    };
}

[[nodiscard]] QJsonObject job_json(const docsuite::PrintJobInfo& job) {
    return QJsonObject{
        {QStringLiteral("id"), job.id},
        {QStringLiteral("printer"), QString::fromStdString(job.printer)},
        {QStringLiteral("title"), QString::fromStdString(job.title)},
        {QStringLiteral("user"), QString::fromStdString(job.user)},
        {QStringLiteral("format"), QString::fromStdString(job.format)},
        {QStringLiteral("state"), QString::fromLatin1(docsuite::print_job_state_name(job.state))},
        {QStringLiteral("size_kib"), job.size_kib},
        {QStringLiteral("priority"), job.priority},
    };
}

[[nodiscard]] std::string optional_keyword(
    const QJsonObject& params,
    const QString& key,
    const std::string& fallback,
    const bool empty_allowed = false) {

    if (!params.contains(key)) {
        return fallback;
    }
    const QJsonValue raw = params.value(key);
    if (!raw.isString()) {
        throw std::runtime_error(
            "parameter must be a string: " + key.toStdString());
    }
    const QString value = raw.toString();
    if ((!empty_allowed && value.isEmpty()) || value.size() > 256) {
        throw std::runtime_error(
            "invalid string parameter: " + key.toStdString());
    }
    return value.toStdString();
}

[[nodiscard]] int optional_integer(
    const QJsonObject& params,
    const QString& key,
    const int fallback,
    const int minimum,
    const int maximum) {

    if (!params.contains(key)) {
        return fallback;
    }
    const QJsonValue raw = params.value(key);
    if (!raw.isDouble()) {
        throw std::runtime_error(
            "parameter must be an integer: " + key.toStdString());
    }
    const double numeric = raw.toDouble();
    const int value = raw.toInt(minimum - 1);
    if (numeric != static_cast<double>(value) || value < minimum || value > maximum) {
        throw std::runtime_error(
            "integer parameter is outside accepted bounds: " + key.toStdString());
    }
    return value;
}

[[nodiscard]] docsuite::PrintProfile profile_from_params(const QJsonObject& params) {
    docsuite::PrintProfile profile;
    profile.name = optional_keyword(params, QStringLiteral("name"), "RPC profile", true);
    profile.media = optional_keyword(params, QStringLiteral("media"), profile.media);
    profile.media_source = optional_keyword(
        params,
        QStringLiteral("media_source"),
        profile.media_source,
        true);
    profile.media_type = optional_keyword(
        params,
        QStringLiteral("media_type"),
        profile.media_type,
        true);
    profile.color_mode = optional_keyword(
        params,
        QStringLiteral("color_mode"),
        profile.color_mode);
    profile.sides = optional_keyword(params, QStringLiteral("sides"), profile.sides);
    profile.quality = optional_integer(
        params,
        QStringLiteral("quality"),
        profile.quality,
        0,
        100);
    profile.copies = optional_integer(
        params,
        QStringLiteral("copies"),
        profile.copies,
        1,
        9999);
    return profile;
}

[[nodiscard]] QByteArray rpc_error(
    const QJsonValue& id,
    const QString& error) {
    QJsonObject object{
        {QStringLiteral("id"), id},
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), error},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

class Service final : public QObject {
public:
    explicit Service(QObject* parent = nullptr)
        : QObject{parent}, manager_{std::make_shared<docsuite::DeviceManager>()} {

        server_.setSocketOptions(QLocalServer::UserAccessOption);
        const QString path = socket_path();
        QLocalServer::removeServer(path);
        if (!server_.listen(path)) {
            throw std::runtime_error(
                "Unable to listen on local DocSuite socket: " +
                server_.errorString().toStdString());
        }

        QObject::connect(&server_, &QLocalServer::newConnection, this, [this]() {
            while (QLocalSocket* socket = server_.nextPendingConnection()) {
                socket->setParent(this);
                QObject::connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                    auto& buffer = buffers_[socket];
                    buffer.append(socket->readAll());
                    if (buffer.size() > kMaxRpcRequestBytes) {
                        socket->write(rpc_error(
                            QJsonValue::Null,
                            QStringLiteral("request exceeds 64 KiB limit")));
                        socket->write("\n");
                        socket->flush();
                        buffers_.remove(socket);
                        socket->disconnectFromServer();
                        return;
                    }

                    while (true) {
                        const qsizetype newline = buffer.indexOf('\n');
                        if (newline < 0) {
                            break;
                        }
                        const QByteArray line = buffer.left(newline).trimmed();
                        buffer.remove(0, newline + 1);
                        if (!line.isEmpty()) {
                            socket->write(dispatch(line));
                            socket->write("\n");
                            socket->flush();
                        }
                    }
                });
                QObject::connect(
                    socket,
                    &QLocalSocket::disconnected,
                    this,
                    [this, socket]() {
                        buffers_.remove(socket);
                        socket->deleteLater();
                    });
            }
        });
    }

private:
    [[nodiscard]] QByteArray dispatch(const QByteArray& line) {
        QJsonParseError parse_error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            return response(
                QJsonValue::Null,
                false,
                {},
                QStringLiteral("invalid JSON request"));
        }

        const QJsonObject request = document.object();
        const QJsonValue id = request.value(QStringLiteral("id"));
        const QString method = request.value(QStringLiteral("method")).toString();
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();

        try {
            if (method == QStringLiteral("ping")) {
                return response(
                    id,
                    true,
                    QJsonObject{
                        {QStringLiteral("service"), QStringLiteral("docsuite-device-service")},
                        {QStringLiteral("version"), QStringLiteral("0.5.0")},
                    },
                    {});
            }

            if (method == QStringLiteral("device.list")) {
                const auto snapshot = manager_->snapshot();
                QJsonArray printers;
                for (const auto& printer : snapshot.printers) {
                    printers.append(QJsonObject{
                        {QStringLiteral("name"), QString::fromStdString(printer.name)},
                        {QStringLiteral("uri"), QString::fromStdString(printer.uri)},
                        {QStringLiteral("model"), QString::fromStdString(printer.model)},
                        {QStringLiteral("default"), printer.is_default},
                    });
                }
                QJsonArray scanners;
                for (const auto& scanner : snapshot.scanners) {
                    scanners.append(QJsonObject{
                        {QStringLiteral("name"), QString::fromStdString(scanner.name)},
                        {QStringLiteral("vendor"), QString::fromStdString(scanner.vendor)},
                        {QStringLiteral("model"), QString::fromStdString(scanner.model)},
                        {QStringLiteral("type"), QString::fromStdString(scanner.type)},
                        {QStringLiteral("backend"), QString::fromStdString(scanner.backend)},
                    });
                }
                return response(
                    id,
                    true,
                    QJsonObject{
                        {QStringLiteral("printers"), printers},
                        {QStringLiteral("scanners"), scanners},
                    },
                    {});
            }

            if (method == QStringLiteral("printer.status")) {
                const std::string printer = required_string(params, QStringLiteral("printer"));
                return response(
                    id,
                    true,
                    printer_status_json(manager_->print_backend().status(printer)),
                    {});
            }

            if (method == QStringLiteral("printer.capabilities")) {
                const std::string printer = required_string(params, QStringLiteral("printer"));
                const bool refresh = params.value(QStringLiteral("refresh")).toBool(false);
                return response(
                    id,
                    true,
                    capabilities_json(
                        manager_->print_backend().capabilities(printer, refresh)),
                    {});
            }

            if (method == QStringLiteral("printer.preflight")) {
                const std::string printer = required_string(params, QStringLiteral("printer"));
                const docsuite::PrintProfile profile = profile_from_params(params);
                const bool detailed = params.value(QStringLiteral("detailed")).toBool(false);
                const bool refresh = params.value(QStringLiteral("refresh")).toBool(false);
                const auto result = detailed
                    ? manager_->print_backend().preflight_detailed(printer, profile, refresh)
                    : manager_->print_backend().preflight(printer, profile, refresh);
                return response(id, true, preflight_json(result), {});
            }

            if (method == QStringLiteral("printer.jobs")) {
                const std::string printer = required_string(params, QStringLiteral("printer"));
                const bool completed = params
                    .value(QStringLiteral("include_completed"))
                    .toBool(true);
                QJsonArray jobs;
                for (const auto& job : manager_->job_manager().list_jobs(printer, completed)) {
                    jobs.append(job_json(job));
                }
                return response(
                    id,
                    true,
                    QJsonObject{{QStringLiteral("jobs"), jobs}},
                    {});
            }

            if (method == QStringLiteral("printer.cancel_job")) {
                const std::string printer = required_string(params, QStringLiteral("printer"));
                const int job_id = required_positive_int(params, QStringLiteral("job_id"));
                const auto job = manager_->job_manager().job(printer, job_id);
                if (!job.has_value()) {
                    throw std::runtime_error("print job not found");
                }

                const std::string user = current_username();
                if (user.empty() || job->user.empty() || job->user != user) {
                    throw std::runtime_error(
                        "refusing to cancel a print job not owned by the local user");
                }
                if (terminal_job(job->state)) {
                    throw std::runtime_error("print job is already in a terminal state");
                }
                if (!manager_->job_manager().cancel(printer, job_id)) {
                    throw std::runtime_error("CUPS refused print job cancellation");
                }

                return response(
                    id,
                    true,
                    QJsonObject{
                        {QStringLiteral("job_id"), job_id},
                        {QStringLiteral("printer"), QString::fromStdString(printer)},
                        {QStringLiteral("canceled"), true},
                    },
                    {});
            }

            if (method == QStringLiteral("scanner.capabilities")) {
                const std::string scanner = required_string(params, QStringLiteral("scanner"));
                return response(
                    id,
                    true,
                    scanner_capabilities_json(
                        manager_->scan_backend().capabilities(scanner)),
                    {});
            }

            if (method == QStringLiteral("scanner.scan")) {
                const std::string scanner = required_string(params, QStringLiteral("scanner"));

                docsuite::ScanSettings settings;
                settings.dpi = optional_integer(
                    params,
                    QStringLiteral("dpi"),
                    settings.dpi,
                    75,
                    1200);

                const QString mode = params
                    .value(QStringLiteral("mode"))
                    .toString(QStringLiteral("Color"));
                if (mode != QStringLiteral("Color") &&
                    mode != QStringLiteral("Gray") &&
                    mode != QStringLiteral("Lineart")) {
                    throw std::runtime_error(
                        "scanner.scan mode must be Color, Gray, or Lineart");
                }
                settings.mode = mode.toStdString();

                const QString source = params
                    .value(QStringLiteral("source"))
                    .toString(QStringLiteral("Flatbed"));
                if (source.isEmpty() || source.size() > 128) {
                    throw std::runtime_error(
                        "scanner.scan source must be a non-empty scanner source name");
                }
                settings.source = source.toStdString();

                const auto frame = manager_->scan_backend().scan(scanner, settings);
                const QString output = QDir(scan_output_directory()).filePath(
                    QStringLiteral("scan-%1.pnm").arg(
                        QUuid::createUuid().toString(QUuid::WithoutBraces)));
                manager_->scan_backend().save_pnm(frame, output.toStdString());
                if (!QFile::setPermissions(
                        output,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                    QFile::remove(output);
                    throw std::runtime_error("Unable to secure scanner output file");
                }

                return response(
                    id,
                    true,
                    QJsonObject{
                        {QStringLiteral("path"), output},
                        {QStringLiteral("width"), frame.width},
                        {QStringLiteral("height"), frame.height},
                        {QStringLiteral("dpi"), frame.dpi},
                        {QStringLiteral("format"),
                         frame.format == docsuite::ScanPixelFormat::rgb24
                             ? QStringLiteral("RGB24")
                             : QStringLiteral("Gray8")},
                        {QStringLiteral("bytes"), static_cast<qint64>(frame.pixels.size())},
                    },
                    {});
            }

            return response(id, false, {}, QStringLiteral("unknown method"));
        } catch (const std::exception& error) {
            return response(id, false, {}, QString::fromUtf8(error.what()));
        }
    }

    [[nodiscard]] static std::string required_string(
        const QJsonObject& params,
        const QString& key) {

        const QJsonValue raw = params.value(key);
        if (!raw.isString()) {
            throw std::runtime_error(
                "missing required string parameter: " + key.toStdString());
        }
        const QString value = raw.toString();
        if (value.isEmpty() || value.size() > 1024) {
            throw std::runtime_error(
                "invalid required string parameter: " + key.toStdString());
        }
        return value.toStdString();
    }

    [[nodiscard]] static int required_positive_int(
        const QJsonObject& params,
        const QString& key) {

        const QJsonValue raw = params.value(key);
        if (!raw.isDouble()) {
            throw std::runtime_error(
                "missing integer parameter: " + key.toStdString());
        }
        const double numeric = raw.toDouble();
        const int result = raw.toInt(0);
        if (numeric != static_cast<double>(result) || result <= 0) {
            throw std::runtime_error(
                "parameter must be a positive integer: " + key.toStdString());
        }
        return result;
    }

    [[nodiscard]] static QByteArray response(
        const QJsonValue& id,
        const bool ok,
        const QJsonObject& result,
        const QString& error) {

        QJsonObject object{
            {QStringLiteral("id"), id},
            {QStringLiteral("ok"), ok},
        };
        if (ok) {
            object.insert(QStringLiteral("result"), result);
        } else {
            object.insert(QStringLiteral("error"), error);
        }
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    QLocalServer server_{};
    QHash<QLocalSocket*, QByteArray> buffers_{};
    std::shared_ptr<docsuite::DeviceManager> manager_;
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("docsuite-device-service"));
    try {
        Service service;
        return app.exec();
    } catch (const std::exception& error) {
        qCritical("%s", error.what());
        return 1;
    }
}
