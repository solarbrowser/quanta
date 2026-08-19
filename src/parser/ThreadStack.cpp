/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/ThreadStack.h"

#ifdef _WIN32
// GetCurrentThreadStackLimits is Windows 8; say so before windows.h in case
// the toolchain's SDK would otherwise hide the declaration.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace Quanta {

size_t thread_stack_bytes() {
#ifdef _WIN32
    // The span the loader reserved for this thread, which is what the link's
    // /STACK setting produced rather than any process-wide default.
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (high > low) return static_cast<size_t>(high - low);
    return 1u * 1024 * 1024;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur >= 1u * 1024 * 1024) {
        return static_cast<size_t>(rl.rlim_cur);
    }
    return 8u * 1024 * 1024;
#endif
}

}  // namespace Quanta
