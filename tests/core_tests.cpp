// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/core/types.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"
#include "docsuite/print/job_manager.hpp"

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
    caps.source = "ipp-direct";
    caps.color_modes = {"color", "monochrome"};
    caps.sides = {"one-sided", "two-sided-long-edge"};
    caps.qualities = {3, 4, 5};
    caps.resolutions_dpi = {600};
    caps.copies_min = 1;
    caps.copies_max = 99;
    assert(caps.color_modes.size() == 2U);
    assert(caps.qualities.at(1) == 4);
    assert(caps.copies_max == 99);

    docsuite::ScannerCapabilities scanner_caps{};
    scanner_caps.scanner = "airscan:test";
    scanner_caps.modes = {"Color", "Gray"};
    scanner_caps.resolutions_dpi = {150, 300, 600};
    scanner_caps.sources = {"Flatbed"};
    scanner_caps.max_width_mm = 215.9;
    scanner_caps.max_height_mm = 296.7;
    assert(scanner_caps.modes.size() == 2U);
    assert(scanner_caps.resolutions_dpi.at(1) == 300);

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

    docsuite::PrintJobInfo job{};
    job.id = 42;
    job.printer = "test-printer";
    job.state = docsuite::PrintJobState::processing;
    assert(job.id == 42);
    assert(std::string{docsuite::print_job_state_name(job.state)} == "processing");

    docsuite::PrintJobTrace trace{};
    trace.job_id = 42;
    trace.submit_to_accept = std::chrono::milliseconds{12};
    trace.events.push_back(docsuite::JobTimelineEvent{
        .name = "cups-accepted",
        .state = docsuite::PrintJobState::pending,
        .at = {},
        .since_submit = std::chrono::milliseconds{12},
    });
    assert(trace.events.size() == 1U);
    assert(trace.submit_to_accept.count() == 12);

    docsuite::ScanSettings scan_settings{};
    assert(scan_settings.dpi == 300);
    assert(scan_settings.mode == "Color");
    assert(scan_settings.source == "Flatbed");

    docsuite::ScanFrame frame{};
    frame.width = 2;
    frame.height = 1;
    frame.dpi = 300;
    frame.format = docsuite::ScanPixelFormat::rgb24;
    frame.pixels = {255, 0, 0, 0, 255, 0};
    assert(frame.pixels.size() == 6U);

    docsuite::OcrResult ocr{};
    ocr.text = "DocSuite";
    ocr.mean_confidence = 94;
    ocr.language = "fra+eng";
    ocr.words.push_back(docsuite::OcrWord{
        .text = "DocSuite",
        .confidence = 94.0F,
        .left = 10,
        .top = 20,
        .width = 100,
        .height = 30,
    });
    assert(ocr.words.size() == 1U);
    assert(ocr.words.front().width == 100);

    std::cout << "DocSuite core smoke tests passed\n";
    return 0;
}
