// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/cups_print_backend.hpp"

#include <cups/cups.h>

#include <stdexcept>

namespace docsuite {

std::vector<PrinterInfo> CupsPrintBackend::list_printers() const {
    cups_dest_t* destinations = nullptr;
    const int count = cupsGetDests(&destinations);

    std::vector<PrinterInfo> result;
    result.reserve(static_cast<std::size_t>(count));

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

        result.push_back(std::move(info));
    }

    cupsFreeDests(count, destinations);
    return result;
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

} // namespace docsuite
