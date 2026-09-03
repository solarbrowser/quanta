/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Math.h"
#include "quanta/core/runtime/TypedArray.h"
#include "../ast_internal.h"
#include <sstream>
#include <set>
#include <cmath>
#include <climits>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <cstdlib>
#include <cstdio>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

// Defined in member.cpp (GetSuperBase / super [[Set]]).
Object* resolve_super_base(Context&, Function*);
void super_set_on(Context&, Object*, const std::string&, const Value&);

static bool is_anonymous_function_def(const ASTNode* node);

// NamedEvaluation for a pattern default: an anonymous class has to carry its
// inferred name before it is evaluated, because a static initializer can read
// it. Everything else can be renamed afterwards, once the value exists.
static void stamp_pattern_default_class(ASTNode* rhs, const ASTNode* target) {
    if (!rhs || rhs->get_type() != ASTNode::Type::CLASS_DECLARATION) return;
    if (!target || target->get_type() != ASTNode::Type::IDENTIFIER) return;
    auto* cd = static_cast<ClassDeclaration*>(rhs);
    if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
        cd->set_inferred_name(static_cast<const Identifier*>(target)->get_name());
    }
}



// Helper: recursively perform destructuring assignment from an ObjectLiteral or ArrayLiteral pattern
// A getter run while destructuring throws into Object::current_context_, which
// is not the context we are binding in whenever the two differ -- inside a
// generator's own fiber context, notably. Adopting it here is what stops the
// rest loop below from spinning forever on a throwing `value` getter.
static void adopt_foreign_exception(Context& ctx) {
    if (!ctx.has_exception() && Object::current_context_ && Object::current_context_ != &ctx
            && Object::current_context_->has_exception()) {
        ctx.throw_exception(Object::current_context_->get_exception(), true);
        Object::current_context_->clear_exception();
    }
}

// CopyDataProperties for an object pattern's `...rest`: every own enumerable
// property of the source the pattern did not already take. Shared with
// Op::CopyRestProperties so both paths build the same object.
Value build_rest_object(Context& ctx, const Value& source_value, Object* source_obj,
                        const std::vector<std::string>& assigned_keys) {
auto rest_obj = ObjectFactory::create_object();
if (source_value.is_string()) {
    // For strings, create indexed char properties (spec 12.15.5.2).
    const std::string& raw = source_value.as_string()->str();
    uint32_t char_idx = 0;
    size_t pos = 0;
    while (pos < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[pos]);
        size_t cl = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (pos + cl > raw.size()) cl = 1;
        std::string char_key = std::to_string(char_idx);
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == char_key) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            PropertyDescriptor cdesc(Value(raw.substr(pos, cl)),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(char_key, cdesc);
        }
        pos += cl;
        char_idx++;
    }
} else if (!source_obj) {
    // A number, boolean or symbol source boxes to a wrapper with no own
    // enumerable properties of its own, so the rest object is simply empty.
    // Only null and undefined are an error, and CheckObjectCoercible has
    // already refused those before this runs.
} else if (source_obj->get_type() == Object::ObjectType::Proxy) {
    // get_enumerable_keys()/get_property() don't know about Proxy traps, so go through ownKeys/getOwnPropertyDescriptor/get directly per spec.
    Proxy* proxy = static_cast<Proxy*>(source_obj);
    for (const auto& k : proxy->own_keys_trap()) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (already_assigned) continue;
        // own_keys_trap() returns symbol keys as their "@@sym:" string encoding; decode back to the real Symbol so traps receive the original key, not its string form.
        Symbol* sym = Symbol::find_by_property_key(k);
        Value key_value = sym ? Value(sym) : Value(k);
        PropertyDescriptor kdesc = proxy->get_own_property_descriptor_trap(key_value);
        if (!kdesc.is_data_descriptor() && !kdesc.is_accessor_descriptor()) continue;
        if (!kdesc.is_enumerable()) continue;
        Value val = proxy->get_trap(key_value);
        if (ctx.has_exception()) return Value();
        PropertyDescriptor rdesc(val,
            static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        rest_obj->set_property_descriptor(k, rdesc);
    }
} else {
    // For objects: use enumerable keys only (spec excludes non-enumerable).
    auto keys = source_obj->get_enumerable_keys();
    for (const auto& k : keys) {
        bool already_assigned = false;
        for (const auto& ak : assigned_keys) {
            if (ak == k) { already_assigned = true; break; }
        }
        if (!already_assigned) {
            Value val = source_obj->get_property(k);
            if (ctx.has_exception()) return Value();
            PropertyDescriptor rdesc(val,
                static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            rest_obj->set_property_descriptor(k, rdesc);
        }
    }
}
    return Value(rest_obj.release());
}


// Helper: assign a value to a target node (Identifier, MemberExpression, or nested pattern)


std::string AssignmentExpression::to_string() const {
    std::string op_str;
    switch (operator_) {
        case Operator::ASSIGN: op_str = " = "; break;
        case Operator::PLUS_ASSIGN: op_str = " += "; break;
        case Operator::MINUS_ASSIGN: op_str = " -= "; break;
        case Operator::MUL_ASSIGN: op_str = " *= "; break;
        case Operator::DIV_ASSIGN: op_str = " /= "; break;
        case Operator::MOD_ASSIGN: op_str = " %= "; break;
        case Operator::LOGICAL_AND_ASSIGN: op_str = " &&= "; break;
        case Operator::LOGICAL_OR_ASSIGN: op_str = " ||= "; break;
        case Operator::NULLISH_ASSIGN: op_str = " ??= "; break;
        default: op_str = " op= "; break;
    }
    return left_->to_string() + op_str + right_->to_string();
}

std::unique_ptr<ASTNode> AssignmentExpression::clone() const {
    return std::make_unique<AssignmentExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_, lhs_is_paren_
    );
}


static bool is_anonymous_function_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}

// Walks the pattern literal, which is the same shape AssignmentExpression's
// own destructuring walks: an ObjectLiteral property's value is the target
// (an Identifier, a nested literal, or an AssignmentExpression carrying a
// default), an ArrayLiteral element likewise, with SpreadElement for rest.
static void walk_pattern_targets(const ASTNode* pattern,
                                 const std::function<void(const ASTNode*)>& on_target,
                                 const std::function<void(const ASTNode*)>& on_expression) {
    if (!pattern) return;
    auto visit_target = [&](const ASTNode* t) {
        if (!t) return;
        if (t->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            const auto* ae = static_cast<const AssignmentExpression*>(t);
            if (on_expression && ae->get_right()) on_expression(ae->get_right());
            walk_pattern_targets(ae->get_left(), on_target, on_expression);
            if (ae->get_left() && ae->get_left()->get_type() != ASTNode::Type::OBJECT_LITERAL &&
                ae->get_left()->get_type() != ASTNode::Type::ARRAY_LITERAL) {
                on_target(ae->get_left());
            }
            return;
        }
        if (t->get_type() == ASTNode::Type::OBJECT_LITERAL ||
            t->get_type() == ASTNode::Type::ARRAY_LITERAL) {
            walk_pattern_targets(t, on_target, on_expression);
            return;
        }
        if (t->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            walk_pattern_targets(static_cast<const SpreadElement*>(t)->get_argument(),
                                 on_target, on_expression);
            on_target(static_cast<const SpreadElement*>(t)->get_argument());
            return;
        }
        on_target(t);
    };

    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        for (const auto& prop : static_cast<const ObjectLiteral*>(pattern)->get_properties()) {
            if (prop->computed && prop->key && on_expression) on_expression(prop->key.get());
            visit_target(prop->value ? prop->value.get() : nullptr);
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        for (const auto& el : static_cast<const ArrayLiteral*>(pattern)->get_elements()) {
            if (el) visit_target(el.get());
        }
    }
}

void DestructuringAssignment::collect_bound_names(std::vector<std::string>& out) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            if (t && t->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<const Identifier*>(t)->get_name();
                if (!n.empty()) out.push_back(n);
            }
        },
        nullptr);
}

void DestructuringAssignment::for_each_expression(const std::function<void(const ASTNode*)>& fn) const {
    walk_pattern_targets(pattern_literal_.get(),
        [&](const ASTNode* t) {
            // A member-expression target (`[o.x] = v`) is itself an expression.
            if (t && t->get_type() != ASTNode::Type::IDENTIFIER) fn(t);
        },
        fn);
}




std::string DestructuringAssignment::to_string() const {
    return pattern_literal_ ? pattern_literal_->to_string() : std::string("[destructuring]");
}

std::unique_ptr<ASTNode> DestructuringAssignment::clone() const {
    auto cloned = std::make_unique<DestructuringAssignment>(
        std::vector<std::unique_ptr<Identifier>>{}, source_ ? source_->clone() : nullptr,
        type_, start_, end_);
    if (pattern_literal_) cloned->set_pattern_literal(pattern_literal_->clone());
    return cloned;
}


} // namespace Quanta
