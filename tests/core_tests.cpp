// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/core/types.hpp"

#include <cassert>
#include <iostream>

int main() {
    const docsuite::PrintProfile standard{};
    assert(standard.media == "iso_a4_210x297mm");
    assert(standard.color_mode == "color");
    assert(standard.sides == "one-sided");
    assert(standard.quality == 4);

    docsuite::PrintProfile mono{};
    mono.name = "Monochrome";
    mono.color_mode = "monochrome";
    assert(mono.color_mode == "monochrome");

    docsuite::PrinterCapabilities caps{};
    caps.printer = "test-printer";
    caps.color_modes = {"color", "monochrome"};
    caps.sides = {"one-sided", "two-sided-long-edge"};
    caps.qualities = {3, 4, 5};
    caps.resolutions_dpi = {600};
    caps.copies_min = 1;
    caps.copies_max = 99;
    assert(caps.color_modes.size() == 2U);
    assert(caps.qualities.at(1) == 4);
    assert(caps.copies_max == 99);

    docsuite::SupplyLevel unknown_color{
        .name = "Color",
        .type = "ink-cartridge",
        .percent = std::nullopt,
        .low_threshold = 15,
    };
    assert(!unknown_color.percent.has_value());

    docsuite::SupplyLevel low_black{
        .name = "Black",
        .type = "ink-cartridge",
        .percent = 10,
        .low_threshold = 15,
    };
    assert(low_black.percent.has_value());
    assert(*low_black.percent <= low_black.low_threshold);

    std::cout << "DocSuite core smoke tests passed\n";
    return 0;
}
