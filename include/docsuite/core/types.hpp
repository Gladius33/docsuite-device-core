// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace docsuite {

enum class DeviceState {
    unknown,
    idle,
    processing,
    stopped,
    offline
};

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

} // namespace docsuite
