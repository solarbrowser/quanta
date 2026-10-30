/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_STABLESORT_H
#define QUANTA_RUNTIME_STABLESORT_H

#include <cstddef>
#include <utility>
#include <vector>

namespace Quanta {

// A stable sort that stays inside its buffers whatever `less` answers. std::stable_sort assumes a strict weak ordering and
// runs off the ends of its buffer when it is not one, and a script's comparator (Math.random() - 0.5, a comparison that
// is not transitive) is anything but promised to be.
template <typename T, typename Less>
void safe_stable_sort(std::vector<T>& v, Less&& less) {
    const size_t n = v.size();
    if (n < 2) return;
    std::vector<T> buffer(n);
    // Bottom-up: runs of `width` are merged in pairs, from `v` into `buffer` and back.
    std::vector<T>* from = &v;
    std::vector<T>* to = &buffer;
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            const size_t mid = lo + width < n ? lo + width : n;
            const size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                // The right one goes first only when it is strictly smaller: equal ones keep their order.
                if (less((*from)[j], (*from)[i])) (*to)[k++] = (*from)[j++];
                else (*to)[k++] = (*from)[i++];
            }
            while (i < mid) (*to)[k++] = (*from)[i++];
            while (j < hi) (*to)[k++] = (*from)[j++];
        }
        std::swap(from, to);
    }
    if (from != &v) v = *from;
}

}  // namespace Quanta

#endif
