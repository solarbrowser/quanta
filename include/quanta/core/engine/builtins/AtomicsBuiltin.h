/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "quanta/core/engine/Context.h"

namespace Quanta {

class Visitor;
// GC roots: pending waitAsync promises/buffers live only in this registry.
void trace_atomics_gc_roots(Visitor& v);

void register_atomics_builtins(Context& ctx);

} // namespace Quanta
