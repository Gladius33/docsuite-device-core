// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace docsuite::detail {

// SANE itself and several third-party backends (notably Canon BJNP) keep
// process-global/socket state. QtConcurrent callers therefore must never run
// SANE discovery/capability/acquisition operations concurrently.
[[nodiscard]] inline std::mutex& sane_global_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] inline std::string sane_lock_path() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] != '\0') {
        return std::string{runtime} + "/docsuite-sane.lock";
    }
    return "/tmp/docsuite-sane-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".lock";
}

// Serializes SANE both inside one process and between the persistent DocSuite
// service and the desktop GUI. The file lock is intentionally held for the
// complete SANE operation, including an actual scan, because backend state is
// not reliably re-entrant.
class SaneRuntimeGuard final {
public:
    SaneRuntimeGuard()
        : thread_lock_{sane_global_mutex()} {

        const std::string path = sane_lock_path();
        int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd_ = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            throw std::runtime_error(
                "Unable to open DocSuite SANE lock '" + path + "': " + std::strerror(errno));
        }

        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string message =
                "Unable to acquire DocSuite SANE lock '" + path + "': " + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(message);
        }
    }

    ~SaneRuntimeGuard() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
        }
    }

    SaneRuntimeGuard(const SaneRuntimeGuard&) = delete;
    SaneRuntimeGuard& operator=(const SaneRuntimeGuard&) = delete;
    SaneRuntimeGuard(SaneRuntimeGuard&&) = delete;
    SaneRuntimeGuard& operator=(SaneRuntimeGuard&&) = delete;

private:
    std::unique_lock<std::mutex> thread_lock_;
    int fd_{-1};
};

} // namespace docsuite::detail
