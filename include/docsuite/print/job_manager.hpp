// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/core/types.hpp"
#include "docsuite/print/cups_print_backend.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace docsuite {

class JobManager {
public:
    explicit JobManager(const CupsPrintBackend& backend) noexcept;

    [[nodiscard]] std::vector<PrintJobInfo> list_jobs(
        const std::string& printer,
        bool include_completed = true) const;

    [[nodiscard]] std::optional<PrintJobInfo> job(
        const std::string& printer,
        int job_id) const;

    [[nodiscard]] PrintJobTrace diagnose_print(
        const std::string& printer,
        const std::string& path,
        const std::string& title,
        const PrintProfile& profile,
        std::chrono::seconds timeout = std::chrono::seconds{180},
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds{200}) const;

    [[nodiscard]] bool cancel(const std::string& printer, int job_id) const;

private:
    const CupsPrintBackend& backend_;
};

[[nodiscard]] const char* print_job_state_name(PrintJobState state) noexcept;

} // namespace docsuite
