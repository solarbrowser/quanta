/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "quanta/core/engine/Context.h"
namespace Quanta {
void register_object_builtins(Context& ctx);
// ToObject: throws for null/undefined, boxes other primitives, passes objects/functions through.
Object* to_object_or_throw(Context& ctx, const Value& value);
// %Object.prototype.toString% body.
// Exposed so other builtins can call the real intrinsic even if the live property was deleted.
Value object_prototype_to_string(Context& ctx, const Value& this_val);
}
