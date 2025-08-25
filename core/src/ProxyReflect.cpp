/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "ProxyReflect.h"
#include "Context.h"
#include "../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

//=============================================================================
// Proxy Implementation
//=============================================================================

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
    
    // Default behavior
    return target_->get_property(key.to_string());
}

bool Proxy::set_trap(const Value& key, const Value& value) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.set) {
        return parsed_handler_.set(key, value);
    }
    
    // Default behavior
    return target_->set_property(key.to_string(), value);
}

bool Proxy::has_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.has) {
        return parsed_handler_.has(key);
    }
    
    // Default behavior
    return target_->has_property(key.to_string());
}

bool Proxy::delete_trap(const Value& key) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.deleteProperty) {
        return parsed_handler_.deleteProperty(key);
    }
    
    // Default behavior
    return target_->delete_property(key.to_string());
}

std::vector<std::string> Proxy::own_keys_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.ownKeys) {
        return parsed_handler_.ownKeys();
    }
    
    // Default behavior
    return target_->get_own_property_keys();
}

Value Proxy::get_prototype_of_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.getPrototypeOf) {
        return parsed_handler_.getPrototypeOf(Value());
    }
    
    // Default behavior
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
    
    // Default behavior
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
    
    // Default behavior
    return target_->is_extensible();
}

bool Proxy::prevent_extensions_trap() {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.preventExtensions) {
        return parsed_handler_.preventExtensions();
    }
    
    // Default behavior
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
    
    // Default behavior
    return target_->get_property_descriptor(key.to_string());
}

bool Proxy::define_property_trap(const Value& key, const PropertyDescriptor& desc) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.defineProperty) {
        return parsed_handler_.defineProperty(key, desc);
    }
    
    // Default behavior
    return target_->set_property_descriptor(key.to_string(), desc);
}

Value Proxy::apply_trap(const std::vector<Value>& args, const Value& this_value) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    if (parsed_handler_.apply) {
        return parsed_handler_.apply(args);
    }
    
    // Default behavior - call the target function
    if (target_->is_function()) {
        Function* func = static_cast<Function*>(target_);
        // Create a dummy context for the call
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
    
    // Default behavior - construct with the target function
    if (target_->is_function()) {
        Function* func = static_cast<Function*>(target_);
        // Create a dummy context for the call
        Context dummy_ctx(nullptr);
        return func->construct(dummy_ctx, args);
    }
    
    return Value();
}

void Proxy::parse_handler() {
    if (!handler_) {
        return;
    }
    
    // Basic trap parsing - enable get trap for now
    Value get_method = handler_->get_property("get");
    if (get_method.is_function()) {
        Function* get_fn = get_method.as_function();
        parsed_handler_.get = [get_fn, this](const Value& key) -> Value {
            // Try to call the get trap safely
            try {
                Context dummy_ctx(nullptr);
                std::vector<Value> args = {Value(target_), key};
                return get_fn->call(dummy_ctx, args);
            } catch (...) {
                // If trap fails, fall back to default behavior
                return target_->get_property(key.to_string());
            }
        };
    }
    
    // Value set_method = handler_->get_property("set");
    // if (set_method.is_function()) {
    //     Function* set_fn = set_method.as_function();
    //     parsed_handler_.set = [set_fn](const Value& key, const Value& value) -> bool {
    //         Context dummy_ctx(nullptr);
    //         Value result = set_fn->call(dummy_ctx, {key, value});
    //         return result.to_boolean();
    //     };
    // }
    
    // Value has_method = handler_->get_property("has");
    // if (has_method.is_function()) {
    //     Function* has_fn = has_method.as_function();
    //     parsed_handler_.has = [has_fn](const Value& key) -> bool {
    //         Context dummy_ctx(nullptr);
    //         Value result = has_fn->call(dummy_ctx, {key});
    //         return result.to_boolean();
    //     };
    // }
    
    // Parse other handler methods similarly...
}

void Proxy::revoke() {
    target_ = nullptr;
    handler_ = nullptr;
    parsed_handler_ = Handler{};
}

void Proxy::throw_if_revoked(Context& ctx) const {
    if (is_revoked()) {
        ctx.throw_exception(Value("TypeError: Proxy has been revoked"));
    }
}

//  PROXY OBJECT METHOD OVERRIDES - TRAP INTEGRATION 

Value Proxy::get_property(const std::string& key) const {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }
    
    // Use the get trap method which already handles traps properly
    return const_cast<Proxy*>(this)->get_trap(Value(key));
}


Value Proxy::proxy_constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value("TypeError: Proxy constructor requires target and handler arguments"));
        return Value();
    }
    
    if (!args[0].is_object() || !args[1].is_object()) {
        ctx.throw_exception(Value("TypeError: Proxy constructor requires object arguments"));
        return Value();
    }
    
    Object* target = args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    return Value(proxy.release());
}

Value Proxy::proxy_revocable(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value("TypeError: Proxy.revocable requires target and handler arguments"));
        return Value();
    }
    
    if (!args[0].is_object() || !args[1].is_object()) {
        ctx.throw_exception(Value("TypeError: Proxy.revocable requires object arguments"));
        return Value();
    }
    
    Object* target = args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    Proxy* proxy_ptr = proxy.get();
    
    // Create revoke function
    auto revoke_fn = ObjectFactory::create_native_function("revoke", 
        [proxy_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Unused parameter
            (void)args; // Unused parameter
            proxy_ptr->revoke();
            return Value();
        });
    
    // Create result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("proxy", Value(proxy.release()));
    result_obj->set_property("revoke", Value(revoke_fn.release()));
    
    return Value(result_obj.release());
}

void Proxy::setup_proxy(Context& ctx) {
    // Create Proxy constructor
    auto proxy_constructor_fn = ObjectFactory::create_native_function("Proxy", proxy_constructor);
    
    // Add Proxy.revocable
    auto revocable_fn = ObjectFactory::create_native_function("revocable", proxy_revocable);
    proxy_constructor_fn->set_property("revocable", Value(revocable_fn.release()));
    
    ctx.register_built_in_object("Proxy", proxy_constructor_fn.release());
}

//=============================================================================
// Reflect Implementation
//=============================================================================

Value Reflect::reflect_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value("TypeError: Reflect.get requires at least one argument"));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    std::string key = args.size() > 1 ? to_property_key(args[1]) : "";
    Value receiver = args.size() > 2 ? args[2] : args[0];
    
    (void)receiver; // Unused for now
    
    return target->get_property(key);
}

Value Reflect::reflect_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value("TypeError: Reflect.set requires at least two arguments"));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    Value value = args[2];
    Value receiver = args.size() > 3 ? args[3] : args[0];
    
    (void)receiver; // Unused for now
    
    bool result = target->set_property(key, value);
    return Value(result);
}

Value Reflect::reflect_has(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value("TypeError: Reflect.has requires two arguments"));
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
        ctx.throw_exception(Value("TypeError: Reflect.deleteProperty requires two arguments"));
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
        ctx.throw_exception(Value("TypeError: Reflect.ownKeys requires one argument"));
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    auto keys = target->get_own_property_keys();
    auto result_array = ObjectFactory::create_array(keys.size());
    
    for (size_t i = 0; i < keys.size(); ++i) {
        result_array->set_element(static_cast<uint32_t>(i), Value(keys[i]));
    }
    
    return Value(result_array.release());
}

Value Reflect::reflect_get_prototype_of(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value("TypeError: Reflect.getPrototypeOf requires one argument"));
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
        ctx.throw_exception(Value("TypeError: Reflect.setPrototypeOf requires two arguments"));
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
        ctx.throw_exception(Value("TypeError: Reflect.isExtensible requires one argument"));
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
        ctx.throw_exception(Value("TypeError: Reflect.preventExtensions requires one argument"));
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
        ctx.throw_exception(Value("TypeError: Reflect.apply requires three arguments"));
        return Value();
    }
    
    if (!args[0].is_function()) {
        ctx.throw_exception(Value("TypeError: Reflect.apply first argument must be a function"));
        return Value();
    }
    
    Function* target = args[0].as_function();
    Value this_value = args[1];
    
    // Convert arguments array to vector
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
        ctx.throw_exception(Value("TypeError: Reflect.construct requires at least two arguments"));
        return Value();
    }
    
    if (!args[0].is_function()) {
        ctx.throw_exception(Value("TypeError: Reflect.construct first argument must be a function"));
        return Value();
    }
    
    Function* target = args[0].as_function();
    
    // Convert arguments array to vector
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

void Reflect::setup_reflect(Context& ctx) {
    // Create Reflect object
    auto reflect_obj = ObjectFactory::create_object();
    
    // Add Reflect methods
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
    
    ctx.register_built_in_object("Reflect", reflect_obj.release());
}

Object* Reflect::to_object(const Value& value, Context& ctx) {
    if (value.is_object()) {
        return value.as_object();
    }
    
    ctx.throw_exception(Value("TypeError: Reflect operation requires an object"));
    return nullptr;
}

std::string Reflect::to_property_key(const Value& value) {
    return value.to_string();
}

PropertyDescriptor Reflect::to_property_descriptor(const Value& value) {
    // Simplified implementation
    return PropertyDescriptor(value);
}

Value Reflect::from_property_descriptor(const PropertyDescriptor& desc) {
    // Simplified implementation
    auto desc_obj = ObjectFactory::create_object();
    desc_obj->set_property("value", desc.get_value());
    desc_obj->set_property("writable", Value(desc.is_writable()));
    desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
    desc_obj->set_property("configurable", Value(desc.is_configurable()));
    return Value(desc_obj.release());
}

} // namespace Quanta