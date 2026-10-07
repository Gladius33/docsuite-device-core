// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/scan/escl_scan_backend.hpp"

#include <cups/http.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unordered_set>

namespace docsuite {
namespace {

struct ParsedPrinterUri {
    std::string host;
};

[[nodiscard]] std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::string first_word(const std::string& text) {
    const auto space = text.find(' ');
    return space == std::string::npos ? text : text.substr(0, space);
}

[[nodiscard]] std::string xml_value(
    const std::string& xml,
    const std::initializer_list<std::string_view> tags) {

    for (const auto tag : tags) {
        const std::string open = "<" + std::string{tag} + ">";
        const std::string close = "</" + std::string{tag} + ">";
        const auto begin = xml.find(open);
        if (begin == std::string::npos) {
            continue;
        }
        const auto content_begin = begin + open.size();
        const auto end = xml.find(close, content_begin);
        if (end != std::string::npos) {
            return trim(xml.substr(content_begin, end - content_begin));
        }
    }
    return {};
}

[[nodiscard]] bool parse_printer_uri(const std::string& uri, ParsedPrinterUri& parsed) {
    if (uri.rfind("ipp://", 0) != 0 && uri.rfind("ipps://", 0) != 0) {
        return false;
    }

    std::array<char, 32> scheme{};
    std::array<char, 256> username{};
    std::array<char, 256> host{};
    std::array<char, 1024> resource{};
    int port = 0;

    const auto status = httpSeparateURI(
        HTTP_URI_CODING_ALL,
        uri.c_str(),
        scheme.data(), static_cast<int>(scheme.size()),
        username.data(), static_cast<int>(username.size()),
        host.data(), static_cast<int>(host.size()),
        &port,
        resource.data(), static_cast<int>(resource.size()));

    if (status != HTTP_URI_STATUS_OK || host.front() == '\0') {
        return false;
    }

    parsed.host = host.data();
    return true;
}

[[nodiscard]] bool fetch_escl_capabilities(const std::string& host, std::string& xml) {
    http_t* connection = httpConnect2(
        host.c_str(),
        80,
        nullptr,
        AF_UNSPEC,
        HTTP_ENCRYPTION_NEVER,
        1,
        1500,
        nullptr);

    if (connection == nullptr) {
        return false;
    }

    httpClearFields(connection);
    httpSetField(connection, HTTP_FIELD_ACCEPT, "application/xml,text/xml,*/*");

    if (httpGet(connection, "/eSCL/ScannerCapabilities") != 0) {
        httpClose(connection);
        return false;
    }

    http_status_t status = HTTP_STATUS_CONTINUE;
    while (status == HTTP_STATUS_CONTINUE) {
        status = httpUpdate(connection);
    }

    if (status != HTTP_STATUS_OK) {
        httpClose(connection);
        return false;
    }

    constexpr std::size_t max_capabilities_size = 1024U * 1024U;
    std::array<char, 8192> buffer{};
    xml.clear();

    while (xml.size() < max_capabilities_size) {
        const ssize_t bytes = httpRead2(connection, buffer.data(), buffer.size());
        if (bytes <= 0) {
            break;
        }
        xml.append(buffer.data(), static_cast<std::size_t>(bytes));
    }

    httpClose(connection);
    return xml.find("ScannerCapabilities") != std::string::npos;
}

} // namespace

std::vector<ScannerInfo> EsclScanBackend::probe_printers(
    const std::vector<PrinterInfo>& printers) const {

    std::vector<ScannerInfo> scanners;
    std::unordered_set<std::string> seen_hosts;

    for (const auto& printer : printers) {
        ParsedPrinterUri parsed;
        if (!parse_printer_uri(printer.uri, parsed) || !seen_hosts.insert(parsed.host).second) {
            continue;
        }

        std::string capabilities_xml;
        if (!fetch_escl_capabilities(parsed.host, capabilities_xml)) {
            continue;
        }

        std::string model = xml_value(capabilities_xml, {
            "pwg:MakeAndModel",
            "scan:MakeAndModel",
            "MakeAndModel",
        });
        if (model.empty()) {
            model = printer.model;
        }

        std::string vendor = xml_value(capabilities_xml, {
            "pwg:Manufacturer",
            "scan:Manufacturer",
            "Manufacturer",
        });
        if (vendor.empty()) {
            vendor = first_word(model);
        }

        std::string type = "scanner";
        if (capabilities_xml.find("Platen") != std::string::npos) {
            type = "platen scanner";
        } else if (capabilities_xml.find("ADF") != std::string::npos) {
            type = "ADF scanner";
        }

        scanners.push_back(ScannerInfo{
            .name = "escl:http://" + parsed.host + ":80",
            .vendor = std::move(vendor),
            .model = std::move(model),
            .type = std::move(type),
            .backend = "eSCL-direct",
        });
    }

    return scanners;
}

} // namespace docsuite
