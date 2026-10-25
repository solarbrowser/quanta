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
#include <pthread.h>
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
#elif defined(__APPLE__)
    // The stack of the thread asking, which a program may have made larger (or smaller) than the process limit.
    const size_t own = pthread_get_stacksize_np(pthread_self());
    if (own >= 1u * 1024 * 1024) return own;
    return 8u * 1024 * 1024;
#else
#if defined(__linux__)
    {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* address = nullptr;
            size_t size = 0;
            const int got = pthread_attr_getstack(&attr, &address, &size);
            pthread_attr_destroy(&attr);
            if (got == 0 && size >= 1u * 1024 * 1024) return size;
        }
    }
#endif
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur >= 1u * 1024 * 1024) {
        return static_cast<size_t>(rl.rlim_cur);
    }
    return 8u * 1024 * 1024;
#endif
}

}  // namespace Quanta
