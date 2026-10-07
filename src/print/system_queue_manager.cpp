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
#include <vector>

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

void validate_instance_name(const std::string& name) {
    if (name.empty() || name.size() > 63U) {
        throw std::runtime_error(
            "CUPS profile instance must contain between 1 and 63 characters");
    }
    if (!std::all_of(name.begin(), name.end(), [](const unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
        })) {
        throw std::runtime_error(
            "CUPS profile instance may only contain letters, digits, '.', '_' and '-'");
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

void replace_option(
    cups_dest_t& destination,
    const char* name,
    const std::string& value) {

    destination.num_options = cupsRemoveOption(
        name,
        destination.num_options,
        &destination.options);
    if (!value.empty()) {
        destination.num_options = cupsAddOption(
            name,
            value.c_str(),
            destination.num_options,
            &destination.options);
    }
}

[[nodiscard]] cups_dest_t* require_base_destination(
    const std::string& queue,
    const int count,
    cups_dest_t* destinations) {

    cups_dest_t* destination = cupsGetDest(
        queue.c_str(),
        nullptr,
        count,
        destinations);
    if (destination == nullptr) {
        throw std::runtime_error(
            "CUPS destination not found for user profile: " + queue);
    }
    return destination;
}

void save_destinations(const int count, cups_dest_t* destinations) {
    if (cupsSetDests2(CUPS_HTTP_DEFAULT, count, destinations) != 0) {
        throw std::runtime_error(
            std::string{"Unable to save CUPS user destinations: "} +
            cupsLastErrorString());
    }
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
        cupsUser());
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

UserPrintProfileInfo SystemQueueManager::save_user_profile(
    const std::string& queue,
    const std::string& instance,
    const PrintProfile& profile) const {

    validate_queue_name(queue);
    validate_instance_name(instance);

    cups_dest_t* destinations = nullptr;
    int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &destinations);
    if (count < 0) {
        throw std::runtime_error(
            std::string{"Unable to read CUPS destinations: "} + cupsLastErrorString());
    }

    try {
        (void)require_base_destination(queue, count, destinations);
        count = cupsAddDest(
            queue.c_str(),
            instance.c_str(),
            count,
            &destinations);

        cups_dest_t* destination = cupsGetDest(
            queue.c_str(),
            instance.c_str(),
            count,
            destinations);
        if (destination == nullptr) {
            throw std::runtime_error(
                "Unable to create CUPS user profile instance " +
                queue + "/" + instance);
        }

        replace_option(*destination, "media", profile.media);
        replace_option(*destination, "print-color-mode", profile.color_mode);
        replace_option(*destination, "sides", profile.sides);
        replace_option(
            *destination,
            "print-quality",
            profile.quality > 0 ? std::to_string(profile.quality) : std::string{});
        replace_option(*destination, "media-source", profile.media_source);
        replace_option(*destination, "media-type", profile.media_type);
        replace_option(
            *destination,
            "copies",
            profile.copies > 0 ? std::to_string(profile.copies) : std::string{});

        save_destinations(count, destinations);
        cupsFreeDests(count, destinations);
    } catch (...) {
        cupsFreeDests(count, destinations);
        throw;
    }

    return UserPrintProfileInfo{
        .queue = queue,
        .instance = instance,
        .display_name = profile.name.empty()
            ? queue + "/" + instance
            : profile.name,
    };
}

std::vector<UserPrintProfileInfo> SystemQueueManager::list_user_profiles(
    const std::string& queue) const {

    validate_queue_name(queue);
    cups_dest_t* destinations = nullptr;
    const int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &destinations);
    if (count < 0) {
        throw std::runtime_error(
            std::string{"Unable to read CUPS destinations: "} + cupsLastErrorString());
    }

    std::vector<UserPrintProfileInfo> result;
    for (int i = 0; i < count; ++i) {
        const cups_dest_t& destination = destinations[i];
        if (destination.name == nullptr ||
            queue != destination.name ||
            destination.instance == nullptr ||
            *destination.instance == '\0') {
            continue;
        }
        result.push_back(UserPrintProfileInfo{
            .queue = queue,
            .instance = destination.instance,
            .display_name = queue + "/" + destination.instance,
        });
    }

    cupsFreeDests(count, destinations);
    std::sort(
        result.begin(),
        result.end(),
        [](const UserPrintProfileInfo& left, const UserPrintProfileInfo& right) {
            return left.instance < right.instance;
        });
    return result;
}

bool SystemQueueManager::remove_user_profile(
    const std::string& queue,
    const std::string& instance) const {

    validate_queue_name(queue);
    validate_instance_name(instance);

    cups_dest_t* destinations = nullptr;
    int count = cupsGetDests2(CUPS_HTTP_DEFAULT, &destinations);
    if (count < 0) {
        throw std::runtime_error(
            std::string{"Unable to read CUPS destinations: "} + cupsLastErrorString());
    }

    if (cupsGetDest(queue.c_str(), instance.c_str(), count, destinations) == nullptr) {
        cupsFreeDests(count, destinations);
        return false;
    }

    try {
        count = cupsRemoveDest(
            queue.c_str(),
            instance.c_str(),
            count,
            &destinations);
        save_destinations(count, destinations);
        cupsFreeDests(count, destinations);
    } catch (...) {
        cupsFreeDests(count, destinations);
        throw;
    }
    return true;
}

} // namespace docsuite
