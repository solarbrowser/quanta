/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
#include <string>
#include <cstddef>
#include <unordered_map>

namespace Quanta {

class Value;
class Context;
class Object;
extern std::unordered_map<std::string, Value> g_object_function_map;

bool private_brand_check(Context& ctx, Object* obj, const std::string& prop_name);

inline size_t utf16_length(const std::string& str) {
    size_t count = 0;
    for (size_t i = 0; i < str.size(); ) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 0xF0) { count += 2; i += 4; }
        else if (c >= 0xE0) { count += 1; i += 3; }
        else if (c >= 0xC0) { count += 1; i += 2; }
        else { count += 1; i += 1; }
    }
    return count;
}

void increment_loop_depth();
void decrement_loop_depth();

struct LoopDepthGuard {
    LoopDepthGuard() { increment_loop_depth(); }
    ~LoopDepthGuard() { decrement_loop_depth(); }
};

} // namespace Quanta
