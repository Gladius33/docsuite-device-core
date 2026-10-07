// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace docsuite {

enum class DeviceState { unknown, idle, processing, stopped, offline };
enum class PrintJobState { unknown, pending, held, processing, stopped, canceled, aborted, completed };
enum class ScanPixelFormat { gray8, rgb24 };

struct PrinterInfo {
    std::string name;
    std::string uri;
    std::string model;
    bool is_default{false};
    bool temporary{false};
};

struct ScannerInfo {
    std::string name;
    std::string vendor;
    std::string model;
    std::string type;
    std::string backend;
};

struct SupplyLevel {
    std::string name;
    std::string type;
    std::optional<int> percent;
    int low_threshold{15};
};

struct PrinterCapabilities {
    std::string printer;
    std::string source{"cups"};
    std::vector<std::string> color_modes;
    std::vector<std::string> media;
    std::vector<std::string> media_types;
    std::vector<std::string> media_sources;
    std::vector<std::string> sides;
    std::vector<int> qualities;
    std::vector<int> resolutions_dpi;
    std::vector<std::string> document_formats;
    int copies_min{1};
    int copies_max{1};
    std::chrono::system_clock::time_point fetched_at{};
};

struct PrinterStatus {
    std::string printer;
    std::string source{"cups"};
    DeviceState state{DeviceState::unknown};
    std::vector<std::string> reasons;
    std::vector<SupplyLevel> supplies;
    bool accepting_jobs{false};
};

struct DeviceSnapshot {
    std::vector<PrinterInfo> printers;
    std::vector<ScannerInfo> scanners;
};

struct PrintProfile {
    std::string name;
    std::string media{"iso_a4_210x297mm"};
    std::string color_mode{"color"};
    std::string sides{"one-sided"};
    int quality{4};
};

struct PrintJobInfo {
    int id{0};
    std::string printer;
    std::string title;
    std::string user;
    std::string format;
    PrintJobState state{PrintJobState::unknown};
    int size_kib{0};
    int priority{0};
    std::chrono::system_clock::time_point created_at{};
    std::chrono::system_clock::time_point processing_at{};
    std::chrono::system_clock::time_point completed_at{};
};

struct JobTimelineEvent {
    std::string name;
    PrintJobState state{PrintJobState::unknown};
    std::chrono::system_clock::time_point at{};
    std::chrono::milliseconds since_submit{0};
};

struct PrintJobTrace {
    int job_id{0};
    std::string printer;
    std::string title;
    std::vector<JobTimelineEvent> events;
    PrintJobState final_state{PrintJobState::unknown};
    std::chrono::milliseconds submit_to_accept{0};
    std::optional<std::chrono::milliseconds> queue_delay;
    std::optional<std::chrono::milliseconds> processing_duration;
    std::optional<std::chrono::milliseconds> total_duration;
    bool timed_out{false};
    std::string history_path;
};

struct ScanSettings {
    int dpi{300};
    std::string mode{"Color"};
    std::string source{"Flatbed"};
};

struct ScanFrame {
    int width{0};
    int height{0};
    int dpi{0};
    ScanPixelFormat format{ScanPixelFormat::rgb24};
    std::vector<std::uint8_t> pixels;
};

} // namespace docsuite
