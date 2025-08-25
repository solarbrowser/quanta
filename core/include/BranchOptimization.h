/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_BRANCH_OPTIMIZATION_H
#define QUANTA_BRANCH_OPTIMIZATION_H

// High-performance branch prediction hints
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Force inline for critical hot paths
#define FORCE_INLINE __attribute__((always_inline)) inline

// Branch optimization macros for common patterns
#define FAST_BRANCH_TRUE(condition) if (LIKELY(condition))
#define FAST_BRANCH_FALSE(condition) if (UNLIKELY(condition))

// Memory prefetch hints for optimized access
#define PREFETCH_READ(addr)  __builtin_prefetch((addr), 0, 3)
#define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)

// Cache line alignment for critical structures
#define CACHE_ALIGNED __attribute__((aligned(64)))

namespace Quanta {

// High-performance branch prediction statistics
class BranchOptimization {
public:
    static void optimize_hot_paths();
    static void enable_aggressive_prefetching();
    static void configure_branch_prediction();
    
private:
    static uint64_t branch_hits_;
    static uint64_t branch_misses_;
};

} // namespace Quanta

#endif // QUANTA_BRANCH_OPTIMIZATION_H