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

        // Internal flag slots: non-enumerable/non-writable/non-configurable, named [[X]] so they don't shadow the accessor getters on RegExp.prototype.
        obj->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(pattern_), PropertyAttributes::None));
        obj->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(flags_.find('g') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(flags_.find('i') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(flags_.find('m') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(flags_.find('u') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(flags_.find('y') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(flags_.find('s') != std::string::npos), PropertyAttributes::None));
        obj->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(flags_.find('d') != std::string::npos), PropertyAttributes::None));
        {
            PropertyDescriptor us_desc(Value(flags_.find('v') != std::string::npos), PropertyAttributes::BuiltinFunction);
            obj->set_property_descriptor("unicodeSets", us_desc);
        }
        {
            // ES2015 21.2.3.2.1: lastIndex is {Writable:true, Enumerable:false, Configurable:false}
            PropertyDescriptor li_desc(Value(0.0), PropertyAttributes::Writable);
            obj->set_property_descriptor("lastIndex", li_desc);
        }

        auto regexp_impl = std::make_shared<RegExp>(pattern_, flags_);
        Object* obj_ptr = obj.get();

        auto test_fn = ObjectFactory::create_native_function("test",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                Value arg0 = args.empty() ? Value() : args[0];
                std::string str;
                if (arg0.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (arg0.is_object() || arg0.is_function()) { str = arg0.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { str = arg0.to_string(); }

                if (regexp_impl->get_global() || regexp_impl->get_sticky()) {
                    Value lastIndex_val = obj_ptr->get_property("lastIndex");
                    if (lastIndex_val.is_number()) {
                        regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                    }
                }

                bool result = regexp_impl->test(str);

                if (regexp_impl->get_global() || regexp_impl->get_sticky()) {
                    obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                }

                return Value(result);
            });

        auto exec_fn = ObjectFactory::create_native_function("exec",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                Value arg0 = args.empty() ? Value() : args[0];
                std::string str;
                if (arg0.is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to string"); return Value(); }
                else if (arg0.is_object() || arg0.is_function()) { str = arg0.to_property_key(); if (ctx.has_exception()) return Value(); }
                else { str = arg0.to_string(); }

                Value lastIndex_val = obj_ptr->get_property("lastIndex");
                if (lastIndex_val.is_number()) {
                    regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                }

                Value result = regexp_impl->exec(str);

                int new_last = regexp_impl->get_last_index();
                if ((regexp_impl->get_global() || regexp_impl->get_sticky()) &&
                    !result.is_null() && result.is_object()) {
                    Value matched = result.as_object()->get_element(0);
                    if (!matched.is_undefined() && matched.to_string().empty()) {
                        int li = static_cast<int>(lastIndex_val.is_number() ? lastIndex_val.to_number() : 0);
                        int advance = 1;
                        if (regexp_impl->get_unicode() || regexp_impl->get_unicode_sets()) {
                            size_t b = 0; int js = 0;
                            while (b < str.size() && js < li) {
                                unsigned char c = (unsigned char)str[b];
                                if (c < 0x80) { b++; js++; }
                                else if (c < 0xE0) { b += 2; js++; }
                                else if (c < 0xF0) { b += 3; js++; }
                                else { b += 4; js += 2; }
                            }
                            if (b < str.size() && (unsigned char)str[b] >= 0xF0) advance = 2;
                        }
                        new_last = li + advance;
                    }
                }
                obj_ptr->set_property("lastIndex", Value(static_cast<double>(new_last)));

                return result;
            });

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                // Use sorted `flags` property per spec (RegExp.prototype.toString = /source/flags).
                Value flags_v = obj_ptr->get_property("flags");
                std::string flags_str = flags_v.is_string() ? flags_v.to_string() : regexp_impl->get_flags();
                return Value("/" + regexp_impl->get_source() + "/" + flags_str);
            });

        auto compile_fn = ObjectFactory::create_native_function("compile",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                std::string pattern = "";
                std::string flags = "";
                if (args.size() > 0) pattern = args[0].to_string();
                if (args.size() > 1) flags = args[1].to_string();

                regexp_impl->compile(pattern, flags);

                obj_ptr->set_property_descriptor("[[source]]",     PropertyDescriptor(Value(regexp_impl->get_source()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[global]]",     PropertyDescriptor(Value(regexp_impl->get_global()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[ignoreCase]]", PropertyDescriptor(Value(regexp_impl->get_ignore_case()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[multiline]]",  PropertyDescriptor(Value(regexp_impl->get_multiline()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[unicode]]",    PropertyDescriptor(Value(regexp_impl->get_unicode()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[sticky]]",     PropertyDescriptor(Value(regexp_impl->get_sticky()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[dotAll]]",     PropertyDescriptor(Value(regexp_impl->get_dotall()), PropertyAttributes::None));
                obj_ptr->set_property_descriptor("[[hasIndices]]", PropertyDescriptor(Value(regexp_impl->get_flags().find('d') != std::string::npos), PropertyAttributes::None));
                obj_ptr->set_property("lastIndex", Value(0.0));

                return Value(obj_ptr);
            }, 2);

        obj->set_property("test", Value(test_fn.release()));
        obj->set_property("exec", Value(exec_fn.release()));
        obj->set_property("toString", Value(toString_fn.release()));
        obj->set_property("compile", Value(compile_fn.release()));

        return Value(obj.release());
    } catch (const std::exception& e) {
        ctx.throw_syntax_error(std::string(e.what()));
        return Value();
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
    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (object_value.is_null() || object_value.is_undefined()) {
        return Value();
    }

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
