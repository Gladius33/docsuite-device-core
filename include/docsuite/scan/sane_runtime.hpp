// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <mutex>

namespace docsuite::detail {

// The SANE C API and several third-party backends (notably Canon BJNP)
// maintain process-global state. Calling sane_init/sane_exit or backend
// discovery concurrently from QtConcurrent workers can corrupt that state.
//
// Keep one process-wide lock for every SANE entry point. This lives in an
// inline function so all translation units share the same mutex instance.
[[nodiscard]] inline std::mutex& sane_global_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace docsuite::detail
