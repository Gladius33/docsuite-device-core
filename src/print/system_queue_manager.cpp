// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/system_queue_manager.hpp"

#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace docsuite {
namespace {

[[nodiscard]] bool driverless_uri(const std::string& uri) {
    return uri.rfind("ipp://", 0) == 0 || uri.rfind("ipps://", 0) == 0;
}

void validate_queue_name(const std::string& name) {
    if (name.empty() || name.size() > 127U) {
        throw std::runtime_error("CUPS queue name must contain between 1 and 127 characters");
    }
    if (!std::all_of(name.begin(), name.end(), [](const unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
        })) {
        throw std::runtime_error(
            "CUPS queue name may only contain letters, digits, '.', '_' and '-'");
    }
}

[[nodiscard]] std::string option_value(
    const cups_dest_t& destination,
    const char* name) {
    if (const char* value = cupsGetOption(
            name,
            destination.num_options,
            destination.options)) {
        return value;
    }
    return {};
}

} // namespace

SystemQueueInfo SystemQueueManager::inspect(const std::string& name) const {
    validate_queue_name(name);

    SystemQueueInfo result;
    result.name = name;

    cups_dest_t* destination = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name.c_str(), nullptr);
    if (destination == nullptr) {
        return result;
    }

    result.exists = true;
    result.device_uri = option_value(*destination, "device-uri");
    result.printer_uri = option_value(*destination, "printer-uri-supported");
    if (result.printer_uri.empty()) {
        result.printer_uri = "ipp://localhost/printers/" + name;
    }
    result.driverless = driverless_uri(result.device_uri);

    const std::string temporary = option_value(*destination, "printer-is-temporary");
    result.temporary = temporary == "true" || temporary == "1" ||
        option_value(*destination, "printer-type").find("temporary") != std::string::npos;

    cupsFreeDests(1, destination);
    return result;
}

SystemQueueInfo SystemQueueManager::ensure_temporary_driverless(
    const std::string& name,
    const std::string& device_uri,
    const std::string& info) const {

    validate_queue_name(name);
    if (!driverless_uri(device_uri)) {
        throw std::runtime_error(
            "DocSuite only creates local queues for ipp:// or ipps:// IPP Everywhere devices");
    }

    if (auto existing = inspect(name); existing.exists) {
        if (!existing.driverless) {
            throw std::runtime_error(
                "A non-driverless CUPS queue with the requested name already exists");
        }
        if (!existing.device_uri.empty() && existing.device_uri != device_uri) {
            throw std::runtime_error(
                "A driverless CUPS queue with the requested name points to a different device");
        }
        return existing;
    }

    ipp_t* request = ippNewRequest(IPP_OP_CUPS_CREATE_LOCAL_PRINTER);
    if (request == nullptr) {
        throw std::runtime_error("Unable to allocate CUPS local-printer request");
    }

    ippAddString(
        request,
        IPP_TAG_OPERATION,
        IPP_TAG_URI,
        "printer-uri",
        nullptr,
        "ipp://localhost/");
    ippAddString(
        request,
        IPP_TAG_OPERATION,
        IPP_TAG_NAME,
        "requesting-user-name",
        nullptr,
        cupsGetUser());
    ippAddString(
        request,
        IPP_TAG_PRINTER,
        IPP_TAG_URI,
        "device-uri",
        nullptr,
        device_uri.c_str());
    ippAddString(
        request,
        IPP_TAG_PRINTER,
        IPP_TAG_NAME,
        "printer-name",
        nullptr,
        name.c_str());
    if (!info.empty()) {
        ippAddString(
            request,
            IPP_TAG_PRINTER,
            IPP_TAG_TEXT,
            "printer-info",
            nullptr,
            info.c_str());
    }

    ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/");
    if (response == nullptr) {
        throw std::runtime_error(
            std::string{"CUPS-Create-Local-Printer failed: "} + cupsLastErrorString());
    }

    SystemQueueInfo result;
    result.name = name;
    result.device_uri = device_uri;
    result.driverless = true;
    result.temporary = true;
    result.exists = true;

    if (ipp_attribute_t* attribute = ippFindAttribute(
            response,
            "printer-uri-supported",
            IPP_TAG_URI)) {
        if (const char* value = ippGetString(attribute, 0, nullptr)) {
            result.printer_uri = value;
        }
    }

    const ipp_status_t status = ippGetStatusCode(response);
    ippDelete(response);

    if (status > IPP_STATUS_OK_EVENTS_COMPLETE || result.printer_uri.empty()) {
        throw std::runtime_error(
            std::string{"CUPS-Create-Local-Printer was rejected: "} + cupsLastErrorString());
    }

    return result;
}

} // namespace docsuite
