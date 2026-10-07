// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/cups_print_backend.hpp"
#include "docsuite/print/print_validation.hpp"

#include <cups/cups.h>

#include <sstream>
#include <stdexcept>
#include <string>

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

} // namespace

PrintPreflightResult CupsPrintBackend::preflight(
    const std::string& printer,
    const PrintProfile& profile,
    const bool force_refresh) const {

    return validate_print_profile(
        capabilities(printer, force_refresh),
        profile);
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
