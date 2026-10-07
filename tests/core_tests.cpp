// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/core/types.hpp"
#include "docsuite/image/image_processor.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"
#include "docsuite/print/job_manager.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    const docsuite::PrintProfile standard{};
    assert(standard.media == "iso_a4_210x297mm");
    assert(standard.color_mode == "color");
    assert(standard.sides == "one-sided");
    assert(standard.quality == 4);
    assert(standard.copies == 1);

    docsuite::PrintProfile mono{};
    mono.name = "Monochrome";
    mono.color_mode = "monochrome";
    mono.media_source = "main";
    mono.media_type = "stationery";
    mono.copies = 2;
    assert(mono.color_mode == "monochrome");
    assert(mono.media_source == "main");
    assert(mono.copies == 2);

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

    docsuite::ImageProcessor processor;
    const auto gray = processor.grayscale(frame);
    assert(gray.width == 2);
    assert(gray.height == 1);
    assert(gray.dpi == 300);
    assert(gray.format == docsuite::ScanPixelFormat::gray8);
    assert(gray.pixels.size() == 2U);
    assert(gray.pixels.at(0) > 60U && gray.pixels.at(0) < 100U);
    assert(gray.pixels.at(1) > 130U && gray.pixels.at(1) < 180U);

    docsuite::ScanFrame white{};
    white.width = 100;
    white.height = 100;
    white.dpi = 300;
    white.format = docsuite::ScanPixelFormat::gray8;
    white.pixels.assign(10000U, static_cast<std::uint8_t>(255));
    assert(processor.is_blank(white));

    auto content_frame = white;
    for (int y = 40; y < 60; ++y) {
        for (int x = 40; x < 60; ++x) {
            content_frame.pixels[
                static_cast<std::size_t>(y) * 100U + static_cast<std::size_t>(x)] = 0;
        }
    }
    assert(!processor.is_blank(content_frame));
    const auto rect = processor.detect_content(content_frame);
    assert(rect.x >= 0 && rect.y >= 0);
    assert(rect.x <= 40 && rect.y <= 40);
    assert(rect.x + rect.width > 59);
    assert(rect.y + rect.height > 59);
    assert(rect.width < 100 && rect.height < 100);

    const auto cropped = processor.crop(content_frame, rect);
    assert(cropped.width == rect.width);
    assert(cropped.height == rect.height);
    assert(cropped.dpi == 300);
    assert(!cropped.pixels.empty());

    const auto enhanced = processor.enhance_document(content_frame, false);
    assert(enhanced.width == content_frame.width);
    assert(enhanced.height == content_frame.height);
    assert(!enhanced.pixels.empty());

    const auto binary = processor.enhance_document(content_frame, true);
    assert(binary.width == content_frame.width);
    assert(binary.height == content_frame.height);
    assert(binary.format == docsuite::ScanPixelFormat::gray8);

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
