/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_FIBERSTACKPOOL_H
#define QUANTA_RUNTIME_FIBERSTACKPOOL_H

#include <cstddef>
#include <vector>

namespace Quanta {

// Reuses fiber stacks instead of allocating -- and above all value-
// initializing: a 2MB memset per async call dominated call-heavy async
// profiles -- a fresh buffer per Generator/AsyncExecutor/AsyncGenerator.
// Returned memory is uninitialized or holds a dead fiber's remains. That is
// safe: the conservative scanner validates every candidate word against the
// heap before treating it as a cell, so stale words cost at most temporary
// false retention.
class FiberStackPool {
public:
    static char* acquire(size_t size);
    static void release(char* p, size_t size);

private:
    struct Bucket { size_t size; std::vector<char*> free; };
    static thread_local std::vector<Bucket> buckets_;
    static constexpr size_t kMaxPerBucket = 8;
};

}

#endif
