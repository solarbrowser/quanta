/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/String.h"
#include <sstream>
#include <cmath>
#include <climits>

namespace Quanta {

static std::string literal_to_property_key(Context& ctx, const Value& val) {
    if (val.is_symbol()) return val.as_symbol()->to_property_key();
    if (!val.is_object() && !val.is_function()) return val.to_string();

    Object* obj = val.is_function() ? static_cast<Object*>(val.as_function()) : val.as_object();

    Symbol* tp_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (tp_sym) {
        Value tp = obj->get_property(tp_sym->to_property_key());
        if (ctx.has_exception()) return "";
        if (!tp.is_undefined()) {
            if (!tp.is_function()) {
                ctx.throw_type_error("Cannot convert object to primitive value");
                return "";
            }
            Value result = tp.as_function()->call(ctx, {Value(std::string("string"))}, val);
            if (ctx.has_exception()) return "";
            if (result.is_symbol()) return result.as_symbol()->to_property_key();
            if (result.is_object() || result.is_function()) {
                ctx.throw_type_error("Cannot convert object to primitive value");
                return "";
            }
            return result.to_string();
        }
    }

    Value ts = obj->get_property("toString");
    if (!ctx.has_exception() && ts.is_function()) {
        Value r = ts.as_function()->call(ctx, {}, val);
        if (ctx.has_exception()) return "";
        if (!r.is_object() && !r.is_function()) return r.to_string();
    }
    if (ctx.has_exception()) return "";

    Value vof = obj->get_property("valueOf");
    if (!ctx.has_exception() && vof.is_function()) {
        Value r = vof.as_function()->call(ctx, {}, val);
        if (ctx.has_exception()) return "";
        if (!r.is_object() && !r.is_function()) return r.to_string();
    }
    if (ctx.has_exception()) return "";

    ctx.throw_type_error("Cannot convert object to primitive value");
    return "";
}

Value ObjectLiteral::evaluate(Context& ctx) {
    auto object = ObjectFactory::create_object();
    if (!object) {
        ctx.throw_exception(Value(std::string("Failed to create object")));
        return Value();
    }

    if (ctx.get_engine() && ctx.get_engine()->get_garbage_collector()) {
        ctx.get_engine()->get_garbage_collector()->register_object(object.get());
    }

    for (const auto& prop : properties_) {
        if (prop->key == nullptr && prop->value && prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            SpreadElement* spread = static_cast<SpreadElement*>(prop->value.get());
            Value spread_value = spread->get_argument()->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            // ES2018: null/undefined in object spread are silently ignored
            if (spread_value.is_null() || spread_value.is_undefined()) continue;
            if (!spread_value.is_object() && !spread_value.is_function() &&
                    !spread_value.is_string() && !spread_value.is_number() && !spread_value.is_boolean()) {
                ctx.throw_type_error("Spread syntax can only be applied to objects");
                return Value();
            }

            // Box primitives for spread
            Object* spread_obj = nullptr;
            if (spread_value.is_object()) {
                spread_obj = spread_value.as_object();
            } else if (spread_value.is_function()) {
                spread_obj = spread_value.as_function();
            } else {
                continue; // primitives have no enumerable own properties
            }
            if (!spread_obj) {
                ctx.throw_exception(Value(std::string("Error: Could not convert value to object")));
                return Value();
            }

            try {
                if (spread_obj->get_type() == Object::ObjectType::Proxy) {
                    // get_enumerable_keys()/get_property() don't know about Proxy traps, so go through ownKeys/getOwnPropertyDescriptor/get directly per spec.
                    Proxy* proxy = static_cast<Proxy*>(spread_obj);
                    for (const auto& prop_name : proxy->own_keys_trap()) {
                        // own_keys_trap() returns symbol keys as their "@@sym:" string encoding; decode back to the real Symbol so traps receive the original key, not its string form.
                        Symbol* sym = Symbol::find_by_property_key(prop_name);
                        Value key_value = sym ? Value(sym) : Value(prop_name);
                        PropertyDescriptor desc = proxy->get_own_property_descriptor_trap(key_value);
                        if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) continue;
                        if (!desc.is_enumerable()) continue;
                        Value prop_value = proxy->get_trap(key_value);
                        object->set_property(prop_name, prop_value);
                    }
                } else {
                    auto property_names = spread_obj->get_enumerable_keys();
                    for (const auto& prop_name : property_names) {
                        Value prop_value = spread_obj->get_property(prop_name);
                        object->set_property(prop_name, prop_value);
                    }
                }
            } catch (const std::exception& e) {
                ctx.throw_exception(Value("Error processing spread properties: " + std::string(e.what())));
                return Value();
            }
            continue;
        }

        std::string key;

        if (!prop->key) {
            ctx.throw_exception(Value(std::string("Property missing key")));
            return Value();
        }

        if (prop->computed) {
            Value key_value = prop->key->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            key = literal_to_property_key(ctx, key_value);
            if (ctx.has_exception()) return Value();
        } else {
            if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(prop->key.get());
                key = id->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                StringLiteral* str = static_cast<StringLiteral*>(prop->key.get());
                key = str->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                NumberLiteral* num = static_cast<NumberLiteral*>(prop->key.get());
                double value = num->get_value();
                if (value == std::floor(value) && value >= LLONG_MIN && value <= LLONG_MAX) {
                    key = std::to_string(static_cast<long long>(value));
                } else {
                    std::ostringstream oss;
                    oss << value;
                    key = oss.str();
                }
            } else {
                ctx.throw_exception(Value(std::string("Invalid property key in object literal")));
                return Value();
            }
        }

        Value value;
        if (prop->value) {
            value = prop->value->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        } else {
            if (prop->key && prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(prop->key.get());
                value = id->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } else {
                ctx.throw_exception(Value(std::string("Invalid shorthand property in object literal")));
                return Value();
            }
        }

        if (value.is_function()) {
            Function* fn = value.as_function();
            // Spec 12.2.6.9 step 6: only set name for IsAnonymousFunctionDefinition
            // (not comma expressions, parenthesized non-anon, etc.)
            auto is_anon_fn_def = [](const ASTNode* n) {
                if (!n) return false;
                auto t = n->get_type();
                return t == ASTNode::Type::FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
                       t == ASTNode::Type::CLASS_DECLARATION;
            };
            // Getters/setters are always named; for other properties, only if IsAnonymousFunctionDefinition
            bool is_getter_setter = prop->type == ObjectLiteral::PropertyType::Getter ||
                                    prop->type == ObjectLiteral::PropertyType::Setter;
            bool should_name = is_getter_setter ||
                               (prop->value && is_anon_fn_def(prop->value.get()));
            if (should_name && (fn->get_name().empty() || fn->get_name() == "<arrow>")) {
                std::string func_name = key;
                if (prop->computed) {
                    Value key_check = prop->key->evaluate(ctx);
                    if (key_check.is_symbol()) {
                        std::string desc = key_check.as_symbol()->get_description();
                        func_name = desc.empty() ? "" : "[" + desc + "]";
                    }
                }
                if (prop->type == ObjectLiteral::PropertyType::Getter) {
                    fn->set_name("get " + func_name);
                } else if (prop->type == ObjectLiteral::PropertyType::Setter) {
                    fn->set_name("set " + func_name);
                } else {
                    fn->set_name(func_name);
                }
            }
            if (prop->type == ObjectLiteral::PropertyType::Method) {
                fn->set_property("__super_constructor__", Value(true));
                // Spec 14.3.9: non-generator methods are not constructors and have no .prototype
                if (fn->is_constructor()) {
                    fn->set_is_constructor(false);
                    fn->set_function_prototype(nullptr);
                }
            }
        }

        if (prop->type == ObjectLiteral::PropertyType::Getter || prop->type == ObjectLiteral::PropertyType::Setter) {
            if (!value.is_function()) {
                ctx.throw_exception(Value(std::string("Getter/setter must be a function")));
                return Value();
            }
            // Getter/setter functions must not expose [[Construct]] -- remove prototype.
            value.as_function()->set_function_prototype(nullptr);

            PropertyDescriptor desc;
            if (object->has_own_property(key)) {
                desc = object->get_property_descriptor(key);
            }

            if (prop->type == ObjectLiteral::PropertyType::Getter) {
                desc.set_getter(value.as_function());
                desc.set_enumerable(true);
                desc.set_configurable(true);
            } else {
                desc.set_setter(value.as_function());
                desc.set_enumerable(true);
                desc.set_configurable(true);
            }

            object->set_property_descriptor(key, desc);
        } else if (key == "__proto__" && !prop->computed && !prop->shorthand && prop->type == ObjectLiteral::PropertyType::Value) {
            if (value.is_object()) {
                object->set_prototype(value.as_object());
            } else if (value.is_null()) {
                object->set_prototype(nullptr);
            }
        } else if (key == "__proto__" && (prop->shorthand || prop->computed)) {
            // Shorthand/computed __proto__ creates an own data property, not set [[Prototype]]
            PropertyDescriptor own_desc(value, PropertyAttributes::Default);
            object->set_property_descriptor(key, own_desc);
        } else {
            object->set_property(key, value);
        }
    }

    return Value(object.release());
}

std::string ObjectLiteral::to_string() const {
    std::ostringstream oss;
    oss << "{";

    for (size_t i = 0; i < properties_.size(); ++i) {
        if (i > 0) oss << ", ";

        if (properties_[i]->key == nullptr && properties_[i]->value &&
            properties_[i]->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            oss << properties_[i]->value->to_string();
        } else {
            if (properties_[i]->computed) {
                oss << "[" << properties_[i]->key->to_string() << "]";
            } else {
                oss << properties_[i]->key->to_string();
            }

            oss << ": " << properties_[i]->value->to_string();
        }
    }

    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> ObjectLiteral::clone() const {
    std::vector<std::unique_ptr<Property>> cloned_properties;

    for (const auto& prop : properties_) {
        auto cloned_prop = std::make_unique<Property>(
            prop->key ? prop->key->clone() : nullptr,
            prop->value ? prop->value->clone() : nullptr,
            prop->computed,
            prop->type
        );
        cloned_prop->shorthand = prop->shorthand;
        cloned_properties.push_back(std::move(cloned_prop));
    }

    return std::make_unique<ObjectLiteral>(std::move(cloned_properties), start_, end_);
}


Value ArrayLiteral::evaluate(Context& ctx) {

    auto array = ObjectFactory::create_array(0);
    if (!array) {
        return Value(std::string("[]"));
    }

    if (ctx.get_engine() && ctx.get_engine()->get_garbage_collector()) {
        ctx.get_engine()->get_garbage_collector()->register_object(array.get());
    }

    uint32_t array_index = 0;
    for (const auto& element : elements_) {
        if (element->get_type() == Type::SPREAD_ELEMENT) {
            Value spread_value = element->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (spread_value.is_object() || spread_value.is_function()) {
                Object* spread_obj = spread_value.is_function()
                    ? static_cast<Object*>(spread_value.as_function())
                    : spread_value.as_object();
                bool used_iterator = false;
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym && !spread_obj->is_array()) {
                    Value iter_method = spread_obj->get_property(iter_sym->to_property_key());
                    if (ctx.has_exception()) return Value();
                    if (!iter_method.is_undefined()) {
                        if (!iter_method.is_null() && !iter_method.is_function()) {
                            ctx.throw_type_error("Symbol.iterator is not callable");
                            return Value();
                        }
                        if (iter_method.is_null()) {
                            ctx.throw_type_error("Symbol.iterator is not a function");
                            return Value();
                        }
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, spread_value);
                        if (ctx.has_exception()) return Value();
                        if (!iter_obj.is_object()) {
                            ctx.throw_type_error("Symbol.iterator must return an Object");
                            return Value();
                        }
                        Value next_fn = iter_obj.as_object()->get_property("next");
                        if (next_fn.is_function()) {
                            used_iterator = true;
                            for (uint32_t ii = 0; ii < 100000; ii++) {
                                Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                if (ctx.has_exception()) return Value();
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                array->set_element(array_index++, res.as_object()->get_property("value"));
                            }
                        }
                    }
                }
                if (!used_iterator) {
                    uint32_t spread_length = spread_obj->get_length();
                    for (uint32_t j = 0; j < spread_length; ++j) {
                        Value item = spread_obj->get_element(j);
                        array->set_element(array_index++, item);
                    }
                }
            } else if (spread_value.is_string()) {
                const std::string& str = spread_value.as_string()->str();
                size_t i = 0;
                while (i < str.size()) {
                    unsigned char c = str[i];
                    size_t char_len = 1;
                    if (c >= 0xF0) char_len = 4;
                    else if (c >= 0xE0) char_len = 3;
                    else if (c >= 0xC0) char_len = 2;
                    std::string ch = str.substr(i, char_len);
                    array->set_element(array_index++, Value(ch));
                    i += char_len;
                }
            } else {
                array->set_element(array_index++, spread_value);
            }
        } else if (element->get_type() == Type::UNDEFINED_LITERAL) {
            array_index++;
        } else {
            Value element_value = element->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            array->set_element(array_index++, element_value);
        }
    }

    array->set_length(array_index);
    return Value(array.release());
}

std::string ArrayLiteral::to_string() const {
    std::ostringstream oss;
    oss << "[";

    for (size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << elements_[i]->to_string();
    }

    oss << "]";
    return oss.str();
}

std::unique_ptr<ASTNode> ArrayLiteral::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_elements;

    for (const auto& element : elements_) {
        cloned_elements.push_back(element->clone());
    }

    return std::make_unique<ArrayLiteral>(std::move(cloned_elements), start_, end_);
}

} // namespace Quanta
