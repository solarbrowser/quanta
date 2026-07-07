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
#include "quanta/parser/AST.h"

namespace Quanta {

class Value;
class Context;
class Object;
extern std::unordered_map<std::string, Value> g_object_function_map;

inline bool is_chain_link_type(ASTNode::Type t) {
    return t == ASTNode::Type::MEMBER_EXPRESSION || t == ASTNode::Type::CALL_EXPRESSION ||
           t == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION;
}

bool private_brand_check(Context& ctx, Object* obj, const std::string& prop_name, bool require_exists = true);

// Set only when a chain link just returned undefined because its OWN base
// was nullish (a genuine `?.` short-circuit). The next link up consumes it
// to decide whether to propagate the short-circuit or throw normally for its
// own null/undefined base (e.g. `obj?.p.q` must still throw at `.q` if `obj.p`
// is a real null). Cleared before evaluating a new chain's first base.
extern thread_local bool g_optional_chain_shortcircuit;

void increment_loop_depth();
void decrement_loop_depth();

struct LoopDepthGuard {
    LoopDepthGuard() { increment_loop_depth(); }
    ~LoopDepthGuard() { decrement_loop_depth(); }
};

} // namespace Quanta
