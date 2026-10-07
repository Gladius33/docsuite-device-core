// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/direct_ipp_probe.hpp"

#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace docsuite {
namespace {

struct IppEndpoint {
    std::string host;
    std::string resource;
    int port{631};
    http_encryption_t encryption{HTTP_ENCRYPTION_NEVER};
};

[[nodiscard]] IppEndpoint parse_endpoint(const std::string& uri) {
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

    if ((status != HTTP_URI_STATUS_OK && status != HTTP_URI_STATUS_MISSING_RESOURCE) ||
        host.front() == '\0') {
        throw std::runtime_error("Invalid direct IPP URI: " + uri);
    }

    const std::string parsed_scheme{scheme.data()};
    if (parsed_scheme != "ipp" && parsed_scheme != "ipps") {
        throw std::runtime_error("Direct IPP probe only accepts ipp:// or ipps:// URIs");
    }

    IppEndpoint endpoint;
    endpoint.host = host.data();
    endpoint.resource = resource.front() != '\0' ? resource.data() : "/";
    endpoint.port = port > 0 ? port : 631;
    endpoint.encryption = parsed_scheme == "ipps"
        ? HTTP_ENCRYPTION_ALWAYS
        : HTTP_ENCRYPTION_NEVER;
    return endpoint;
}

[[nodiscard]] ipp_t* query(
    const std::string& uri,
    const char* const* requested_attributes,
    const int requested_attribute_count) {

    const IppEndpoint endpoint = parse_endpoint(uri);
    http_t* connection = httpConnect2(
        endpoint.host.c_str(),
        endpoint.port,
        nullptr,
        AF_UNSPEC,
        endpoint.encryption,
        1,
        3000,
        nullptr);
    if (connection == nullptr) {
        throw std::runtime_error("Unable to connect to direct IPP endpoint: " + uri);
    }

    ipp_t* request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    if (request == nullptr) {
        httpClose(connection);
        throw std::runtime_error("Unable to allocate IPP request");
    }

    ippAddString(
        request,
        IPP_TAG_OPERATION,
        IPP_TAG_URI,
        "printer-uri",
        nullptr,
        uri.c_str());
    ippAddStrings(
        request,
        IPP_TAG_OPERATION,
        IPP_TAG_KEYWORD,
        "requested-attributes",
        requested_attribute_count,
        nullptr,
        requested_attributes);

    ipp_t* response = cupsDoRequest(connection, request, endpoint.resource.c_str());
    httpClose(connection);

    if (response == nullptr) {
        throw std::runtime_error(
            std::string{"Direct IPP request failed: "} + cupsLastErrorString());
    }

    const ipp_status_t status = ippGetStatusCode(response);
    if (status >= IPP_STATUS_ERROR_BAD_REQUEST) {
        ippDelete(response);
        throw std::runtime_error(
            std::string{"Direct IPP endpoint rejected request: "} + cupsLastErrorString());
    }

    return response;
}

[[nodiscard]] std::vector<std::string> strings(ipp_attribute_t* attribute) {
    std::vector<std::string> result;
    if (attribute == nullptr) {
        return result;
    }

    const int count = ippGetCount(attribute);
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        if (const char* value = ippGetString(attribute, index, nullptr)) {
            result.emplace_back(value);
        }
    }
    return result;
}

[[nodiscard]] std::vector<int> integers(ipp_attribute_t* attribute) {
    std::vector<int> result;
    if (attribute == nullptr) {
        return result;
    }

    const int count = ippGetCount(attribute);
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        result.push_back(ippGetInteger(attribute, index));
    }
    return result;
}

[[nodiscard]] std::vector<int> resolutions(ipp_attribute_t* attribute) {
    std::vector<int> result;
    if (attribute == nullptr) {
        return result;
    }

    const int count = ippGetCount(attribute);
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        int y_resolution = 0;
        ipp_res_t units = IPP_RES_PER_INCH;
        int x_resolution = ippGetResolution(
            attribute,
            index,
            &y_resolution,
            &units);
        if (units == IPP_RES_PER_CM) {
            x_resolution = static_cast<int>(
                std::lround(static_cast<double>(x_resolution) * 2.54));
        }
        if (x_resolution > 0) {
            result.push_back(x_resolution);
        }
    }
    return result;
}

[[nodiscard]] DeviceState state_from_ipp(const int value) noexcept {
    switch (value) {
        case 3: return DeviceState::idle;
        case 4: return DeviceState::processing;
        case 5: return DeviceState::stopped;
        default: return DeviceState::unknown;
    }
}

void append_supplies(
    PrinterStatus& result,
    const std::vector<std::string>& names,
    const std::vector<std::string>& types,
    const std::vector<int>& levels,
    const std::vector<int>& low_levels) {

    for (std::size_t index = 0; index < names.size(); ++index) {
        SupplyLevel supply;
        supply.name = names[index];
        if (index < types.size()) {
            supply.type = types[index];
        }
        if (index < levels.size() && levels[index] >= 0 && levels[index] <= 100) {
            supply.percent = levels[index];
        }
        if (index < low_levels.size() && low_levels[index] >= 0 && low_levels[index] <= 100) {
            supply.low_threshold = low_levels[index];
        }
        result.supplies.push_back(std::move(supply));
    }
}

} // namespace

PrinterCapabilities DirectIppProbe::capabilities(
    const std::string& device_uri) const {

    static const char* const requested_attributes[] = {
        "print-color-mode-supported",
        "media-supported",
        "media-type-supported",
        "media-source-supported",
        "sides-supported",
        "print-quality-supported",
        "printer-resolution-supported",
        "document-format-supported",
        "copies-supported",
    };

    ipp_t* response = query(
        device_uri,
        requested_attributes,
        static_cast<int>(std::size(requested_attributes)));

    PrinterCapabilities result;
    result.printer = device_uri;
    result.source = "ipp-direct";
    result.color_modes = strings(ippFindAttribute(
        response,
        "print-color-mode-supported",
        IPP_TAG_KEYWORD));
    result.media = strings(ippFindAttribute(
        response,
        "media-supported",
        IPP_TAG_KEYWORD));
    result.media_types = strings(ippFindAttribute(
        response,
        "media-type-supported",
        IPP_TAG_KEYWORD));
    result.media_sources = strings(ippFindAttribute(
        response,
        "media-source-supported",
        IPP_TAG_KEYWORD));
    result.sides = strings(ippFindAttribute(
        response,
        "sides-supported",
        IPP_TAG_KEYWORD));
    result.qualities = integers(ippFindAttribute(
        response,
        "print-quality-supported",
        IPP_TAG_ENUM));
    result.resolutions_dpi = resolutions(ippFindAttribute(
        response,
        "printer-resolution-supported",
        IPP_TAG_RESOLUTION));
    result.document_formats = strings(ippFindAttribute(
        response,
        "document-format-supported",
        IPP_TAG_MIMETYPE));

    if (ipp_attribute_t* copies = ippFindAttribute(
            response,
            "copies-supported",
            IPP_TAG_RANGE)) {
        int upper = 1;
        result.copies_min = ippGetRange(copies, 0, &upper);
        result.copies_max = upper;
    }

    result.fetched_at = std::chrono::system_clock::now();
    ippDelete(response);
    return result;
}

PrinterStatus DirectIppProbe::status(const std::string& device_uri) const {
    static const char* const requested_attributes[] = {
        "printer-state",
        "printer-is-accepting-jobs",
        "printer-state-reasons",
        "marker-names",
        "marker-types",
        "marker-levels",
        "marker-low-levels",
    };

    ipp_t* response = query(
        device_uri,
        requested_attributes,
        static_cast<int>(std::size(requested_attributes)));

    PrinterStatus result;
    result.printer = device_uri;
    result.source = "ipp-direct";

    if (ipp_attribute_t* attribute = ippFindAttribute(
            response,
            "printer-state",
            IPP_TAG_ENUM)) {
        result.state = state_from_ipp(ippGetInteger(attribute, 0));
    }

    if (ipp_attribute_t* attribute = ippFindAttribute(
            response,
            "printer-is-accepting-jobs",
            IPP_TAG_BOOLEAN)) {
        result.accepting_jobs = ippGetBoolean(attribute, 0) != 0;
    }

    result.reasons = strings(ippFindAttribute(
        response,
        "printer-state-reasons",
        IPP_TAG_KEYWORD));
    result.reasons.erase(
        std::remove(result.reasons.begin(), result.reasons.end(), "none"),
        result.reasons.end());

    append_supplies(
        result,
        strings(ippFindAttribute(response, "marker-names", IPP_TAG_NAME)),
        strings(ippFindAttribute(response, "marker-types", IPP_TAG_KEYWORD)),
        integers(ippFindAttribute(response, "marker-levels", IPP_TAG_INTEGER)),
        integers(ippFindAttribute(response, "marker-low-levels", IPP_TAG_INTEGER)));

    ippDelete(response);
    return result;
}

} // namespace docsuite
