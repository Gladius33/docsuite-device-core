// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/image/image_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#ifdef DOCSUITE_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace docsuite {
namespace {

void validate_frame(const ScanFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) {
        throw std::runtime_error("Cannot process an empty scan frame");
    }
    const std::size_t channels = frame.format == ScanPixelFormat::rgb24 ? 3U : 1U;
    const std::size_t expected = static_cast<std::size_t>(frame.width) *
        static_cast<std::size_t>(frame.height) * channels;
    if (frame.pixels.size() < expected) {
        throw std::runtime_error("Scan frame buffer is smaller than its declared dimensions");
    }
}

[[nodiscard]] std::uint8_t luminance(
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue) {
    const unsigned value = 299U * red + 587U * green + 114U * blue;
    return static_cast<std::uint8_t>(value / 1000U);
}

[[nodiscard]] ScanFrame grayscale_fallback(const ScanFrame& frame) {
    validate_frame(frame);
    if (frame.format == ScanPixelFormat::gray8) {
        return frame;
    }

    ScanFrame out;
    out.width = frame.width;
    out.height = frame.height;
    out.dpi = frame.dpi;
    out.format = ScanPixelFormat::gray8;
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));

    for (std::size_t i = 0, pixel = 0; pixel < out.pixels.size(); i += 3U, ++pixel) {
        out.pixels[pixel] = luminance(frame.pixels[i], frame.pixels[i + 1U], frame.pixels[i + 2U]);
    }
    return out;
}

#ifdef DOCSUITE_HAVE_OPENCV
[[nodiscard]] cv::Mat to_mat(const ScanFrame& frame) {
    validate_frame(frame);
    const int type = frame.format == ScanPixelFormat::rgb24 ? CV_8UC3 : CV_8UC1;
    const std::size_t step = static_cast<std::size_t>(frame.width) *
        (frame.format == ScanPixelFormat::rgb24 ? 3U : 1U);
    return cv::Mat(
        frame.height,
        frame.width,
        type,
        const_cast<std::uint8_t*>(frame.pixels.data()),
        step).clone();
}

[[nodiscard]] ScanFrame from_mat(const cv::Mat& input, const int dpi) {
    cv::Mat source = input;
    if (!source.isContinuous()) {
        source = source.clone();
    }

    ScanFrame out;
    out.width = source.cols;
    out.height = source.rows;
    out.dpi = dpi;
    out.format = source.channels() == 1 ? ScanPixelFormat::gray8 : ScanPixelFormat::rgb24;
    const std::size_t bytes = source.total() * source.elemSize();
    out.pixels.assign(source.data, source.data + bytes);
    return out;
}

[[nodiscard]] cv::Mat gray_mat(const ScanFrame& frame) {
    cv::Mat source = to_mat(frame);
    if (source.channels() == 1) {
        return source;
    }
    cv::Mat gray;
    cv::cvtColor(source, gray, cv::COLOR_RGB2GRAY);
    return gray;
}
#endif

} // namespace

bool ImageProcessor::deskew_available() const noexcept {
#ifdef DOCSUITE_HAVE_OPENCV
    return true;
#else
    return false;
#endif
}

ScanFrame ImageProcessor::grayscale(const ScanFrame& frame) const {
#ifdef DOCSUITE_HAVE_OPENCV
    return from_mat(gray_mat(frame), frame.dpi);
#else
    return grayscale_fallback(frame);
#endif
}

ScanFrame ImageProcessor::enhance_document(const ScanFrame& frame, const bool binarize) const {
#ifdef DOCSUITE_HAVE_OPENCV
    cv::Mat gray = gray_mat(frame);
    cv::Mat enhanced;
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(gray, enhanced);
    if (binarize) {
        cv::adaptiveThreshold(
            enhanced,
            enhanced,
            255,
            cv::ADAPTIVE_THRESH_GAUSSIAN_C,
            cv::THRESH_BINARY,
            31,
            12.0);
    }
    return from_mat(enhanced, frame.dpi);
#else
    ScanFrame out = grayscale_fallback(frame);
    auto [low_it, high_it] = std::minmax_element(out.pixels.begin(), out.pixels.end());
    const int low = static_cast<int>(*low_it);
    const int high = static_cast<int>(*high_it);
    if (high <= low) {
        return out;
    }
    for (auto& value : out.pixels) {
        int stretched = (static_cast<int>(value) - low) * 255 / (high - low);
        stretched = std::clamp(stretched, 0, 255);
        if (binarize) {
            stretched = stretched >= 180 ? 255 : 0;
        }
        value = static_cast<std::uint8_t>(stretched);
    }
    return out;
#endif
}

ImageRect ImageProcessor::detect_content(const ScanFrame& frame) const {
    const ScanFrame gray = grayscale_fallback(frame);
    int min_x = gray.width;
    int min_y = gray.height;
    int max_x = -1;
    int max_y = -1;

    for (int y = 0; y < gray.height; ++y) {
        for (int x = 0; x < gray.width; ++x) {
            const auto value = gray.pixels[
                static_cast<std::size_t>(y) * static_cast<std::size_t>(gray.width) +
                static_cast<std::size_t>(x)];
            if (value < 245U) {
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }

    if (max_x < min_x || max_y < min_y) {
        return ImageRect{0, 0, frame.width, frame.height};
    }

    const int margin_x = std::max(8, frame.width / 100);
    const int margin_y = std::max(8, frame.height / 100);
    min_x = std::max(0, min_x - margin_x);
    min_y = std::max(0, min_y - margin_y);
    max_x = std::min(frame.width - 1, max_x + margin_x);
    max_y = std::min(frame.height - 1, max_y + margin_y);
    return ImageRect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
}

ScanFrame ImageProcessor::crop(const ScanFrame& frame, const ImageRect rect) const {
    validate_frame(frame);
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > frame.width || rect.y + rect.height > frame.height) {
        throw std::runtime_error("Crop rectangle is outside the scan frame");
    }

    const std::size_t channels = frame.format == ScanPixelFormat::rgb24 ? 3U : 1U;
    ScanFrame out;
    out.width = rect.width;
    out.height = rect.height;
    out.dpi = frame.dpi;
    out.format = frame.format;
    const std::size_t output_stride = static_cast<std::size_t>(out.width) * channels;
    const std::size_t input_stride = static_cast<std::size_t>(frame.width) * channels;
    out.pixels.resize(output_stride * static_cast<std::size_t>(out.height));

    for (int y = 0; y < out.height; ++y) {
        const std::size_t source_offset =
            (static_cast<std::size_t>(rect.y + y) * input_stride) +
            static_cast<std::size_t>(rect.x) * channels;
        const std::size_t target_offset = static_cast<std::size_t>(y) * output_stride;
        std::copy_n(
            frame.pixels.data() + source_offset,
            output_stride,
            out.pixels.data() + target_offset);
    }
    return out;
}

bool ImageProcessor::is_blank(const ScanFrame& frame, const double white_ratio) const {
    if (white_ratio <= 0.0 || white_ratio > 1.0) {
        throw std::runtime_error("Blank-page white ratio must be within (0, 1]");
    }
    const ScanFrame gray = grayscale_fallback(frame);
    const auto white = std::count_if(gray.pixels.begin(), gray.pixels.end(), [](const std::uint8_t value) {
        return value >= 245U;
    });
    const double ratio = static_cast<double>(white) / static_cast<double>(gray.pixels.size());
    return ratio >= white_ratio;
}

ScanFrame ImageProcessor::deskew(const ScanFrame& frame) const {
#ifndef DOCSUITE_HAVE_OPENCV
    throw std::runtime_error("Deskew requires an OpenCV-enabled DocSuite build");
#else
    cv::Mat source = to_mat(frame);
    cv::Mat gray;
    if (source.channels() == 1) {
        gray = source;
    } else {
        cv::cvtColor(source, gray, cv::COLOR_RGB2GRAY);
    }

    cv::Mat binary;
    cv::threshold(gray, binary, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    std::vector<cv::Point> points;
    cv::findNonZero(binary, points);
    if (points.size() < 100U) {
        return frame;
    }

    const cv::RotatedRect box = cv::minAreaRect(points);
    double angle = static_cast<double>(box.angle);
    if (angle < -45.0) {
        angle = -(90.0 + angle);
    } else {
        angle = -angle;
    }

    if (std::abs(angle) < 0.15 || std::abs(angle) > 15.0) {
        return frame;
    }

    const cv::Point2f center(
        static_cast<float>(source.cols) / 2.0F,
        static_cast<float>(source.rows) / 2.0F);
    const cv::Mat matrix = cv::getRotationMatrix2D(center, angle, 1.0);
    cv::Mat rotated;
    const cv::Scalar border = source.channels() == 1
        ? cv::Scalar(255)
        : cv::Scalar(255, 255, 255);
    cv::warpAffine(
        source,
        rotated,
        matrix,
        source.size(),
        cv::INTER_CUBIC,
        cv::BORDER_CONSTANT,
        border);
    return from_mat(rotated, frame.dpi);
#endif
}

} // namespace docsuite
