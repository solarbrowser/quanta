/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "quanta/core/engine/Context.h"

namespace Quanta {

void register_regexp_builtins(Context& ctx);

// obj[Symbol.match|matchAll|replace|search|split] (which: 0..4) without a lookup by name when obj is an ordinary pattern.
Value regexp_symbol_method(Object* obj, int which, const char* key);

} // namespace Quanta
