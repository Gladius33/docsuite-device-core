// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/cups_print_backend.hpp"
#include "docsuite/print/direct_ipp_probe.hpp"

#include <cups/cups.h>
#include <cups/ipp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace docsuite {
namespace {

[[nodiscard]] bool option_is_true(const char* value) {
    if (value == nullptr) {
        return false;
    }
    const std::string_view text{value};
    return text == "true" || text == "yes" || text == "1";
}

[[nodiscard]] std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] bool is_dnssd_printer_uri(const std::string& uri) {
    const auto lowered = lower_copy(uri);
    return lowered.find("._ipp._tcp.local") != std::string::npos ||
        lowered.find("._ipps._tcp.local") != std::string::npos;
}

[[nodiscard]] bool is_self_advertised_cups_queue(const std::string& uri) {
    if (uri.find("._ipp._tcp.local/cups") == std::string::npos &&
        uri.find("._ipps._tcp.local/cups") == std::string::npos) {
        return false;
    }

    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname) - 1U) != 0) {
        return false;
    }

    const std::string lowered_uri = lower_copy(uri);
    const std::string marker = "%40%20" + lower_copy(hostname) + ".";
    return lowered_uri.find(marker) != std::string::npos;
}

[[nodiscard]] std::string model_token_with_digit(const std::string& text) {
    std::string token;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const unsigned char ch = i < text.size()
            ? static_cast<unsigned char>(text[i])
            : static_cast<unsigned char>(' ');

        if (std::isalnum(ch) != 0) {
            token.push_back(static_cast<char>(std::tolower(ch)));
            continue;
        }

        if (token.size() >= 4U &&
            std::any_of(token.begin(), token.end(), [](const unsigned char value) {
                return std::isdigit(value) != 0;
            })) {
            return token;
        }
        token.clear();
    }
    return {};
}

[[nodiscard]] std::vector<std::string> ipp_strings(ipp_attribute_t* attribute) {
    std::vector<std::string> values;
    if (attribute == nullptr) {
        return values;
    }

    const int count = ippGetCount(attribute);
    values.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        if (const char* value = ippGetString(attribute, i, nullptr)) {
            values.emplace_back(value);
        }
    }
    return values;
}

[[nodiscard]] std::vector<int> ipp_integers(ipp_attribute_t* attribute) {
    std::vector<int> values;
    if (attribute == nullptr) {
        return values;
    }

    const int count = ippGetCount(attribute);
    values.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        values.push_back(ippGetInteger(attribute, i));
    }
    return values;
}

[[nodiscard]] std::vector<int> ipp_resolutions(ipp_attribute_t* attribute) {
    std::vector<int> values;
    if (attribute == nullptr) {
        return values;
    }

    const int count = ippGetCount(attribute);
    values.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        int y_resolution = 0;
        ipp_res_t units = IPP_RES_PER_INCH;
        int x_resolution = ippGetResolution(attribute, i, &y_resolution, &units);
        if (units == IPP_RES_PER_CM) {
            x_resolution = static_cast<int>(std::lround(static_cast<double>(x_resolution) * 2.54));
        }
        if (x_resolution > 0) {
            values.push_back(x_resolution);
        }
    }
    return values;
}

[[nodiscard]] std::vector<std::string> split_csv(const char* raw) {
    std::vector<std::string> values;
    if (raw == nullptr || *raw == '\0') {
        return values;
    }

    std::string input{raw};
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t comma = input.find(',', start);
        const std::size_t end = comma == std::string::npos ? input.size() : comma;
        std::string value = input.substr(start, end - start);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.pop_back();
        }
        if (!value.empty()) {
            values.push_back(std::move(value));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1U;
    }
    return values;
}

[[nodiscard]] std::vector<int> split_csv_ints(const char* raw) {
    std::vector<int> result;
    for (const auto& value : split_csv(raw)) {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && end != nullptr && *end == '\0') {
            result.push_back(static_cast<int>(parsed));
        }
    }
    return result;
}

[[nodiscard]] DeviceState parse_printer_state_value(const int value) {
    switch (value) {
        case 3: return DeviceState::idle;
        case 4: return DeviceState::processing;
        case 5: return DeviceState::stopped;
        default: return DeviceState::unknown;
    }
}

[[nodiscard]] DeviceState parse_printer_state(const char* state) {
    if (state == nullptr) {
        return DeviceState::unknown;
    }
    return parse_printer_state_value(std::atoi(state));
}

void append_supplies(
    PrinterStatus& result,
    const std::vector<std::string>& names,
    const std::vector<std::string>& types,
    const std::vector<int>& levels,
    const std::vector<int>& lows) {

    for (std::size_t i = 0; i < names.size(); ++i) {
        SupplyLevel supply;
        supply.name = names[i];
        if (i < types.size()) {
            supply.type = types[i];
        }
        if (i < levels.size() && levels[i] >= 0 && levels[i] <= 100) {
            supply.percent = levels[i];
        }
        if (i < lows.size() && lows[i] >= 0 && lows[i] <= 100) {
            supply.low_threshold = lows[i];
        }
        result.supplies.push_back(std::move(supply));
    }
}

[[nodiscard]] PrinterStatus local_cups_status(
    const std::string& printer,
    const cups_dest_t& destination) {

    PrinterStatus result;
    result.printer = printer;
    result.source = "cups";
    result.state = parse_printer_state(cupsGetOption(
        "printer-state", destination.num_options, destination.options));
    result.accepting_jobs = option_is_true(cupsGetOption(
        "printer-is-accepting-jobs", destination.num_options, destination.options));

    result.reasons = split_csv(cupsGetOption(
        "printer-state-reasons", destination.num_options, destination.options));
    result.reasons.erase(
        std::remove(result.reasons.begin(), result.reasons.end(), "none"),
        result.reasons.end());

    append_supplies(
        result,
        split_csv(cupsGetOption("marker-names", destination.num_options, destination.options)),
        split_csv(cupsGetOption("marker-types", destination.num_options, destination.options)),
        split_csv_ints(cupsGetOption("marker-levels", destination.num_options, destination.options)),
        split_csv_ints(cupsGetOption("marker-low-levels", destination.num_options, destination.options)));

    return result;
}

} // namespace

std::vector<PrinterInfo> CupsPrintBackend::list_printers(const bool include_transient) const {
    cups_dest_t* destinations = nullptr;
    const int count = cupsGetDests(&destinations);

    std::vector<PrinterInfo> all;
    all.reserve(static_cast<std::size_t>(std::max(count, 0)));

    for (int i = 0; i < count; ++i) {
        const cups_dest_t& dest = destinations[i];
        PrinterInfo info;
        info.name = dest.name != nullptr ? dest.name : "";
        info.is_default = dest.is_default != 0;

        if (const char* uri = cupsGetOption("device-uri", dest.num_options, dest.options)) {
            info.uri = uri;
        }
        if (const char* model = cupsGetOption("printer-make-and-model", dest.num_options, dest.options)) {
            info.model = model;
        }
        info.temporary = option_is_true(
            cupsGetOption("printer-is-temporary", dest.num_options, dest.options));
        all.push_back(std::move(info));
    }

    cupsFreeDests(count, destinations);
    if (include_transient) {
        return all;
    }

    std::vector<PrinterInfo> result;
    result.reserve(all.size());

    for (const auto& candidate : all) {
        if (candidate.temporary || is_self_advertised_cups_queue(candidate.uri)) {
            continue;
        }

        if (is_dnssd_printer_uri(candidate.uri)) {
            const std::string candidate_token = model_token_with_digit(
                candidate.model + " " + candidate.name);
            const bool represented_by_managed_queue = !candidate_token.empty() && std::any_of(
                all.begin(), all.end(),
                [&candidate, &candidate_token](const PrinterInfo& other) {
                    if (&candidate == &other || other.temporary || is_dnssd_printer_uri(other.uri)) {
                        return false;
                    }
                    return model_token_with_digit(other.model + " " + other.name) == candidate_token;
                });
            if (represented_by_managed_queue) {
                continue;
            }
        }

        result.push_back(candidate);
    }

    return result;
}

PrinterCapabilities CupsPrintBackend::capabilities(
    const std::string& printer,
    const bool force_refresh) const {

    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{capability_cache_mutex_};
        const auto found = capability_cache_.find(printer);
        if (!force_refresh && found != capability_cache_.end() &&
            now - found->second.fetched_at < capability_ttl_) {
            return found->second.value;
        }
    }

    cups_dest_t* destination = cupsGetNamedDest(CUPS_HTTP_DEFAULT, printer.c_str(), nullptr);
    if (destination == nullptr) {
        throw std::runtime_error("CUPS destination not found: " + printer);
    }

    PrinterCapabilities result;
    bool loaded_direct = false;
    const char* raw_device_uri = cupsGetOption(
        "device-uri", destination->num_options, destination->options);

    if (raw_device_uri != nullptr) {
        try {
            result = DirectIppProbe{}.capabilities(raw_device_uri);
            result.printer = printer;
            loaded_direct = true;
        } catch (const std::exception&) {
            // A proprietary, stale or temporarily unreachable device URI must not
            // prevent CUPS from providing its own normalized destination data.
        }
    }

    if (!loaded_direct) {
        cups_dinfo_t* info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, destination);
        if (info == nullptr) {
            cupsFreeDests(1, destination);
            throw std::runtime_error("Unable to query printer capabilities: " + printer);
        }

        result.printer = printer;
        result.source = "cups";
        result.color_modes = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "print-color-mode"));
        result.media = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "media"));
        result.media_types = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "media-type"));
        result.media_sources = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "media-source"));
        result.sides = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "sides"));
        result.qualities = ipp_integers(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "print-quality"));
        result.resolutions_dpi = ipp_resolutions(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "printer-resolution"));
        result.document_formats = ipp_strings(cupsFindDestSupported(
            CUPS_HTTP_DEFAULT, destination, info, "document-format"));

        if (ipp_attribute_t* copies = cupsFindDestSupported(
                CUPS_HTTP_DEFAULT, destination, info, "copies")) {
            int upper = 1;
            result.copies_min = ippGetRange(copies, 0, &upper);
            result.copies_max = upper;
        }

        result.fetched_at = std::chrono::system_clock::now();
        cupsFreeDestInfo(info);
    }

    cupsFreeDests(1, destination);

    {
        std::scoped_lock lock{capability_cache_mutex_};
        capability_cache_[printer] = CachedCapabilities{
            .value = result,
            .fetched_at = now,
        };
    }

    return result;
}

PrinterStatus CupsPrintBackend::status(const std::string& printer) const {
    cups_dest_t* destination = cupsGetNamedDest(CUPS_HTTP_DEFAULT, printer.c_str(), nullptr);
    if (destination == nullptr) {
        throw std::runtime_error("CUPS destination not found: " + printer);
    }

    PrinterStatus local = local_cups_status(printer, *destination);
    const char* raw_device_uri = cupsGetOption(
        "device-uri", destination->num_options, destination->options);

    if (raw_device_uri != nullptr) {
        try {
            PrinterStatus direct = DirectIppProbe{}.status(raw_device_uri);
            direct.printer = printer;
            if (direct.supplies.empty()) {
                direct.supplies = local.supplies;
            }
            cupsFreeDests(1, destination);
            return direct;
        } catch (const std::exception&) {
            // Preserve the local CUPS status when the physical endpoint cannot
            // be queried directly.
        }
    }

    cupsFreeDests(1, destination);
    return local;
}

int CupsPrintBackend::print_file(
    const std::string& printer,
    const std::string& path,
    const std::string& title,
    const PrintProfile& profile) const {

    cups_option_t* options = nullptr;
    int option_count = 0;

    option_count = cupsAddOption("media", profile.media.c_str(), option_count, &options);
    option_count = cupsAddOption("print-color-mode", profile.color_mode.c_str(), option_count, &options);
    option_count = cupsAddOption("sides", profile.sides.c_str(), option_count, &options);
    option_count = cupsAddOption("print-quality", std::to_string(profile.quality).c_str(), option_count, &options);

    const int job_id = cupsPrintFile(
        printer.c_str(),
        path.c_str(),
        title.c_str(),
        option_count,
        options);

    cupsFreeOptions(option_count, options);

    if (job_id == 0) {
        throw std::runtime_error(cupsLastErrorString());
    }

    return job_id;
}

void CupsPrintBackend::clear_capability_cache() const {
    std::scoped_lock lock{capability_cache_mutex_};
    capability_cache_.clear();
}

} // namespace docsuite
