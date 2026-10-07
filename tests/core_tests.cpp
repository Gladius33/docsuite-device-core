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

    std::cout << "DocSuite core smoke tests passed\n";
    return 0;
}
