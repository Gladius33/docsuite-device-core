// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "docsuite/print/job_manager.hpp"

#include <cups/cups.h>
#include <cups/ipp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace docsuite {
namespace {

constexpr std::size_t kRecentTerminalJobLimit = 10U;
constexpr auto kRecentTerminalJobAge = std::chrono::hours{24};

[[nodiscard]] PrintJobState from_ipp_state(const ipp_jstate_t state) noexcept {
    switch (state) {
        case IPP_JSTATE_PENDING: return PrintJobState::pending;
        case IPP_JSTATE_HELD: return PrintJobState::held;
        case IPP_JSTATE_PROCESSING: return PrintJobState::processing;
        case IPP_JSTATE_STOPPED: return PrintJobState::stopped;
        case IPP_JSTATE_CANCELED: return PrintJobState::canceled;
        case IPP_JSTATE_ABORTED: return PrintJobState::aborted;
        case IPP_JSTATE_COMPLETED: return PrintJobState::completed;
        default: return PrintJobState::unknown;
    }
}

[[nodiscard]] bool terminal(const PrintJobState state) noexcept {
    return state == PrintJobState::completed ||
        state == PrintJobState::canceled ||
        state == PrintJobState::aborted ||
        state == PrintJobState::stopped;
}

[[nodiscard]] std::chrono::system_clock::time_point from_time_t(const std::time_t value) {
    if (value <= 0) {
        return {};
    }
    return std::chrono::system_clock::from_time_t(value);
}

[[nodiscard]] PrintJobInfo convert_job(const cups_job_t& job) {
    PrintJobInfo result;
    result.id = job.id;
    result.printer = job.dest != nullptr ? job.dest : "";
    result.title = job.title != nullptr ? job.title : "";
    result.user = job.user != nullptr ? job.user : "";
    result.format = job.format != nullptr ? job.format : "";
    result.state = from_ipp_state(job.state);
    result.size_kib = job.size;
    result.priority = job.priority;
    result.created_at = from_time_t(job.creation_time);
    result.processing_at = from_time_t(job.processing_time);
    result.completed_at = from_time_t(job.completed_time);
    return result;
}

[[nodiscard]] std::string json_escape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 16U);
    for (const unsigned char ch : input) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch >= 0x20U) {
                    output.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return output;
}

[[nodiscard]] long long epoch_ms(const std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

[[nodiscard]] std::filesystem::path history_file_path() {
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return std::filesystem::path{state_home} /
            "docsuite-device-core" / "print-history.jsonl";
    }

    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} /
            ".local" / "state" / "docsuite-device-core" / "print-history.jsonl";
    }

    return std::filesystem::temp_directory_path() /
        "docsuite-device-core-print-history.jsonl";
}

void persist_trace(PrintJobTrace& trace) {
    const auto path = history_file_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return;
    }

    std::ofstream output(path, std::ios::app);
    if (!output) {
        return;
    }

    output << '{'
           << "\"job_id\":" << trace.job_id << ','
           << "\"printer\":\"" << json_escape(trace.printer) << "\","
           << "\"title\":\"" << json_escape(trace.title) << "\","
           << "\"final_state\":\"" << print_job_state_name(trace.final_state) << "\","
           << "\"submit_to_accept_ms\":" << trace.submit_to_accept.count() << ','
           << "\"timed_out\":" << (trace.timed_out ? "true" : "false") << ',';

    if (trace.queue_delay.has_value()) {
        output << "\"queue_delay_ms\":" << trace.queue_delay->count() << ',';
    } else {
        output << "\"queue_delay_ms\":null,";
    }

    if (trace.processing_duration.has_value()) {
        output << "\"processing_duration_ms\":" << trace.processing_duration->count() << ',';
    } else {
        output << "\"processing_duration_ms\":null,";
    }

    if (trace.total_duration.has_value()) {
        output << "\"total_duration_ms\":" << trace.total_duration->count() << ',';
    } else {
        output << "\"total_duration_ms\":null,";
    }

    output << "\"events\":[";
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
        const auto& event = trace.events[i];
        if (i != 0U) {
            output << ',';
        }
        output << '{'
               << "\"name\":\"" << json_escape(event.name) << "\","
               << "\"state\":\"" << print_job_state_name(event.state) << "\","
               << "\"at_ms\":" << epoch_ms(event.at) << ','
               << "\"since_submit_ms\":" << event.since_submit.count()
               << '}';
    }
    output << "]}\n";

    if (output) {
        trace.history_path = path.string();
    }
}

[[nodiscard]] std::optional<std::chrono::milliseconds> duration_between(
    const std::chrono::system_clock::time_point start,
    const std::chrono::system_clock::time_point end) {
    if (start.time_since_epoch().count() == 0 || end.time_since_epoch().count() == 0 || end < start) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
}

} // namespace

const char* print_job_state_name(const PrintJobState state) noexcept {
    switch (state) {
        case PrintJobState::pending: return "pending";
        case PrintJobState::held: return "held";
        case PrintJobState::processing: return "processing";
        case PrintJobState::stopped: return "stopped";
        case PrintJobState::canceled: return "canceled";
        case PrintJobState::aborted: return "aborted";
        case PrintJobState::completed: return "completed";
        case PrintJobState::unknown: return "unknown";
    }
    return "unknown";
}

JobManager::JobManager(const CupsPrintBackend& backend) noexcept
    : backend_(backend) {}

std::vector<PrintJobInfo> JobManager::list_jobs(
    const std::string& printer,
    const bool include_completed) const {

    cups_job_t* jobs = nullptr;
    const int which = include_completed ? CUPS_WHICHJOBS_ALL : CUPS_WHICHJOBS_ACTIVE;
    const int count = cupsGetJobs2(
        CUPS_HTTP_DEFAULT,
        &jobs,
        printer.empty() ? nullptr : printer.c_str(),
        0,
        which);

    if (count < 0) {
        throw std::runtime_error(cupsLastErrorString());
    }

    std::vector<PrintJobInfo> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        result.push_back(convert_job(jobs[i]));
    }

    cupsFreeJobs(count, jobs);

    // CUPS can retain completed jobs for a long time. Surface the useful
    // working set only: all active jobs plus at most ten terminal jobs from
    // the last 24 hours. Full DocSuite timing history remains in JSONL.
    std::sort(result.begin(), result.end(), [](const PrintJobInfo& left, const PrintJobInfo& right) {
        return left.id > right.id;
    });

    if (!include_completed) {
        return result;
    }

    const auto cutoff = std::chrono::system_clock::now() - kRecentTerminalJobAge;
    std::vector<PrintJobInfo> visible;
    visible.reserve(result.size());
    std::size_t terminal_count = 0;
    for (auto& candidate : result) {
        if (terminal(candidate.state)) {
            const auto terminal_time =
                candidate.completed_at.time_since_epoch().count() != 0
                    ? candidate.completed_at
                    : candidate.created_at;
            if (terminal_time.time_since_epoch().count() == 0 || terminal_time < cutoff ||
                terminal_count >= kRecentTerminalJobLimit) {
                continue;
            }
            ++terminal_count;
        }
        visible.push_back(std::move(candidate));
    }
    return visible;
}

std::optional<PrintJobInfo> JobManager::job(
    const std::string& printer,
    const int job_id) const {

    const auto jobs = list_jobs(printer, true);
    for (const auto& candidate : jobs) {
        if (candidate.id == job_id) {
            return candidate;
        }
    }
    return std::nullopt;
}

PrintJobTrace JobManager::diagnose_print(
    const std::string& printer,
    const std::string& path,
    const std::string& title,
    const PrintProfile& profile,
    const std::chrono::seconds timeout,
    const std::chrono::milliseconds poll_interval) const {

    PrintJobTrace trace;
    trace.printer = printer;
    trace.title = title;

    const auto submit_system = std::chrono::system_clock::now();
    const auto submit_steady = std::chrono::steady_clock::now();
    trace.events.push_back(JobTimelineEvent{
        .name = "submit-start",
        .state = PrintJobState::unknown,
        .at = submit_system,
        .since_submit = std::chrono::milliseconds{0},
    });

    trace.job_id = backend_.print_file_advanced(printer, path, title, profile);

    const auto accepted_system = std::chrono::system_clock::now();
    const auto accepted_steady = std::chrono::steady_clock::now();
    trace.submit_to_accept = std::chrono::duration_cast<std::chrono::milliseconds>(
        accepted_steady - submit_steady);
    trace.events.push_back(JobTimelineEvent{
        .name = "cups-accepted",
        .state = PrintJobState::pending,
        .at = accepted_system,
        .since_submit = trace.submit_to_accept,
    });

    PrintJobState last_state = PrintJobState::unknown;
    std::optional<PrintJobInfo> final_job;

    while (std::chrono::steady_clock::now() - submit_steady <= timeout) {
        const auto current = job(printer, trace.job_id);
        if (current.has_value()) {
            final_job = current;
            if (current->state != last_state) {
                last_state = current->state;
                const auto now_system = std::chrono::system_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - submit_steady);
                trace.events.push_back(JobTimelineEvent{
                    .name = std::string{"cups-"} + print_job_state_name(current->state),
                    .state = current->state,
                    .at = now_system,
                    .since_submit = elapsed,
                });
            }

            if (terminal(current->state)) {
                break;
            }
        }

        std::this_thread::sleep_for(poll_interval);
    }

    if (!final_job.has_value() || !terminal(final_job->state)) {
        trace.timed_out = true;
        trace.final_state = final_job.has_value() ? final_job->state : PrintJobState::unknown;
        trace.events.push_back(JobTimelineEvent{
            .name = "diagnostic-timeout",
            .state = trace.final_state,
            .at = std::chrono::system_clock::now(),
            .since_submit = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - submit_steady),
        });
    } else {
        trace.final_state = final_job->state;
        trace.queue_delay = duration_between(final_job->created_at, final_job->processing_at);
        trace.processing_duration = duration_between(
            final_job->processing_at, final_job->completed_at);
        trace.total_duration = duration_between(final_job->created_at, final_job->completed_at);
    }

    persist_trace(trace);
    return trace;
}

bool JobManager::cancel(const std::string& printer, const int job_id) const {
    const ipp_status_t status = cupsCancelJob2(
        CUPS_HTTP_DEFAULT,
        printer.c_str(),
        job_id,
        0);
    return status < IPP_STATUS_ERROR_BAD_REQUEST;
}

} // namespace docsuite
