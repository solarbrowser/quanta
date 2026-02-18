/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/AST.h"
#include <iostream>
#include <algorithm>

namespace Quanta {


Proxy::Proxy(Object* target, Object* handler) 
    : Object(ObjectType::Proxy), target_(target), handler_(handler) {
    parse_handler();
}

Value Proxy::get_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.get) {
        return parsed_handler_.get(key);
    }
    
    return target_->get_property(key.to_string());
}

bool Proxy::set_trap(const Value& key, const Value& value) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.set) {
        return parsed_handler_.set(key, value);
    }
    
    return target_->set_property(key.to_string(), value);
}

bool Proxy::has_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.has) {
        return parsed_handler_.has(key);
    }
    
    return target_->has_property(key.to_string());
}

bool Proxy::delete_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.deleteProperty) {
        return parsed_handler_.deleteProperty(key);
    }
    
    return target_->delete_property(key.to_string());
}

std::vector<std::string> Proxy::own_keys_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.ownKeys) {
        return parsed_handler_.ownKeys();
    }
    
    return target_->get_own_property_keys();
}

Value Proxy::get_prototype_of_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.getPrototypeOf) {
        return parsed_handler_.getPrototypeOf(Value());
    }
    
    Object* proto = target_->get_prototype();
    return proto ? Value(proto) : Value::null();
}

bool Proxy::set_prototype_of_trap(Object* proto) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.setPrototypeOf) {
        return parsed_handler_.setPrototypeOf(proto);
    }
    
    target_->set_prototype(proto);
    return true;
}

bool Proxy::is_extensible_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.isExtensible) {
        return parsed_handler_.isExtensible();
    }
    
    return target_->is_extensible();
}

bool Proxy::prevent_extensions_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.preventExtensions) {
        return parsed_handler_.preventExtensions();
    }
    
    target_->prevent_extensions();
    return true;
}

PropertyDescriptor Proxy::get_own_property_descriptor_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.getOwnPropertyDescriptor) {
        return parsed_handler_.getOwnPropertyDescriptor(key);
    }
    
    return target_->get_property_descriptor(key.to_string());
}

bool Proxy::define_property_trap(const Value& key, const PropertyDescriptor& desc) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.defineProperty) {
        return parsed_handler_.defineProperty(key, desc);
    }
    
    return target_->set_property_descriptor(key.to_string(), desc);
}

Value Proxy::apply_trap(const std::vector<Value>& args, const Value& this_value) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.apply) {
        return parsed_handler_.apply(args);
    }
    
    if (target_->is_function()) {
        Function* func = static_cast<Function*>(target_);
        Context dummy_ctx(nullptr);
        return func->call(dummy_ctx, args, this_value);
    }
    
    return Value();
}

Value Proxy::construct_trap(const std::vector<Value>& args) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.construct) {
        return parsed_handler_.construct(args);
    }
    
    if (target_->is_function()) {
        Function* func = static_cast<Function*>(target_);
        Context dummy_ctx(nullptr);
        return func->construct(dummy_ctx, args);
    }
    
    return Value();
}

void Proxy::parse_handler() {
    if (!handler_) {
        return;
    }
    
    Value get_method = handler_->get_property("get");
    if (get_method.is_function()) {
        Function* get_fn = get_method.as_function();
        parsed_handler_.get = [get_fn, this](const Value& key) -> Value {
            try {
                Context dummy_ctx(nullptr);
                std::vector<Value> args = {Value(target_), key};
                return get_fn->call(dummy_ctx, args);
            } catch (...) {
                return target_->get_property(key.to_string());
            }
        };
    }
    
    
    
}

void Proxy::revoke() {
    target_ = nullptr;
    handler_ = nullptr;
    parsed_handler_ = Handler{};
}

void Proxy::throw_if_revoked(Context& ctx) const {
    if (is_revoked()) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy has been revoked")));
    }
}


Value Proxy::get_property(const std::string& key) const {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    return const_cast<Proxy*>(this)->get_trap(Value(key));
}


Value Proxy::proxy_constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy constructor requires target and handler arguments")));
        return Value();
    }
    
    if (!args[0].is_object() || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy constructor requires object arguments")));
        return Value();
    }
    
    Object* target = args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    return Value(proxy.release());
}

Value Proxy::proxy_revocable(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy.revocable requires target and handler arguments")));
        return Value();
    }
    
    if (!args[0].is_object() || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy.revocable requires object arguments")));
        return Value();
    }
    
    Object* target = args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    Proxy* proxy_ptr = proxy.get();
    
    auto revoke_fn = ObjectFactory::create_native_function("revoke", 
        [proxy_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            proxy_ptr->revoke();
            return Value();
        });
    
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("proxy", Value(proxy.release()));
    result_obj->set_property("revoke", Value(revoke_fn.release()));
    
    return Value(result_obj.release());
}

void Proxy::setup_proxy(Context& ctx) {
    auto proxy_constructor_fn = ObjectFactory::create_native_function("Proxy", proxy_constructor);
    
    auto revocable_fn = ObjectFactory::create_native_function("revocable", proxy_revocable);
    proxy_constructor_fn->set_property("revocable", Value(revocable_fn.release()));
    
    ctx.register_built_in_object("Proxy", proxy_constructor_fn.release());
}


Value Reflect::reflect_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.get requires at least one argument")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    std::string key = args.size() > 1 ? to_property_key(args[1]) : "";
    Value receiver = args.size() > 2 ? args[2] : args[0];
    
    (void)receiver;
    
    return target->get_property(key);
}

Value Reflect::reflect_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.set requires at least two arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    Value value = args[2];
    Value receiver = args.size() > 3 ? args[3] : args[0];
    
    (void)receiver;
    
    bool result = target->set_property(key, value);
    return Value(result);
}

Value Reflect::reflect_has(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.has requires two arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    return Value(target->has_property(key));
}

Value Reflect::reflect_delete_property(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.deleteProperty requires two arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    return Value(target->delete_property(key));
}

Value Reflect::reflect_own_keys(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.ownKeys requires one argument")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    auto keys = target->get_own_property_keys();

    // ES6 [[OwnPropertyKeys]] order: integer indices (ascending), then string keys (insertion), then symbols (insertion)
    std::vector<std::string> index_keys, string_keys, symbol_keys;
    for (const auto& key : keys) {
        if (key.find("@@sym:") == 0) {
            symbol_keys.push_back(key);
        } else {
            bool is_index = !key.empty();
            for (char c : key) {
                if (c < '0' || c > '9') { is_index = false; break; }
            }
            if (is_index && key.length() <= 10) {
                index_keys.push_back(key);
            } else {
                string_keys.push_back(key);
            }
        }
    }
    std::sort(index_keys.begin(), index_keys.end(), [](const std::string& a, const std::string& b) {
        return std::stoul(a) < std::stoul(b);
    });

    auto result_array = ObjectFactory::create_array(keys.size());
    uint32_t idx = 0;
    for (const auto& key : index_keys) {
        result_array->set_element(idx++, Value(key));
    }
    for (const auto& key : string_keys) {
        result_array->set_element(idx++, Value(key));
    }
    for (const auto& key : symbol_keys) {
        Symbol* sym = Symbol::find_by_property_key(key);
        if (sym) {
            result_array->set_element(idx++, Value(sym));
        } else {
            result_array->set_element(idx++, Value(key));
        }
    }

    return Value(result_array.release());
}

Value Reflect::reflect_get_prototype_of(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.getPrototypeOf requires one argument")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    Object* proto = target->get_prototype();
    return proto ? Value(proto) : Value::null();
}

Value Reflect::reflect_set_prototype_of(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.setPrototypeOf requires two arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    Object* proto = args[1].is_null() ? nullptr : 
                   (args[1].is_object() ? args[1].as_object() : nullptr);
    
    target->set_prototype(proto);
    return Value(true);
}

Value Reflect::reflect_is_extensible(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.isExtensible requires one argument")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    return Value(target->is_extensible());
}

Value Reflect::reflect_prevent_extensions(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.preventExtensions requires one argument")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    target->prevent_extensions();
    return Value(true);
}

Value Reflect::reflect_apply(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 3) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.apply requires three arguments")));
        return Value();
    }
    
    if (!args[0].is_function()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.apply first argument must be a function")));
        return Value();
    }
    
    Function* target = args[0].as_function();
    Value this_value = args[1];
    
    std::vector<Value> apply_args;
    if (args[2].is_object()) {
        Object* args_obj = args[2].as_object();
        if (args_obj->is_array()) {
            uint32_t length = args_obj->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                apply_args.push_back(args_obj->get_element(i));
            }
        }
    }
    
    return target->call(ctx, apply_args, this_value);
}

Value Reflect::reflect_construct(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.construct requires at least two arguments")));
        return Value();
    }

    if (!args[0].is_function()) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.construct first argument must be a function")));
        return Value();
    }

    Function* target = args[0].as_function();

    // Get newTarget (3rd argument), default to target if not provided
    Function* newTarget = target;
    if (args.size() >= 3) {
        if (!args[2].is_function()) {
            ctx.throw_exception(Value(std::string("TypeError: Reflect.construct newTarget must be a constructor")));
            return Value();
        }
        newTarget = args[2].as_function();
    }

    // Check if newTarget is a constructor
    if (!newTarget->is_constructor()) {
        ctx.throw_exception(Value(std::string("TypeError: newTarget is not a constructor")));
        return Value();
    }

    std::vector<Value> construct_args;
    if (args[1].is_object()) {
        Object* args_obj = args[1].as_object();
        if (args_obj->is_array()) {
            uint32_t length = args_obj->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                construct_args.push_back(args_obj->get_element(i));
            }
        }
    }

    return target->construct(ctx, construct_args);
}

Value Reflect::reflect_get_own_property_descriptor(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.getOwnPropertyDescriptor requires two arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    std::string key = to_property_key(args[1]);
    PropertyDescriptor desc = target->get_property_descriptor(key);
    
    return from_property_descriptor(desc);
}

Value Reflect::reflect_define_property(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 3) {
        ctx.throw_exception(Value(std::string("TypeError: Reflect.defineProperty requires three arguments")));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    
    if (!args[2].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Property descriptor must be an object")));
        return Value(false);
    }
    
    Object* desc_obj = args[2].as_object();
    PropertyDescriptor prop_desc;
    
    if (desc_obj->has_own_property("get")) {
        Value getter = desc_obj->get_property("get");
        if (getter.is_function()) {
            prop_desc.set_getter(getter.as_object());
        }
    }
    
    if (desc_obj->has_own_property("set")) {
        Value setter = desc_obj->get_property("set");
        if (setter.is_function()) {
            prop_desc.set_setter(setter.as_object());
        }
    }
    
    if (desc_obj->has_own_property("value")) {
        Value value = desc_obj->get_property("value");
        prop_desc.set_value(value);
    }
    
    if (desc_obj->has_own_property("writable")) {
        prop_desc.set_writable(desc_obj->get_property("writable").to_boolean());
    }
    
    if (desc_obj->has_own_property("enumerable")) {
        prop_desc.set_enumerable(desc_obj->get_property("enumerable").to_boolean());
    }
    
    if (desc_obj->has_own_property("configurable")) {
        prop_desc.set_configurable(desc_obj->get_property("configurable").to_boolean());
    }
    
    bool success = target->set_property_descriptor(key, prop_desc);
    return Value(success);
}

void Reflect::setup_reflect(Context& ctx) {
    auto reflect_obj = ObjectFactory::create_object();
    
    auto get_fn = ObjectFactory::create_native_function("get", reflect_get);
    auto set_fn = ObjectFactory::create_native_function("set", reflect_set);
    auto has_fn = ObjectFactory::create_native_function("has", reflect_has);
    auto delete_property_fn = ObjectFactory::create_native_function("deleteProperty", reflect_delete_property);
    auto own_keys_fn = ObjectFactory::create_native_function("ownKeys", reflect_own_keys);
    auto get_prototype_of_fn = ObjectFactory::create_native_function("getPrototypeOf", reflect_get_prototype_of);
    auto set_prototype_of_fn = ObjectFactory::create_native_function("setPrototypeOf", reflect_set_prototype_of);
    auto is_extensible_fn = ObjectFactory::create_native_function("isExtensible", reflect_is_extensible);
    auto prevent_extensions_fn = ObjectFactory::create_native_function("preventExtensions", reflect_prevent_extensions);
    auto apply_fn = ObjectFactory::create_native_function("apply", reflect_apply);
    auto construct_fn = ObjectFactory::create_native_function("construct", reflect_construct);
    auto get_own_property_descriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2);
    auto define_property_fn = ObjectFactory::create_native_function("defineProperty", reflect_define_property, 3);
    
    reflect_obj->set_property("get", Value(get_fn.release()));
    reflect_obj->set_property("set", Value(set_fn.release()));
    reflect_obj->set_property("has", Value(has_fn.release()));
    reflect_obj->set_property("deleteProperty", Value(delete_property_fn.release()));
    reflect_obj->set_property("ownKeys", Value(own_keys_fn.release()));
    reflect_obj->set_property("getPrototypeOf", Value(get_prototype_of_fn.release()));
    reflect_obj->set_property("setPrototypeOf", Value(set_prototype_of_fn.release()));
    reflect_obj->set_property("isExtensible", Value(is_extensible_fn.release()));
    reflect_obj->set_property("preventExtensions", Value(prevent_extensions_fn.release()));
    reflect_obj->set_property("apply", Value(apply_fn.release()));
    reflect_obj->set_property("construct", Value(construct_fn.release()));
    reflect_obj->set_property("getOwnPropertyDescriptor", Value(get_own_property_descriptor_fn.release()));
    reflect_obj->set_property("defineProperty", Value(define_property_fn.release()));
    
    ctx.register_built_in_object("Reflect", reflect_obj.release());
}

Object* Reflect::to_object(const Value& value, Context& ctx) {
    if (value.is_object()) {
        return value.as_object();
    }
    
    ctx.throw_exception(Value(std::string("TypeError: Reflect operation requires an object")));
    return nullptr;
}

std::string Reflect::to_property_key(const Value& value) {
    return value.to_string();
}

PropertyDescriptor Reflect::to_property_descriptor(const Value& value) {
    return PropertyDescriptor(value);
}

Value Reflect::from_property_descriptor(const PropertyDescriptor& desc) {
    auto desc_obj = ObjectFactory::create_object();
    desc_obj->set_property("value", desc.get_value());
    desc_obj->set_property("writable", Value(desc.is_writable()));
    desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
    desc_obj->set_property("configurable", Value(desc.is_configurable()));
    return Value(desc_obj.release());
}

}
