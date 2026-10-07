// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/print_validation.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace docsuite {
namespace {

[[nodiscard]] bool contains(
    const std::vector<std::string>& values,
    const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] bool contains(
    const std::vector<int>& values,
    const int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void validate_keyword(
    PrintPreflightResult& result,
    const char* label,
    const std::string& value,
    const std::vector<std::string>& supported,
    const bool optional) {

    if (value.empty()) {
        if (!optional) {
            result.errors.emplace_back(std::string{label} + " is required");
        }
        return;
    }

    if (supported.empty()) {
        result.warnings.emplace_back(
            std::string{label} + " capability is not advertised; CUPS will validate '" +
            value + "'");
        return;
    }

    if (!contains(supported, value)) {
        result.errors.emplace_back(
            std::string{label} + " value '" + value + "' is not supported");
    }
}

} // namespace

PrintPreflightResult validate_print_profile(
    const PrinterCapabilities& capabilities,
    const PrintProfile& profile) {

    PrintPreflightResult result;

    validate_keyword(
        result,
        "media",
        profile.media,
        capabilities.media,
        false);
    validate_keyword(
        result,
        "print-color-mode",
        profile.color_mode,
        capabilities.color_modes,
        false);
    validate_keyword(
        result,
        "sides",
        profile.sides,
        capabilities.sides,
        false);
    validate_keyword(
        result,
        "media-source",
        profile.media_source,
        capabilities.media_sources,
        true);
    validate_keyword(
        result,
        "media-type",
        profile.media_type,
        capabilities.media_types,
        true);

    if (profile.quality <= 0) {
        result.warnings.emplace_back(
            "print-quality is unset; the CUPS/default printer quality will be used");
    } else if (capabilities.qualities.empty()) {
        result.warnings.emplace_back(
            "print-quality capability is not advertised; CUPS will validate '" +
            std::to_string(profile.quality) + "'");
    } else if (!contains(capabilities.qualities, profile.quality)) {
        result.errors.emplace_back(
            "print-quality value '" + std::to_string(profile.quality) +
            "' is not supported");
    }

    if (profile.copies <= 0) {
        result.errors.emplace_back("copies must be positive");
    } else if (capabilities.copies_min > 0 &&
               capabilities.copies_max >= capabilities.copies_min &&
               (profile.copies < capabilities.copies_min ||
                profile.copies > capabilities.copies_max)) {
        result.errors.emplace_back(
            "copies value '" + std::to_string(profile.copies) +
            "' is outside supported range " +
            std::to_string(capabilities.copies_min) + "-" +
            std::to_string(capabilities.copies_max));
    }

    result.ok = result.errors.empty();
    return result;
}

} // namespace docsuite
