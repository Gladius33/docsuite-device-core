// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/cups_print_backend.hpp"
#include "docsuite/print/print_validation.hpp"

#include <cups/cups.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace docsuite {
namespace {

void add_nonempty_option(
    const char* name,
    const std::string& value,
    int& count,
    cups_option_t** options) {
    if (!value.empty()) {
        count = cupsAddOption(name, value.c_str(), count, options);
    }
}

[[nodiscard]] std::string join_errors(const std::vector<std::string>& errors) {
    std::ostringstream message;
    message << "Print preflight failed";
    for (const auto& error : errors) {
        message << "; " << error;
    }
    return message.str();
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> profile_options(
    const PrintProfile& profile) {

    std::vector<std::pair<std::string, std::string>> options;
    if (!profile.media.empty()) {
        options.emplace_back("media", profile.media);
    }
    if (!profile.color_mode.empty()) {
        options.emplace_back("print-color-mode", profile.color_mode);
    }
    if (!profile.sides.empty()) {
        options.emplace_back("sides", profile.sides);
    }
    if (profile.quality > 0) {
        options.emplace_back("print-quality", std::to_string(profile.quality));
    }
    if (!profile.media_source.empty()) {
        options.emplace_back("media-source", profile.media_source);
    }
    if (!profile.media_type.empty()) {
        options.emplace_back("media-type", profile.media_type);
    }
    if (profile.copies > 0) {
        options.emplace_back("copies", std::to_string(profile.copies));
    }
    return options;
}

void append_option_pairs(
    std::vector<std::string>& output,
    const char* prefix,
    const int count,
    cups_option_t* options) {

    for (int i = 0; i < count; ++i) {
        const char* name = options[i].name != nullptr ? options[i].name : "?";
        const char* value = options[i].value != nullptr ? options[i].value : "?";
        output.emplace_back(
            std::string{prefix} + name + "=" + value);
    }
}

} // namespace

PrintPreflightResult CupsPrintBackend::preflight(
    const std::string& printer,
    const PrintProfile& profile,
    const bool force_refresh) const {

    return validate_print_profile(
        capabilities(printer, force_refresh),
        profile);
}

PrintPreflightResult CupsPrintBackend::preflight_detailed(
    const std::string& printer,
    const PrintProfile& profile,
    const bool force_refresh) const {

    PrintPreflightResult result = preflight(printer, profile, force_refresh);
    if (!result.ok) {
        return result;
    }

    cups_dest_t* destination = cupsGetNamedDest(
        CUPS_HTTP_DEFAULT,
        printer.c_str(),
        nullptr);
    if (destination == nullptr) {
        result.ok = false;
        result.errors.emplace_back("CUPS destination not found: " + printer);
        return result;
    }

    cups_dinfo_t* info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, destination);
    if (info == nullptr) {
        cupsFreeDests(1, destination);
        result.warnings.emplace_back(
            "CUPS detailed destination information is unavailable; cross-option conflicts were not checked");
        return result;
    }

    cups_option_t* accepted = nullptr;
    int accepted_count = 0;

    for (const auto& [name, value] : profile_options(profile)) {
        if (cupsCheckDestSupported(
                CUPS_HTTP_DEFAULT,
                destination,
                info,
                name.c_str(),
                value.c_str()) == 0) {
            result.errors.emplace_back(
                "CUPS does not report support for " + name + "='" + value + "'");
            continue;
        }

        cups_option_t* conflicts = nullptr;
        int conflict_count = 0;
        cups_option_t* resolved = nullptr;
        int resolved_count = 0;

        const int conflict = cupsCopyDestConflicts(
            CUPS_HTTP_DEFAULT,
            destination,
            info,
            accepted_count,
            accepted,
            name.c_str(),
            value.c_str(),
            &conflict_count,
            &conflicts,
            &resolved_count,
            &resolved);

        if (conflict < 0) {
            result.warnings.emplace_back(
                "CUPS could not evaluate cross-option constraints for " +
                name + "='" + value + "'");
        } else if (conflict > 0) {
            result.errors.emplace_back(
                "CUPS reports an option conflict when applying " +
                name + "='" + value + "'");
            append_option_pairs(
                result.errors,
                "conflict: ",
                conflict_count,
                conflicts);
            append_option_pairs(
                result.warnings,
                "CUPS suggested resolution: ",
                resolved_count,
                resolved);
        } else {
            accepted_count = cupsAddOption(
                name.c_str(),
                value.c_str(),
                accepted_count,
                &accepted);
        }

        cupsFreeOptions(conflict_count, conflicts);
        cupsFreeOptions(resolved_count, resolved);
    }

    cupsFreeOptions(accepted_count, accepted);
    cupsFreeDestInfo(info);
    cupsFreeDests(1, destination);

    result.ok = result.errors.empty();
    return result;
}

int CupsPrintBackend::print_file_advanced(
    const std::string& printer,
    const std::string& path,
    const std::string& title,
    const PrintProfile& profile) const {

    const auto validation = preflight(printer, profile, false);
    if (!validation.ok) {
        throw std::runtime_error(join_errors(validation.errors));
    }

    cups_option_t* options = nullptr;
    int option_count = 0;

    add_nonempty_option("media", profile.media, option_count, &options);
    add_nonempty_option("print-color-mode", profile.color_mode, option_count, &options);
    add_nonempty_option("sides", profile.sides, option_count, &options);
    if (profile.quality > 0) {
        option_count = cupsAddOption(
            "print-quality",
            std::to_string(profile.quality).c_str(),
            option_count,
            &options);
    }
    add_nonempty_option("media-source", profile.media_source, option_count, &options);
    add_nonempty_option("media-type", profile.media_type, option_count, &options);
    if (profile.copies > 0) {
        option_count = cupsAddOption(
            "copies",
            std::to_string(profile.copies).c_str(),
            option_count,
            &options);
    }

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

} // namespace docsuite
