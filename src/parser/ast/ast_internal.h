/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
#include <string>
#include <cstddef>
#include <unordered_map>
#include "quanta/core/runtime/String.h"

namespace Quanta {

class Value;
class Context;
class Object;
extern std::unordered_map<std::string, Value> g_object_function_map;

bool private_brand_check(Context& ctx, Object* obj, const std::string& prop_name, bool require_exists = true);

void increment_loop_depth();
void decrement_loop_depth();

struct LoopDepthGuard {
    LoopDepthGuard() { increment_loop_depth(); }
    ~LoopDepthGuard() { decrement_loop_depth(); }
};

} // namespace Quanta
