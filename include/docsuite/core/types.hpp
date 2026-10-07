// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

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
};

struct ScannerInfo {
    std::string name;
    std::string vendor;
    std::string model;
    std::string type;
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
