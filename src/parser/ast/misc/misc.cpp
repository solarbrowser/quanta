/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/vm/Interpreter.h"
#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/String.h"
#include "../ast_internal.h"
#include <algorithm>

namespace Quanta {

thread_local bool g_optional_chain_shortcircuit = false;



std::string ConditionalExpression::to_string() const {
    return test_->to_string() + " ? " + consequent_->to_string() + " : " + alternate_->to_string();
}

std::unique_ptr<ASTNode> ConditionalExpression::clone() const {
    return std::make_unique<ConditionalExpression>(
        test_->clone(),
        consequent_->clone(),
        alternate_->clone(),
        start_,
        end_
    );
}


// Backs both RegexLiteral::evaluate and Op::CreateRegExp, so a literal cannot
// mean one thing compiled and another interpreted.
Value create_regexp_literal(Context& ctx, const std::string& pattern, const std::string& flags) {
    // Regex literals share the RegExp constructor implementation so exec/test and
    // lastIndex semantics can never diverge between literals and new RegExp().
    Object* ctor = ctx.get_built_in_object("RegExp");
    if (!ctor || !ctor->is_function()) {
        ctx.throw_error("RegExp constructor is not available");
        return Value();
    }
    // Clear any enclosing new.target: a literal evaluated inside a constructor body
    // must not inherit that constructor's prototype.
    Value saved_new_target = ctx.get_new_target();
    ctx.set_new_target(Value());
    Value re = static_cast<Function*>(ctor)->construct(ctx, { Value(pattern), Value(flags) });
    ctx.set_new_target(saved_new_target);
    return re;
}



std::string RegexLiteral::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

std::unique_ptr<ASTNode> RegexLiteral::clone() const {
    return std::make_unique<RegexLiteral>(pattern_, flags_, start_, end_);
}




std::string SpreadElement::to_string() const {
    return "..." + argument_->to_string();
}

std::unique_ptr<ASTNode> SpreadElement::clone() const {
    return std::make_unique<SpreadElement>(argument_->clone(), start_, end_);
}




std::string JSXElement::to_string() const {
    std::string result = "<" + tag_name_;

    for (const auto& attr : attributes_) {
        result += " " + attr->to_string();
    }

    if (self_closing_) {
        result += " />";
    } else {
        result += ">";

        for (const auto& child : children_) {
            result += child->to_string();
        }

        result += "</" + tag_name_ + ">";
    }

    return result;
}

std::unique_ptr<ASTNode> JSXElement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_attrs;
    for (const auto& attr : attributes_) {
        cloned_attrs.push_back(attr->clone());
    }

    std::vector<std::unique_ptr<ASTNode>> cloned_children;
    for (const auto& child : children_) {
        cloned_children.push_back(child->clone());
    }

    return std::make_unique<JSXElement>(tag_name_, std::move(cloned_attrs),
                                        std::move(cloned_children), self_closing_, start_, end_);
}


// ES5: property access on a primitive (string/number/boolean) reads through that primitive's
// wrapper constructor's prototype (mirrors MemberExpression::evaluate's primitive-boxing branch).
static Value box_primitive_and_get_property(Context& ctx, const Value& object_value, const std::string& prop_name) {
    std::string ctor_name = object_value.is_string() ? "String" :
        (object_value.is_number() ? "Number" : "Boolean");
    Value ctor = ctx.get_binding(ctor_name);
    if (!ctor.is_object() && !ctor.is_function()) return Value();
    Object* ctor_obj = ctor.is_object() ? ctor.as_object() : ctor.as_function();
    Value prototype = ctor_obj->get_property("prototype");
    if (!prototype.is_object()) return Value();
    Object* proto_obj = prototype.as_object();

    PropertyDescriptor desc = proto_obj->get_property_descriptor(prop_name);
    if (desc.is_accessor_descriptor() && desc.has_getter()) {
        Function* getter = as_function(desc.get_getter());
        if (getter) return getter->call(ctx, {}, object_value);
    }
    return proto_obj->get_property(prop_name);
}



std::string OptionalChainingExpression::to_string() const {
    if (computed_) {
        return object_->to_string() + "?.[" + property_->to_string() + "]";
    } else {
        return object_->to_string() + "?." + property_->to_string();
    }
}

std::unique_ptr<ASTNode> OptionalChainingExpression::clone() const {
    return std::make_unique<OptionalChainingExpression>(
        object_->clone(), property_->clone(), computed_, start_, end_
    );
}




std::string NullishCoalescingExpression::to_string() const {
    return "(" + left_->to_string() + " ?? " + right_->to_string() + ")";
}

std::unique_ptr<ASTNode> NullishCoalescingExpression::clone() const {
    return std::make_unique<NullishCoalescingExpression>(
        left_->clone(), right_->clone(), start_, end_
    );
}



std::string JSXText::to_string() const {
    return text_;
}

std::unique_ptr<ASTNode> JSXText::clone() const {
    return std::make_unique<JSXText>(text_, start_, end_);
}



std::string JSXExpression::to_string() const {
    return "{" + expression_->to_string() + "}";
}

std::unique_ptr<ASTNode> JSXExpression::clone() const {
    return std::make_unique<JSXExpression>(expression_->clone(), start_, end_);
}



std::string JSXAttribute::to_string() const {
    if (value_) {
        return name_ + "=" + value_->to_string();
    } else {
        return name_;
    }
}

std::unique_ptr<ASTNode> JSXAttribute::clone() const {
    std::unique_ptr<ASTNode> cloned_value = value_ ? value_->clone() : nullptr;
    return std::make_unique<JSXAttribute>(name_, std::move(cloned_value), start_, end_);
}

// Reaching this means a node was handed to the engine that the compiler never
// emitted code for. There is no tree-walker behind it any more.
Value ASTNode::evaluate_compiled(Context& ctx) {
    bool compiled = false;
    Value v = VM::run_expression(this, ctx, compiled);
    if (!compiled && !ctx.has_exception()) {
        ctx.throw_type_error("Internal: expression could not be compiled");
    }
    return v;
}

Value ASTNode::evaluate(Context& ctx) {
    ctx.throw_type_error("Internal: node has no compiled form");
    return Value();
}

} // namespace Quanta
