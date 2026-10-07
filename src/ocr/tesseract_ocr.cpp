// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/ocr/tesseract_ocr.hpp"

#include <stdexcept>
#include <string>

#ifdef DOCSUITE_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#endif

namespace docsuite {

bool TesseractOcr::available() const noexcept {
#ifdef DOCSUITE_HAVE_TESSERACT
    return true;
#else
    return false;
#endif
}

OcrResult TesseractOcr::recognize(
    const ScanFrame& frame,
    const std::string& language) const {

#ifndef DOCSUITE_HAVE_TESSERACT
    (void)frame;
    (void)language;
    throw std::runtime_error("DocSuite was built without Tesseract OCR support");
#else
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        throw std::runtime_error("Cannot OCR an empty scan frame");
    }

    tesseract::TessBaseAPI api;
    if (api.Init(nullptr, language.c_str()) != 0) {
        throw std::runtime_error(
            "Unable to initialize Tesseract language data: " + language);
    }

    const int bytes_per_pixel = frame.format == ScanPixelFormat::rgb24 ? 3 : 1;
    const int bytes_per_line = frame.width * bytes_per_pixel;

    api.SetImage(
        frame.pixels.data(),
        frame.width,
        frame.height,
        bytes_per_pixel,
        bytes_per_line);
    if (frame.dpi > 0) {
        api.SetSourceResolution(frame.dpi);
    }

    char* raw_text = api.GetUTF8Text();
    if (raw_text == nullptr) {
        api.End();
        throw std::runtime_error("Tesseract did not return OCR text");
    }

    OcrResult result;
    result.text = raw_text;
    result.mean_confidence = api.MeanTextConf();
    result.language = language;

    delete[] raw_text;
    api.End();
    return result;
#endif
}

} // namespace docsuite
