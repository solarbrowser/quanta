/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

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

Value ConditionalExpression::evaluate(Context& ctx) {
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (test_value.to_boolean()) {
        return consequent_->evaluate(ctx);
    } else {
        return alternate_->evaluate(ctx);
    }
}

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


Value RegexLiteral::evaluate(Context& ctx) {
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
    Value re = static_cast<Function*>(ctor)->construct(ctx, { Value(pattern_), Value(flags_) });
    ctx.set_new_target(saved_new_target);
    return re;
}

std::string RegexLiteral::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

std::unique_ptr<ASTNode> RegexLiteral::clone() const {
    return std::make_unique<RegexLiteral>(pattern_, flags_, start_, end_);
}


Value SpreadElement::evaluate(Context& ctx) {
    return argument_->evaluate(ctx);
}

std::string SpreadElement::to_string() const {
    return "..." + argument_->to_string();
}

std::unique_ptr<ASTNode> SpreadElement::clone() const {
    return std::make_unique<SpreadElement>(argument_->clone(), start_, end_);
}


Value JSXElement::evaluate(Context& ctx) {

    Value react = ctx.get_binding("React");
    if (!react.is_object()) {
        ctx.throw_exception(Value(std::string("React is not defined - JSX requires React to be in scope")));
        return Value();
    }

    Value createElement = static_cast<Object*>(react.as_object())->get_property("createElement");
    if (!createElement.is_function()) {
        ctx.throw_exception(Value(std::string("React.createElement is not a function")));
        return Value();
    }

    std::vector<Value> args;

    if (std::islower(tag_name_[0])) {
        args.push_back(Value(tag_name_));
    } else {
        Value component = ctx.get_binding(tag_name_);
        args.push_back(component);
    }

    auto props_obj = ObjectFactory::create_object();
    for (const auto& attr : attributes_) {
        JSXAttribute* jsx_attr = static_cast<JSXAttribute*>(attr.get());
        Value attr_value = jsx_attr->get_value()->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        props_obj->set_property(jsx_attr->get_name(), attr_value);
    }
    args.push_back(Value(props_obj.release()));

    for (const auto& child : children_) {
        Value child_value = child->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        args.push_back(child_value);
    }

    Function* create_fn = createElement.as_function();
    return create_fn->call(ctx, args);
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
        Function* getter = dynamic_cast<Function*>(desc.get_getter());
        if (getter) return getter->call(ctx, {}, object_value);
    }
    return proto_obj->get_property(prop_name);
}

Value OptionalChainingExpression::evaluate(Context& ctx) {
    // See g_optional_chain_shortcircuit's doc comment (ast_internal.h): if my
    // own base isn't itself a chain link, an unrelated earlier chain's stale
    // short-circuit flag must not leak into this (possibly new) one.
    if (!is_chain_link_type(object_->get_type())) g_optional_chain_shortcircuit = false;

    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (object_value.is_null() || object_value.is_undefined()) {
        g_optional_chain_shortcircuit = true;
        return Value();
    }
    g_optional_chain_shortcircuit = false;

    if (computed_) {
        Value property_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // Indexing a primitive string by a numeric index returns the character, same as a
        // plain (non-optional) MemberExpression -- e.g. `"hello"?.[0]` must yield "h".
        if (object_value.is_string() && property_value.is_number()) {
            std::string str_value = object_value.to_string();
            int index = static_cast<int>(property_value.to_number());
            if (index >= 0 && index < static_cast<int>(str_value.length())) {
                return Value(std::string(1, str_value[index]));
            }
            return Value();
        }

        std::string prop_name;
        if (property_value.is_symbol()) {
            prop_name = property_value.as_symbol()->to_property_key();
        } else {
            prop_name = property_value.to_string();
        }

        if (object_value.is_object()) {
            Object* obj = object_value.as_object();
            return obj->get_property(prop_name);
        }
        if (object_value.is_string() || object_value.is_number() || object_value.is_boolean()) {
            return box_primitive_and_get_property(ctx, object_value, prop_name);
        }
    } else {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop_id = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop_id->get_name();

            if (object_value.is_object() || object_value.is_function()) {
                Object* obj = object_value.is_function()
                    ? static_cast<Object*>(object_value.as_function())
                    : object_value.as_object();
                if (!prop_name.empty() && prop_name[0] == '#') {
                    if (!private_brand_check(ctx, obj, prop_name)) {
                        ctx.throw_type_error("Cannot read private member " + prop_name + " from an object whose class did not declare it");
                        return Value();
                    }
                    // Instance fields are stored under a per-class-qualified key (see resolve_private_storage_key).
                    std::string qualified = resolve_private_storage_key(prop_name, obj);
                    if (obj->has_private_slot(qualified)) prop_name = qualified;
                }
                return obj->get_property(prop_name);
            }
            if (object_value.is_string() && prop_name == "length") {
                return Value(static_cast<double>(utf16_length(object_value.to_string())));
            }
            if (object_value.is_string() || object_value.is_number() || object_value.is_boolean()) {
                return box_primitive_and_get_property(ctx, object_value, prop_name);
            }
        }
    }

    return Value();
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


Value NullishCoalescingExpression::evaluate(Context& ctx) {
    Value left_value = left_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (!left_value.is_null() && !left_value.is_undefined()) {
        return left_value;
    }

    Value right_value = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    return right_value;
}

std::string NullishCoalescingExpression::to_string() const {
    return "(" + left_->to_string() + " ?? " + right_->to_string() + ")";
}

std::unique_ptr<ASTNode> NullishCoalescingExpression::clone() const {
    return std::make_unique<NullishCoalescingExpression>(
        left_->clone(), right_->clone(), start_, end_
    );
}

Value JSXText::evaluate(Context& ctx) {
    (void)ctx;
    return Value(text_);
}

std::string JSXText::to_string() const {
    return text_;
}

std::unique_ptr<ASTNode> JSXText::clone() const {
    return std::make_unique<JSXText>(text_, start_, end_);
}

Value JSXExpression::evaluate(Context& ctx) {
    return expression_->evaluate(ctx);
}

std::string JSXExpression::to_string() const {
    return "{" + expression_->to_string() + "}";
}

std::unique_ptr<ASTNode> JSXExpression::clone() const {
    return std::make_unique<JSXExpression>(expression_->clone(), start_, end_);
}

Value JSXAttribute::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
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

} // namespace Quanta
