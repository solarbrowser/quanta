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
        // receiver = the proxy itself
        return parsed_handler_.get(key, Value(static_cast<Object*>(this)));
    }

    return target_->get_property(key.to_string());
}

bool Proxy::set_trap(const Value& key, const Value& value) {
    if (is_revoked()) {
        throw std::runtime_error("Proxy has been revoked");
    }

    if (parsed_handler_.set) {
        // receiver = the proxy itself
        return parsed_handler_.set(key, value, Value(static_cast<Object*>(this)));
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
        return parsed_handler_.getPrototypeOf();
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
        return parsed_handler_.apply(args, this_value);
    }

    if (target_->is_function()) {
        Function* func = static_cast<Function*>(target_);
        Context* ctx = Object::current_context_;
        if (ctx) return func->call(*ctx, args, this_value);
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
        Context* ctx = Object::current_context_;
        if (ctx) return func->construct(*ctx, args);
    }

    return Value();
}

bool Proxy::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    return set_trap(Value(key), value);
}

Object* Proxy::get_prototype() const {
    if (is_revoked()) throw std::runtime_error("Proxy has been revoked");
    if (parsed_handler_.getPrototypeOf) {
        Value result = parsed_handler_.getPrototypeOf();
        if (result.is_null()) return nullptr;
        if (result.is_object()) return result.as_object();
        if (result.is_function()) return result.as_function();
    }
    return target_ ? target_->get_prototype() : nullptr;
}

void Proxy::parse_handler() {
    if (!handler_) return;

    // --- get trap: handler(target, key, receiver) ---
    Value get_method = handler_->get_property("get");
    if (get_method.is_function()) {
        Function* fn = get_method.as_function();
        parsed_handler_.get = [fn, this](const Value& key, const Value& receiver) -> Value {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->get_property(key.to_string());
            std::vector<Value> args = {Value(target_), key, receiver};
            return fn->call(*ctx, args);
        };
    }

    // --- set trap: handler(target, key, value, receiver) ---
    Value set_method = handler_->get_property("set");
    if (set_method.is_function()) {
        Function* fn = set_method.as_function();
        parsed_handler_.set = [fn, this](const Value& key, const Value& value, const Value& receiver) -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->set_property(key.to_string(), value);
            std::vector<Value> args = {Value(target_), key, value, receiver};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- has trap: handler(target, key) ---
    Value has_method = handler_->get_property("has");
    if (has_method.is_function()) {
        Function* fn = has_method.as_function();
        parsed_handler_.has = [fn, this](const Value& key) -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->has_property(key.to_string());
            std::vector<Value> args = {Value(target_), key};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- deleteProperty trap: handler(target, key) ---
    Value delete_method = handler_->get_property("deleteProperty");
    if (delete_method.is_function()) {
        Function* fn = delete_method.as_function();
        parsed_handler_.deleteProperty = [fn, this](const Value& key) -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->delete_property(key.to_string());
            std::vector<Value> args = {Value(target_), key};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- ownKeys trap: handler(target) ---
    Value ownKeys_method = handler_->get_property("ownKeys");
    if (ownKeys_method.is_function()) {
        Function* fn = ownKeys_method.as_function();
        parsed_handler_.ownKeys = [fn, this]() -> std::vector<std::string> {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->get_own_property_keys();
            std::vector<Value> args = {Value(target_)};
            Value result = fn->call(*ctx, args);
            std::vector<std::string> keys;
            if (result.is_object()) {
                Object* arr = result.as_object();
                uint32_t len = arr->get_length();
                for (uint32_t i = 0; i < len; ++i) {
                    keys.push_back(arr->get_element(i).to_string());
                }
            }
            return keys;
        };
    }

    // --- getPrototypeOf trap: handler(target) ---
    Value getProto_method = handler_->get_property("getPrototypeOf");
    if (getProto_method.is_function()) {
        Function* fn = getProto_method.as_function();
        parsed_handler_.getPrototypeOf = [fn, this]() -> Value {
            Context* ctx = Object::current_context_;
            if (!ctx) {
                Object* p = target_->get_prototype();
                return p ? Value(p) : Value::null();
            }
            std::vector<Value> args = {Value(target_)};
            return fn->call(*ctx, args);
        };
    }

    // --- setPrototypeOf trap: handler(target, proto) ---
    Value setProto_method = handler_->get_property("setPrototypeOf");
    if (setProto_method.is_function()) {
        Function* fn = setProto_method.as_function();
        parsed_handler_.setPrototypeOf = [fn, this](Object* proto) -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) { target_->set_prototype(proto); return true; }
            Value proto_val = proto ? Value(proto) : Value::null();
            std::vector<Value> args = {Value(target_), proto_val};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- isExtensible trap: handler(target) ---
    Value isExt_method = handler_->get_property("isExtensible");
    if (isExt_method.is_function()) {
        Function* fn = isExt_method.as_function();
        parsed_handler_.isExtensible = [fn, this]() -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->is_extensible();
            std::vector<Value> args = {Value(target_)};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- preventExtensions trap: handler(target) ---
    Value prevExt_method = handler_->get_property("preventExtensions");
    if (prevExt_method.is_function()) {
        Function* fn = prevExt_method.as_function();
        parsed_handler_.preventExtensions = [fn, this]() -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) { target_->prevent_extensions(); return true; }
            std::vector<Value> args = {Value(target_)};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- getOwnPropertyDescriptor trap: handler(target, key) ---
    Value gopd_method = handler_->get_property("getOwnPropertyDescriptor");
    if (gopd_method.is_function()) {
        Function* fn = gopd_method.as_function();
        parsed_handler_.getOwnPropertyDescriptor = [fn, this](const Value& key) -> PropertyDescriptor {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->get_property_descriptor(key.to_string());
            std::vector<Value> args = {Value(target_), key};
            Value result = fn->call(*ctx, args);
            if (result.is_undefined() || result.is_null()) return PropertyDescriptor();
            if (!result.is_object()) return PropertyDescriptor();
            Object* desc_obj = result.as_object();
            PropertyDescriptor desc;
            if (desc_obj->has_own_property("value")) desc.set_value(desc_obj->get_property("value"));
            if (desc_obj->has_own_property("writable")) desc.set_writable(desc_obj->get_property("writable").to_boolean());
            if (desc_obj->has_own_property("enumerable")) desc.set_enumerable(desc_obj->get_property("enumerable").to_boolean());
            if (desc_obj->has_own_property("configurable")) desc.set_configurable(desc_obj->get_property("configurable").to_boolean());
            if (desc_obj->has_own_property("get")) {
                Value getter = desc_obj->get_property("get");
                if (getter.is_function()) desc.set_getter(getter.as_function());
            }
            if (desc_obj->has_own_property("set")) {
                Value setter = desc_obj->get_property("set");
                if (setter.is_function()) desc.set_setter(setter.as_function());
            }
            return desc;
        };
    }

    // --- defineProperty trap: handler(target, key, descriptor) ---
    Value defProp_method = handler_->get_property("defineProperty");
    if (defProp_method.is_function()) {
        Function* fn = defProp_method.as_function();
        parsed_handler_.defineProperty = [fn, this](const Value& key, const PropertyDescriptor& desc) -> bool {
            Context* ctx = Object::current_context_;
            if (!ctx) return target_->set_property_descriptor(key.to_string(), desc);
            auto desc_obj = ObjectFactory::create_object();
            if (desc.has_value()) desc_obj->set_property("value", desc.get_value());
            desc_obj->set_property("writable", Value(desc.is_writable()));
            desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
            desc_obj->set_property("configurable", Value(desc.is_configurable()));
            std::vector<Value> args = {Value(target_), key, Value(desc_obj.release())};
            Value result = fn->call(*ctx, args);
            return result.to_boolean();
        };
    }

    // --- apply trap: handler(target, thisArg, argumentsList) ---
    Value apply_method = handler_->get_property("apply");
    if (apply_method.is_function()) {
        Function* fn = apply_method.as_function();
        parsed_handler_.apply = [fn, this](const std::vector<Value>& call_args, const Value& this_val) -> Value {
            Context* ctx = Object::current_context_;
            if (!ctx) return Value();
            auto args_array = ObjectFactory::create_array(static_cast<int>(call_args.size()));
            for (size_t i = 0; i < call_args.size(); ++i)
                args_array->set_element(static_cast<uint32_t>(i), call_args[i]);
            std::vector<Value> args = {Value(target_), this_val, Value(args_array.release())};
            return fn->call(*ctx, args);
        };
    }

    // --- construct trap: handler(target, argumentsList, newTarget) ---
    Value construct_method = handler_->get_property("construct");
    if (construct_method.is_function()) {
        Function* fn = construct_method.as_function();
        parsed_handler_.construct = [fn, this](const std::vector<Value>& call_args) -> Value {
            Context* ctx = Object::current_context_;
            if (!ctx) return Value();
            auto args_array = ObjectFactory::create_array(static_cast<int>(call_args.size()));
            for (size_t i = 0; i < call_args.size(); ++i)
                args_array->set_element(static_cast<uint32_t>(i), call_args[i]);
            std::vector<Value> args = {Value(target_), Value(args_array.release()), Value(target_)};
            return fn->call(*ctx, args);
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
    
    if ((!args[0].is_object() && !args[0].is_function()) || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy constructor requires object arguments")));
        return Value();
    }

    Object* target = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    return Value(proxy.release());
}

Value Proxy::proxy_revocable(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy.revocable requires target and handler arguments")));
        return Value();
    }
    
    if ((!args[0].is_object() && !args[0].is_function()) || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Proxy.revocable requires object arguments")));
        return Value();
    }

    Object* target = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
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
    auto proxy_constructor_fn = ObjectFactory::create_native_constructor("Proxy", proxy_constructor);
    
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

    // If newTarget == target, use normal construct
    if (newTarget == target) {
        return target->construct(ctx, construct_args);
    }

    // Reflect.construct(target, args, newTarget): call target with new.target = newTarget
    // Create new object using newTarget's prototype
    auto new_object = ObjectFactory::create_object();
    Value nt_proto = newTarget->get_property("prototype");
    if (nt_proto.is_object()) new_object->set_prototype(nt_proto.as_object());

    bool was_in_constructor = ctx.is_in_constructor_call();
    Value old_new_target = ctx.get_new_target();

    ctx.set_in_constructor_call(true);
    ctx.set_new_target(Value(static_cast<Object*>(newTarget)));
    Value result = target->call(ctx, construct_args, Value(new_object.get()));
    ctx.set_in_constructor_call(was_in_constructor);
    ctx.set_new_target(old_new_target);

    if (!ctx.has_exception() && (result.is_object() || result.is_function())) {
        return result;
    }
    return Value(new_object.release());
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
