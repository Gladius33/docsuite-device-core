// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/scan/escl_scan_backend.hpp"

#include <cups/http.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef DOCSUITE_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace docsuite {
namespace {

struct ParsedHttpUri {
    std::string scheme;
    std::string host;
    int port{0};
    std::string resource{"/"};
};

class HttpGuard {
public:
    explicit HttpGuard(http_t* connection) : connection_{connection} {}
    ~HttpGuard() {
        if (connection_ != nullptr) {
            httpClose(connection_);
        }
    }
    HttpGuard(const HttpGuard&) = delete;
    HttpGuard& operator=(const HttpGuard&) = delete;
    [[nodiscard]] http_t* get() const noexcept { return connection_; }

private:
    http_t* connection_{nullptr};
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

[[nodiscard]] std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string first_word(const std::string& text) {
    const auto space = text.find(' ');
    return space == std::string::npos ? text : text.substr(0, space);
}

[[nodiscard]] bool parse_http_uri(const std::string& uri, ParsedHttpUri& parsed) {
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

    parsed.scheme = lower_copy(scheme.data());
    parsed.host = host.data();
    parsed.port = port > 0 ? port : (parsed.scheme == "https" || parsed.scheme == "ipps" ? 443 : 80);
    parsed.resource = resource.front() != '\0' ? resource.data() : "/";
    return true;
}

[[nodiscard]] bool parse_printer_uri(const std::string& uri, ParsedHttpUri& parsed) {
    if (uri.rfind("ipp://", 0) != 0 && uri.rfind("ipps://", 0) != 0) {
        return false;
    }
    return parse_http_uri(uri, parsed);
}

[[nodiscard]] bool parse_scanner_uri(const std::string& scanner_name, ParsedHttpUri& parsed) {
    std::string uri = scanner_name;
    if (uri.rfind("escl:", 0) == 0) {
        uri.erase(0, 5);
    }
    if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) {
        return false;
    }
    return parse_http_uri(uri, parsed);
}

[[nodiscard]] http_encryption_t encryption_for(const ParsedHttpUri& uri) {
    return uri.scheme == "https" || uri.scheme == "ipps"
        ? HTTP_ENCRYPTION_ALWAYS
        : HTTP_ENCRYPTION_NEVER;
}

[[nodiscard]] HttpGuard connect_http(const ParsedHttpUri& uri, const int timeout_ms = 5000) {
    http_t* connection = httpConnect2(
        uri.host.c_str(),
        uri.port,
        nullptr,
        AF_UNSPEC,
        encryption_for(uri),
        1,
        timeout_ms,
        nullptr);
    if (connection == nullptr) {
        throw std::runtime_error("Unable to connect to eSCL scanner at " + uri.host);
    }
    httpSetTimeout(connection, 30.0, nullptr, nullptr);
    return HttpGuard{connection};
}

[[nodiscard]] http_status_t response_status(http_t* connection) {
    http_status_t status = HTTP_STATUS_CONTINUE;
    while (status == HTTP_STATUS_CONTINUE) {
        status = httpUpdate(connection);
    }
    return status;
}

[[nodiscard]] std::vector<std::uint8_t> read_body(
    http_t* connection,
    const std::size_t max_size) {

    std::array<char, 64U * 1024U> buffer{};
    std::vector<std::uint8_t> result;
    while (result.size() < max_size) {
        const ssize_t bytes = httpRead2(connection, buffer.data(), buffer.size());
        if (bytes <= 0) {
            break;
        }
        const auto count = static_cast<std::size_t>(bytes);
        if (count > max_size - result.size()) {
            throw std::runtime_error("eSCL response exceeds configured safety limit");
        }
        result.insert(
            result.end(),
            reinterpret_cast<const std::uint8_t*>(buffer.data()),
            reinterpret_cast<const std::uint8_t*>(buffer.data()) + count);
    }
    return result;
}

[[nodiscard]] std::string fetch_escl_capabilities(const ParsedHttpUri& uri) {
    HttpGuard connection = connect_http(uri, 3000);
    httpClearFields(connection.get());
    if (httpGet(connection.get(), "/eSCL/ScannerCapabilities") != 0) {
        throw std::runtime_error("Unable to send eSCL ScannerCapabilities request");
    }
    const http_status_t status = response_status(connection.get());
    if (status != HTTP_STATUS_OK) {
        throw std::runtime_error(
            "eSCL ScannerCapabilities returned HTTP " + std::to_string(static_cast<int>(status)));
    }

    constexpr std::size_t max_capabilities_size = 1024U * 1024U;
    const auto bytes = read_body(connection.get(), max_capabilities_size);
    const std::string xml(bytes.begin(), bytes.end());
    if (xml.find("ScannerCapabilities") == std::string::npos) {
        throw std::runtime_error("eSCL ScannerCapabilities response is not recognized XML");
    }
    return xml;
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

[[nodiscard]] std::vector<std::string> xml_values(
    const std::string& xml,
    const std::string_view tag) {

    std::vector<std::string> result;
    const std::string open = "<" + std::string{tag} + ">";
    const std::string close = "</" + std::string{tag} + ">";
    std::size_t cursor = 0;
    while (true) {
        const auto begin = xml.find(open, cursor);
        if (begin == std::string::npos) {
            break;
        }
        const auto content_begin = begin + open.size();
        const auto end = xml.find(close, content_begin);
        if (end == std::string::npos) {
            break;
        }
        std::string value = trim(xml.substr(content_begin, end - content_begin));
        if (!value.empty() && std::find(result.begin(), result.end(), value) == result.end()) {
            result.push_back(std::move(value));
        }
        cursor = end + close.size();
    }
    return result;
}

[[nodiscard]] int positive_int(const std::string& value) {
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : 0;
    } catch (...) {
        return 0;
    }
}

[[nodiscard]] std::vector<int> resolutions_from_xml(const std::string& xml) {
    std::vector<int> result;
    for (const auto& value : xml_values(xml, "scan:XResolution")) {
        const int dpi = positive_int(value);
        if (dpi > 0 && std::find(result.begin(), result.end(), dpi) == result.end()) {
            result.push_back(dpi);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] double max_dimension_mm(
    const std::string& xml,
    const std::initializer_list<std::string_view> tags) {
    const int units = positive_int(xml_value(xml, tags));
    return units > 0 ? static_cast<double>(units) * 25.4 / 300.0 : 0.0;
}

[[nodiscard]] std::string eSCL_color_mode(const std::string& requested) {
    const std::string lower = lower_copy(requested);
    if (lower.find("gray") != std::string::npos || lower.find("grey") != std::string::npos ||
        lower.find("mono") != std::string::npos) {
        return "Grayscale8";
    }
    if (lower == "blackandwhite1" || lower == "bw") {
        return "BlackAndWhite1";
    }
    return "RGB24";
}

[[nodiscard]] std::string eSCL_input_source(const std::string& requested) {
    const std::string lower = lower_copy(requested);
    if (lower.find("adf") != std::string::npos || lower.find("feeder") != std::string::npos) {
        return "Feeder";
    }
    return "Platen";
}

[[nodiscard]] std::string scan_settings_xml(const ScanSettings& settings) {
    const std::string color = eSCL_color_mode(settings.mode);
    const std::string source = eSCL_input_source(settings.source);
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" "
        "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">"
        "<pwg:Version>2.0</pwg:Version>"
        "<scan:Intent>Document</scan:Intent>"
        "<pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>"
        "<scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt>"
        "<pwg:InputSource>" + source + "</pwg:InputSource>"
        "<scan:ColorMode>" + color + "</scan:ColorMode>"
        "<scan:XResolution>" + std::to_string(settings.dpi) + "</scan:XResolution>"
        "<scan:YResolution>" + std::to_string(settings.dpi) + "</scan:YResolution>"
        "</scan:ScanSettings>";
}

[[nodiscard]] std::string resource_from_location(const std::string& location) {
    if (location.empty()) {
        return {};
    }
    if (location.front() == '/') {
        return location;
    }
    ParsedHttpUri parsed;
    return parse_http_uri(location, parsed) ? parsed.resource : std::string{};
}

#ifdef DOCSUITE_HAVE_OPENCV
[[nodiscard]] ScanFrame decode_jpeg(
    const std::vector<std::uint8_t>& encoded,
    const int dpi) {

    if (encoded.empty()) {
        throw std::runtime_error("eSCL returned an empty document");
    }
    cv::Mat compressed(
        1,
        static_cast<int>(encoded.size()),
        CV_8UC1,
        const_cast<std::uint8_t*>(encoded.data()));
    cv::Mat image = cv::imdecode(compressed, cv::IMREAD_UNCHANGED);
    if (image.empty()) {
        throw std::runtime_error("Unable to decode eSCL JPEG document");
    }

    ScanFrame frame;
    frame.width = image.cols;
    frame.height = image.rows;
    frame.dpi = dpi;

    cv::Mat normalized;
    if (image.channels() == 1) {
        frame.format = ScanPixelFormat::gray8;
        normalized = image;
    } else if (image.channels() == 3) {
        frame.format = ScanPixelFormat::rgb24;
        cv::cvtColor(image, normalized, cv::COLOR_BGR2RGB);
    } else if (image.channels() == 4) {
        frame.format = ScanPixelFormat::rgb24;
        cv::cvtColor(image, normalized, cv::COLOR_BGRA2RGB);
    } else {
        throw std::runtime_error("Unsupported channel count in eSCL JPEG document");
    }

    if (!normalized.isContinuous()) {
        normalized = normalized.clone();
    }
    const std::size_t byte_count = normalized.total() * normalized.elemSize();
    frame.pixels.assign(normalized.data, normalized.data + byte_count);
    return frame;
}
#endif

} // namespace

std::vector<ScannerInfo> EsclScanBackend::probe_printers(
    const std::vector<PrinterInfo>& printers) const {

    std::vector<ScannerInfo> scanners;
    std::unordered_set<std::string> seen_hosts;

    for (const auto& printer : printers) {
        ParsedHttpUri printer_uri;
        if (!parse_printer_uri(printer.uri, printer_uri) ||
            !seen_hosts.insert(printer_uri.host).second) {
            continue;
        }

        ParsedHttpUri scanner_uri{
            .scheme = "http",
            .host = printer_uri.host,
            .port = 80,
            .resource = "/eSCL",
        };

        std::string capabilities_xml;
        try {
            capabilities_xml = fetch_escl_capabilities(scanner_uri);
        } catch (...) {
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
        } else if (capabilities_xml.find("ADF") != std::string::npos ||
                   capabilities_xml.find("Adf") != std::string::npos) {
            type = "ADF scanner";
        }

        scanners.push_back(ScannerInfo{
            .name = "escl:http://" + scanner_uri.host + ":80",
            .vendor = std::move(vendor),
            .model = std::move(model),
            .type = std::move(type),
            .backend = "eSCL-direct",
        });
    }

    return scanners;
}

bool EsclScanBackend::acquisition_available() const noexcept {
#ifdef DOCSUITE_HAVE_OPENCV
    return true;
#else
    return false;
#endif
}

ScannerCapabilities EsclScanBackend::capabilities(const std::string& scanner_name) const {
    ParsedHttpUri uri;
    if (!parse_scanner_uri(scanner_name, uri)) {
        throw std::runtime_error("Unsupported eSCL scanner URI: " + scanner_name);
    }

    const std::string xml = fetch_escl_capabilities(uri);
    ScannerCapabilities result;
    result.scanner = scanner_name;
    result.source = "escl-direct";

    const auto raw_modes = xml_values(xml, "scan:ColorMode");
    for (const auto& mode : raw_modes) {
        if (mode == "RGB24") {
            if (std::find(result.modes.begin(), result.modes.end(), "Color") == result.modes.end()) {
                result.modes.push_back("Color");
            }
        } else if (mode == "Grayscale8") {
            if (std::find(result.modes.begin(), result.modes.end(), "Gray") == result.modes.end()) {
                result.modes.push_back("Gray");
            }
        } else if (mode == "BlackAndWhite1") {
            if (std::find(result.modes.begin(), result.modes.end(), "Lineart") == result.modes.end()) {
                result.modes.push_back("Lineart");
            }
        }
    }

    result.resolutions_dpi = resolutions_from_xml(xml);
    if (xml.find("PlatenInputCaps") != std::string::npos || xml.find("Platen") != std::string::npos) {
        result.sources.push_back("Flatbed");
    }
    if (xml.find("AdfSimplexInputCaps") != std::string::npos || xml.find("ADF") != std::string::npos) {
        result.sources.push_back("ADF");
    }
    if (xml.find("AdfDuplexInputCaps") != std::string::npos) {
        result.sources.push_back("ADF Duplex");
    }

    result.max_width_mm = max_dimension_mm(xml, {"pwg:MaxWidth", "scan:MaxWidth", "MaxWidth"});
    result.max_height_mm = max_dimension_mm(xml, {"pwg:MaxHeight", "scan:MaxHeight", "MaxHeight"});
    return result;
}

ScanFrame EsclScanBackend::scan(
    const std::string& scanner_name,
    const ScanSettings& settings) const {

#ifndef DOCSUITE_HAVE_OPENCV
    (void)scanner_name;
    (void)settings;
    throw std::runtime_error("Direct eSCL acquisition requires an OpenCV-enabled DocSuite build");
#else
    ParsedHttpUri uri;
    if (!parse_scanner_uri(scanner_name, uri)) {
        throw std::runtime_error("Unsupported eSCL scanner URI: " + scanner_name);
    }
    if (settings.dpi <= 0) {
        throw std::runtime_error("eSCL scan resolution must be positive");
    }

    HttpGuard connection = connect_http(uri, 5000);
    const std::string xml = scan_settings_xml(settings);

    httpClearFields(connection.get());
    httpSetField(connection.get(), HTTP_FIELD_CONTENT_TYPE, "text/xml; charset=utf-8");
    httpSetLength(connection.get(), xml.size());
    if (httpPost(connection.get(), "/eSCL/ScanJobs") != 0) {
        throw std::runtime_error("Unable to send eSCL ScanJobs POST request");
    }
    const ssize_t written = httpWrite2(connection.get(), xml.data(), xml.size());
    if (written < 0 || static_cast<std::size_t>(written) != xml.size()) {
        throw std::runtime_error("Unable to send complete eSCL ScanSettings payload");
    }

    const http_status_t create_status = response_status(connection.get());
    if (create_status != HTTP_STATUS_CREATED && create_status != HTTP_STATUS_OK) {
        throw std::runtime_error(
            "eSCL ScanJobs returned HTTP " + std::to_string(static_cast<int>(create_status)));
    }

    const char* location_field = httpGetField(connection.get(), HTTP_FIELD_LOCATION);
    const std::string location = location_field != nullptr ? location_field : "";
    const std::string job_resource = resource_from_location(location);
    if (job_resource.empty()) {
        throw std::runtime_error("eSCL ScanJobs response did not provide a usable Location header");
    }

    // Drain any optional response body before reusing the HTTP connection.
    (void)read_body(connection.get(), 64U * 1024U);

    const std::string document_resource = job_resource + "/NextDocument";
    httpClearFields(connection.get());
    if (httpGet(connection.get(), document_resource.c_str()) != 0) {
        throw std::runtime_error("Unable to request eSCL NextDocument");
    }
    const http_status_t document_status = response_status(connection.get());
    if (document_status != HTTP_STATUS_OK) {
        throw std::runtime_error(
            "eSCL NextDocument returned HTTP " + std::to_string(static_cast<int>(document_status)));
    }

    constexpr std::size_t max_document_size = 256U * 1024U * 1024U;
    const auto document = read_body(connection.get(), max_document_size);
    return decode_jpeg(document, settings.dpi);
#endif
}

} // namespace docsuite
