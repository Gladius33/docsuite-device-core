// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/direct_ipp_probe.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename T>
[[nodiscard]] bool contains(const std::vector<T>& values, const T& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: docsuite-direct-ipp-probe-test <ipp-uri>");
        }

        const std::string uri = argv[1];
        docsuite::DirectIppProbe probe;

        const auto caps = probe.capabilities(uri);
        require(caps.source == "ipp-direct", "capability source is not ipp-direct");
        require(contains(caps.color_modes, std::string{"color"}), "missing color mode");
        require(contains(caps.color_modes, std::string{"monochrome"}), "missing monochrome mode");
        require(contains(caps.sides, std::string{"one-sided"}), "missing one-sided mode");
        require(
            contains(caps.sides, std::string{"two-sided-long-edge"}),
            "missing long-edge duplex mode");
        require(contains(caps.qualities, 3), "missing draft quality");
        require(contains(caps.qualities, 4), "missing normal quality");
        require(contains(caps.qualities, 5), "missing high quality");
        require(contains(caps.resolutions_dpi, 600), "missing 600 dpi resolution");
        require(contains(caps.media, std::string{"iso_a4_210x297mm"}), "missing A4 medium");
        require(contains(caps.media_sources, std::string{"main"}), "missing main paper source");
        require(contains(caps.media_sources, std::string{"rear"}), "missing rear paper source");
        require(contains(caps.document_formats, std::string{"image/jpeg"}), "missing JPEG format");
        require(contains(caps.document_formats, std::string{"image/urf"}), "missing URF format");
        require(
            contains(caps.document_formats, std::string{"image/pwg-raster"}),
            "missing PWG raster format");
        require(caps.copies_min == 1, "copies minimum is not 1");
        require(caps.copies_max == 99, "copies maximum is not 99");

        const auto status = probe.status(uri);
        require(status.source == "ipp-direct", "status source is not ipp-direct");
        require(status.state == docsuite::DeviceState::idle, "printer is not idle");
        require(status.accepting_jobs, "printer is not accepting jobs");
        require(status.reasons.empty(), "printer-state-reasons should normalize 'none' away");
        require(status.supplies.size() == 2U, "expected Color and Black supplies");
        require(status.supplies[0].name == "Color", "first supply is not Color");
        require(!status.supplies[0].percent.has_value(), "negative Color marker level must be unknown");
        require(status.supplies[1].name == "Black", "second supply is not Black");
        require(status.supplies[1].percent.has_value(), "Black marker level is unavailable");
        require(*status.supplies[1].percent == 10, "Black marker level is not 10 percent");
        require(status.supplies[1].low_threshold == 15, "Black low threshold is not 15 percent");

        std::cout << "Direct IPP mock probe: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Direct IPP mock probe failed: " << error.what() << '\n';
        return 1;
    }
}
