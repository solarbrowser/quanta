/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Symbol.h"
#include <algorithm>

namespace Quanta {

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
    (void)ctx;
    try {
        auto obj = std::make_unique<Object>(Object::ObjectType::RegExp);

        Value regexp_ctor = ctx.get_binding("RegExp");
        if (regexp_ctor.is_function()) {
            Value proto = regexp_ctor.as_function()->get_property("prototype");
            if (proto.is_object()) {
                obj->set_prototype(proto.as_object());
            }
        }

        obj->set_property("_isRegExp", Value(true));

        obj->set_property("__pattern__", Value(pattern_));
        obj->set_property("__flags__", Value(flags_));

        obj->set_property("source", Value(pattern_));
        std::string sorted_flags = flags_;
        std::sort(sorted_flags.begin(), sorted_flags.end());
        obj->set_property("flags", Value(sorted_flags));
        obj->set_property("global", Value(flags_.find('g') != std::string::npos));
        obj->set_property("ignoreCase", Value(flags_.find('i') != std::string::npos));
        obj->set_property("multiline", Value(flags_.find('m') != std::string::npos));
        obj->set_property("unicode", Value(flags_.find('u') != std::string::npos));
        obj->set_property("sticky", Value(flags_.find('y') != std::string::npos));
        obj->set_property("lastIndex", Value(0.0));

        auto regexp_impl = std::make_shared<RegExp>(pattern_, flags_);
        Object* obj_ptr = obj.get();

        auto test_fn = ObjectFactory::create_native_function("test",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) return Value(false);

                if (regexp_impl->get_global()) {
                    Value lastIndex_val = obj_ptr->get_property("lastIndex");
                    if (lastIndex_val.is_number()) {
                        regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                    }
                }

                std::string str = args[0].to_string();
                bool result = regexp_impl->test(str);

                if (regexp_impl->get_global()) {
                    obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                }

                return Value(result);
            });

        auto exec_fn = ObjectFactory::create_native_function("exec",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) return Value::null();

                Value lastIndex_val = obj_ptr->get_property("lastIndex");
                if (lastIndex_val.is_number()) {
                    regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                }

                std::string str = args[0].to_string();
                Value result = regexp_impl->exec(str);

                obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                return result;
            });

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                (void)args;
                return Value(regexp_impl->to_string());
            });

        auto compile_fn = ObjectFactory::create_native_function("compile",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                std::string pattern = "";
                std::string flags = "";
                if (args.size() > 0) pattern = args[0].to_string();
                if (args.size() > 1) flags = args[1].to_string();

                regexp_impl->compile(pattern, flags);

                obj_ptr->set_property("source", Value(regexp_impl->get_source()));
                std::string sf = regexp_impl->get_flags();
                std::sort(sf.begin(), sf.end());
                obj_ptr->set_property("flags", Value(sf));
                obj_ptr->set_property("global", Value(regexp_impl->get_global()));
                obj_ptr->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                obj_ptr->set_property("multiline", Value(regexp_impl->get_multiline()));
                obj_ptr->set_property("lastIndex", Value(0.0));

                return Value(obj_ptr);
            }, 2);

        obj->set_property("test", Value(test_fn.release()));
        obj->set_property("exec", Value(exec_fn.release()));
        obj->set_property("toString", Value(toString_fn.release()));
        obj->set_property("compile", Value(compile_fn.release()));

        return Value(obj.release());
    } catch (const std::exception& e) {
        return Value::null();
    }
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


Value OptionalChainingExpression::evaluate(Context& ctx) {
    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (object_value.is_null() || object_value.is_undefined()) {
        return Value();
    }

    if (computed_) {
        Value property_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

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
    } else {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop_id = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop_id->get_name();

            if (object_value.is_object()) {
                Object* obj = object_value.as_object();
                return obj->get_property(prop_name);
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
