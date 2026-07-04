/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_FIBERSTATE_H
#define QUANTA_FIBERSTATE_H

#include <ucontext.h>

namespace Quanta {

// The ucontext pair of a fiber, kept out of the owning GC cell: two
// ucontext_t's are ~2KB and would bloat Generator/AsyncGenerator cells from
// ~300B to a 2560B size class. Plain-malloc backing store, owned via
// unique_ptr. Also gives the conservative root scanner one fixed spot
// for the saved register area.
struct FiberState {
    ucontext_t fiber_ctx;
    ucontext_t caller_ctx;
};

}

#endif
