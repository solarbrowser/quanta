/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "ObjectBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/Parser.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include <cmath>
#include <sstream>
#include <algorithm>

namespace Quanta {

void register_object_builtins(Context& ctx) {
    auto object_constructor = ObjectFactory::create_native_constructor("Object",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }
            
            Value value = args[0];
            
            if (value.is_null() || value.is_undefined()) {
                return Value(ObjectFactory::create_object().release());
            }
            
            if (value.is_object() || value.is_function()) {
                return value;
            }
            
            if (value.is_string()) {
                auto string_obj = ObjectFactory::create_string(value.to_string());
                return Value(string_obj.release());
            } else if (value.is_number()) {
                auto number_obj = ObjectFactory::create_object();
                number_obj->set_property("[[PrimitiveValue]]", Value(value.as_number()));
                Value num_ctor = ctx.get_binding("Number");
                if (num_ctor.is_function()) {
                    Value num_proto = static_cast<Object*>(num_ctor.as_function())->get_property("prototype");
                    if (num_proto.is_object()) {
                        number_obj->set_prototype(num_proto.as_object());
                    }
                }
                return Value(number_obj.release());
            } else if (value.is_boolean()) {
                auto boolean_obj = ObjectFactory::create_boolean(value.to_boolean());
                return Value(boolean_obj.release());
            } else if (value.is_symbol()) {
                // ES6: Create a Symbol wrapper object
                Symbol* sym = value.as_symbol();
                auto symbol_obj = ObjectFactory::create_object();
                // Set Symbol.prototype for instanceof
                Value sym_ctor = ctx.get_binding("Symbol");
                if (sym_ctor.is_function()) {
                    Value sym_proto = static_cast<Object*>(sym_ctor.as_function())->get_property("prototype");
                    if (sym_proto.is_object()) {
                        symbol_obj->set_prototype(sym_proto.as_object());
                    }
                }
                // Store primitive symbol for valueOf
                Value captured_sym = value;
                auto valueOf_fn = ObjectFactory::create_native_function("valueOf",
                    [captured_sym](Context& /* ctx */, const std::vector<Value>& /* args */) -> Value {
                        return captured_sym;
                    }, 0);
                symbol_obj->set_property("valueOf", Value(valueOf_fn.release()), PropertyAttributes::BuiltinFunction);
                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [sym](Context& /* ctx */, const std::vector<Value>& /* args */) -> Value {
                        return Value(sym->to_string());
                    }, 0);
                symbol_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
                return Value(symbol_obj.release());
            } else if (value.is_bigint()) {
                auto bigint_obj = ObjectFactory::create_object();
                bigint_obj->set_property("valueOf", value);
                return Value(bigint_obj.release());
            }
            
            return Value(ObjectFactory::create_object().release());
        });
    
    auto keys_fn = ObjectFactory::create_native_function("keys", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.keys requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            // ES6: Accept primitives — wrap strings to String objects
            if (args[0].is_string()) {
                std::string str = args[0].to_string();
                auto result = ObjectFactory::create_array();
                for (uint32_t i = 0; i < str.length(); i++) {
                    result->set_element(i, Value(std::to_string(i)));
                }
                result->set_property("length", Value(static_cast<double>(str.length())));
                return Value(result.release());
            }
            if (!args[0].is_object() && !args[0].is_function()) {
                return Value(ObjectFactory::create_array().release());
            }

            Object* obj = args[0].is_function() ?
                static_cast<Object*>(args[0].as_function()) :
                args[0].as_object();

            std::vector<std::string> raw_keys;
            if (obj->get_type() == Object::ObjectType::Proxy) {
                try {
                    raw_keys = static_cast<Proxy*>(obj)->own_keys_trap();
                } catch (const std::runtime_error&) {
                    if (!ctx.has_exception()) {
                        ctx.throw_type_error("'ownKeys' proxy invariant violated");
                    }
                    return Value();
                }
                if (ctx.has_exception()) return Value();
            } else {
                raw_keys = obj->get_enumerable_keys();
            }

            // Filter out symbol-keyed properties
            std::vector<std::string> filtered;
            for (const auto& k : raw_keys) {
                if (k.find("@@sym:") != 0 && k.find("Symbol.") != 0) {
                    filtered.push_back(k);
                }
            }

            auto result_array = ObjectFactory::create_array(filtered.size());
            if (!result_array) {
                result_array = ObjectFactory::create_array(0);
            }

            for (size_t i = 0; i < filtered.size(); i++) {
                result_array->set_element(i, Value(filtered[i]));
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("keys", Value(keys_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto values_fn = ObjectFactory::create_native_function("values", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.values requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.values called on non-object")));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                Value value = obj->get_property(keys[i]);
                result_array->set_element(i, value);
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("values", Value(values_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto entries_fn = ObjectFactory::create_native_function("entries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.entries requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.entries called on non-object")));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                auto pair_array = ObjectFactory::create_array(2);
                pair_array->set_element(0, Value(keys[i]));
                pair_array->set_element(1, obj->get_property(keys[i]));
                result_array->set_element(i, Value(pair_array.release()));
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("entries", Value(entries_fn.release()), PropertyAttributes::BuiltinFunction);

    auto is_fn = ObjectFactory::create_native_function("is",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;

            Value x = args.size() > 0 ? args[0] : Value();
            Value y = args.size() > 1 ? args[1] : Value();

            return Value(x.same_value(y));
        }, 2);

    PropertyDescriptor is_length_desc(Value(2.0), PropertyAttributes::Configurable);
    is_length_desc.set_enumerable(false);
    is_length_desc.set_writable(false);
    is_fn->set_property_descriptor("length", is_length_desc);
    object_constructor->set_property("is", Value(is_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto fromEntries_fn = ObjectFactory::create_native_function("fromEntries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.fromEntries requires at least 1 argument")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.fromEntries called on non-object")));
                return Value();
            }
            
            Object* iterable = args[0].as_object();
            auto result_obj = ObjectFactory::create_object();

            auto process_entry = [&](Value entry) {
                if (!entry.is_object()) return;
                Object* pair = entry.as_object();
                Value key = pair->get_element(0);
                Value val = pair->get_element(1);
                result_obj->set_property(key.to_string(), val);
            };

            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            Value iter_method = iterable->get_property(iter_sym->to_property_key());
            if (iter_method.is_function()) {
                Value iterator_obj = iter_method.as_function()->call(ctx, {}, Value(iterable));
                if (iterator_obj.is_object()) {
                    Object* iterator = iterator_obj.as_object();
                    Value next_method = iterator->get_property("next");
                    if (next_method.is_function()) {
                        while (true) {
                            Value result = next_method.as_function()->call(ctx, {}, iterator_obj);
                            if (!result.is_object()) break;
                            if (result.as_object()->get_property("done").to_boolean()) break;
                            process_entry(result.as_object()->get_property("value"));
                        }
                    }
                }
            } else {
                uint32_t length = iterable->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    process_entry(iterable->get_element(i));
                }
            }

            return Value(result_obj.release());
        }, 1);
    object_constructor->set_property("fromEntries", Value(fromEntries_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_type_error("Object.create requires at least 1 argument");
                return Value();
            }

            Object* new_obj_ptr = nullptr;

            if (args[0].is_null()) {
                auto new_obj = ObjectFactory::create_object();
                if (!new_obj) {
                    ctx.throw_exception(Value(std::string("Error: Failed to create object")));
                    return Value();
                }
                new_obj->set_prototype(nullptr);  // Set prototype to null
                new_obj_ptr = new_obj.release();
            }
            else if (args[0].is_object()) {
                Object* prototype = args[0].as_object();
                auto new_obj = ObjectFactory::create_object(prototype);
                if (!new_obj) {
                    ctx.throw_exception(Value(std::string("Error: Failed to create object with prototype")));
                    return Value();
                }
                new_obj_ptr = new_obj.release();
            }
            else {
                ctx.throw_type_error("Object prototype may only be an Object or null");
                return Value();
            }

            if (args.size() > 1 && !args[1].is_undefined()) {
                if (!args[1].is_object()) {
                    ctx.throw_type_error("Property descriptors must be an object");
                    return Value();
                }

                Object* properties = args[1].as_object();
                auto prop_names = properties->get_own_property_keys();

                for (const auto& prop_name : prop_names) {
                    Value descriptor_val = properties->get_property(prop_name);
                    if (!descriptor_val.is_object()) {
                        continue;
                    }

                    Object* desc = descriptor_val.as_object();
                    PropertyDescriptor prop_desc;

                    if (desc->has_own_property("get")) {
                        Value getter = desc->get_property("get");
                        if (getter.is_function()) {
                            prop_desc.set_getter(getter.as_object());
                        }
                    }
                    if (desc->has_own_property("set")) {
                        Value setter = desc->get_property("set");
                        if (setter.is_function()) {
                            prop_desc.set_setter(setter.as_object());
                        }
                    }

                    if (desc->has_own_property("value")) {
                        prop_desc.set_value(desc->get_property("value"));
                    }
                    if (desc->has_own_property("writable")) {
                        prop_desc.set_writable(desc->get_property("writable").to_boolean());
                    } else {
                        prop_desc.set_writable(false);
                    }

                    if (desc->has_own_property("enumerable")) {
                        prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                    } else {
                        prop_desc.set_enumerable(false);
                    }
                    if (desc->has_own_property("configurable")) {
                        prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                    } else {
                        prop_desc.set_configurable(false);
                    }

                    new_obj_ptr->set_property_descriptor(prop_name, prop_desc);
                }
            }

            return Value(new_obj_ptr);
        }, 2);
    object_constructor->set_property("create", Value(create_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto assign_fn = ObjectFactory::create_native_function("assign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.assign requires at least one argument")));
                return Value();
            }
            
            Value target = args[0];
            if (!target.is_object()) {
                if (target.is_null() || target.is_undefined()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                    return Value();
                }
                auto obj = ObjectFactory::create_object();
                obj->set_property("valueOf", Value(target));
                target = Value(obj.release());
            }
            
            Object* target_obj = target.as_object();
            
            for (size_t i = 1; i < args.size(); i++) {
                Value source = args[i];
                if (source.is_null() || source.is_undefined()) {
                    continue;
                }
                
                if (source.is_object()) {
                    Object* source_obj = source.as_object();
                    bool source_is_proxy = (source_obj->get_type() == Object::ObjectType::Proxy);
                    std::vector<std::string> property_keys;
                    if (source_is_proxy) {
                        property_keys = static_cast<Proxy*>(source_obj)->own_keys_trap();
                    } else {
                        property_keys = source_obj->get_own_property_keys();
                    }

                    for (const std::string& prop : property_keys) {
                        Value value;
                        bool is_enumerable = true;
                        if (source_is_proxy) {
                            // Spec: Object.assign calls [[GetOwnProperty]] then [[Get]] for each key
                            Proxy* source_proxy = static_cast<Proxy*>(source_obj);
                            PropertyDescriptor desc = source_proxy->get_own_property_descriptor_trap(Value(prop));
                            if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) continue;
                            if (!desc.is_enumerable()) continue;
                            value = source_proxy->get_trap(Value(prop));
                        } else {
                            PropertyDescriptor desc = source_obj->get_property_descriptor(prop);
                            if (!desc.is_enumerable() && desc.has_value()) {
                                is_enumerable = false;
                            }
                            value = source_obj->get_property(prop);
                        }
                        if (!is_enumerable) continue;
                        // Use [[Set]] semantics: invoke setter if target has accessor
                        PropertyDescriptor target_desc = target_obj->get_property_descriptor(prop);
                        if (target_desc.is_accessor_descriptor() && target_desc.has_setter()) {
                            Function* setter = dynamic_cast<Function*>(target_desc.get_setter());
                            if (setter) {
                                setter->call(ctx, {value}, Value(target_obj));
                            }
                        } else {
                            target_obj->set_property(prop, value);
                        }
                    }
                }
            }
            
            return target;
        }, 2);
    object_constructor->set_property("assign", Value(assign_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getPrototypeOf_fn = ObjectFactory::create_native_function("getPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getPrototypeOf requires an argument")));
                return Value();
            }

            Value obj_val = args[0];
            
            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }

            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                if (obj_val.is_string()) {
                    Value string_ctor = ctx.get_binding("String");
                    if (string_ctor.is_function()) {
                        Function* str_fn = string_ctor.as_function();
                        Value proto = str_fn->get_property("prototype");
                        return proto;
                    }
                } else if (obj_val.is_number()) {
                    Value number_ctor = ctx.get_binding("Number");
                    if (number_ctor.is_function()) {
                        Function* num_fn = number_ctor.as_function();
                        Value proto = num_fn->get_property("prototype");
                        return proto;
                    }
                } else if (obj_val.is_boolean()) {
                    Value boolean_ctor = ctx.get_binding("Boolean");
                    if (boolean_ctor.is_function()) {
                        Function* bool_fn = boolean_ctor.as_function();
                        Value proto = bool_fn->get_property("prototype");
                        return proto;
                    }
                }
                return Value::null();
            }

            // For Proxy, call getPrototypeOf trap
            if (obj->get_type() == Object::ObjectType::Proxy) {
                return static_cast<Proxy*>(obj)->get_prototype_of_trap();
            }

            Object* proto = obj->get_prototype();
            if (proto) {
                Function* func_proto = dynamic_cast<Function*>(proto);
                if (func_proto) {
                    return Value(func_proto);
                }
                return Value(proto);
            }

            return Value::null();
        }, 1);
    object_constructor->set_property("getPrototypeOf", Value(getPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);

    auto setPrototypeOf_fn = ObjectFactory::create_native_function("setPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value(std::string("TypeError: Object.setPrototypeOf requires 2 arguments")));
                return Value();
            }

            Value obj_val = args[0];
            Value proto_val = args[1];

            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }

            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Object.setPrototypeOf called on non-object")));
                return Value();
            }

            // For Proxy, call setPrototypeOf trap
            if (obj->get_type() == Object::ObjectType::Proxy) {
                Object* new_proto = proto_val.is_null() ? nullptr :
                    proto_val.is_object() ? proto_val.as_object() :
                    proto_val.is_function() ? static_cast<Object*>(proto_val.as_function()) : nullptr;
                static_cast<Proxy*>(obj)->set_prototype_of_trap(new_proto);
                return obj_val;
            }

            if (proto_val.is_null()) {
                obj->set_prototype(nullptr);
            } else if (proto_val.is_object()) {
                obj->set_prototype(proto_val.as_object());
            } else if (proto_val.is_function()) {
                obj->set_prototype(proto_val.as_function());
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Object prototype may only be an Object or null")));
                return Value();
            }

            return obj_val;
        }, 2);
    object_constructor->set_property("setPrototypeOf", Value(setPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyDescriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyDescriptor requires 2 arguments")));
                return Value();
            }

            if (!args[0].is_object() && !args[0].is_function()) {
                return Value();
            }

            Object* obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();

            std::string prop_name = args[1].to_property_key();

            PropertyDescriptor desc;
            if (obj->get_type() == Object::ObjectType::Proxy) {
                desc = static_cast<Proxy*>(obj)->get_own_property_descriptor_trap(Value(prop_name));
            } else {
                desc = obj->get_property_descriptor(prop_name);
            }

            if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) {
                if (!obj->has_own_property(prop_name)) {
                    return Value();
                }

                auto descriptor = ObjectFactory::create_object();
                Value prop_value = obj->get_property(prop_name);
                descriptor->set_property("value", prop_value);
                descriptor->set_property("writable", Value(true));
                descriptor->set_property("enumerable", Value(true));
                descriptor->set_property("configurable", Value(true));
                return Value(descriptor.release());
            }

            auto descriptor = ObjectFactory::create_object();

            if (desc.is_data_descriptor()) {
                descriptor->set_property("value", desc.get_value());
                descriptor->set_property("writable", Value(desc.is_writable()));
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            } else if (desc.is_generic_descriptor()) {
                descriptor->set_property("value", Value());
                descriptor->set_property("writable", Value(desc.is_writable()));
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            } else if (desc.is_accessor_descriptor()) {
                if (desc.has_getter()) {
                    Object* getter = desc.get_getter();
                    if (getter && getter->is_function()) {
                        descriptor->set_property("get", Value(static_cast<Function*>(getter)));
                    } else {
                        descriptor->set_property("get", Value(getter));
                    }
                } else {
                    descriptor->set_property("get", Value());
                }
                if (desc.has_setter()) {
                    Object* setter = desc.get_setter();
                    if (setter && setter->is_function()) {
                        descriptor->set_property("set", Value(static_cast<Function*>(setter)));
                    } else {
                        descriptor->set_property("set", Value(setter));
                    }
                } else {
                    descriptor->set_property("set", Value());
                }
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            }

            return Value(descriptor.release());
        }, 2);
    object_constructor->set_property("getOwnPropertyDescriptor", Value(getOwnPropertyDescriptor_fn.release()), PropertyAttributes::BuiltinFunction);

    auto defineProperty_fn = ObjectFactory::create_native_function("defineProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 3) {
                ctx.throw_type_error("Object.defineProperty requires 3 arguments");
                return Value();
            }

            if (!args[0].is_object_like()) {
                ctx.throw_type_error("Object.defineProperty called on non-object");
                return Value();
            }

            Object* obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();
            std::string prop_name;
            if (args[1].is_symbol()) {
                prop_name = args[1].as_symbol()->to_property_key();
            } else {
                prop_name = args[1].to_string();
            }

            if (args[2].is_object()) {
                Object* desc = args[2].as_object();

                PropertyDescriptor prop_desc;

                if (desc->has_own_property("get")) {
                    Value getter = desc->get_property("get");
                    if (getter.is_function()) {
                        prop_desc.set_getter(getter.as_object());
                    }
                }

                if (desc->has_own_property("set")) {
                    Value setter = desc->get_property("set");
                    if (setter.is_function()) {
                        prop_desc.set_setter(setter.as_object());
                    }
                }

                if (desc->has_own_property("value")) {
                    Value value = desc->get_property("value");
                    prop_desc.set_value(value);
                }

                if (desc->has_own_property("writable")) {
                    prop_desc.set_writable(desc->get_property("writable").to_boolean());
                } else {
                    prop_desc.set_writable(false);
                }

                if (desc->has_own_property("enumerable")) {
                    prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                } else {
                    prop_desc.set_enumerable(false);
                }

                if (desc->has_own_property("configurable")) {
                    prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                } else {
                    prop_desc.set_configurable(false);
                }

                bool success;
                if (obj->get_type() == Object::ObjectType::Proxy) {
                    success = static_cast<Proxy*>(obj)->define_property_trap(Value(prop_name), prop_desc);
                } else {
                    success = obj->set_property_descriptor(prop_name, prop_desc);
                }
                if (!success) {
                    ctx.throw_type_error("Cannot define property");
                    return Value();
                }
            }

            return args[0];
        }, 3);
    object_constructor->set_property("defineProperty", Value(defineProperty_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyNames_fn = ObjectFactory::create_native_function("getOwnPropertyNames",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyNames requires 1 argument")));
                return Value();
            }

            // ES6: Accept primitives — wrap strings to String objects
            if (args[0].is_string()) {
                std::string str = args[0].to_string();
                auto result = ObjectFactory::create_array();
                uint32_t idx = 0;
                for (uint32_t i = 0; i < str.length(); i++) {
                    result->set_element(idx++, Value(std::to_string(i)));
                }
                result->set_element(idx++, Value(std::string("length")));
                result->set_property("length", Value(static_cast<double>(idx)));
                return Value(result.release());
            }
            if (!args[0].is_object() && !args[0].is_function()) {
                return Value(ObjectFactory::create_array().release());
            }

            Object* obj = args[0].is_function() ?
                static_cast<Object*>(args[0].as_function()) :
                args[0].as_object();
            auto result = ObjectFactory::create_array();

            std::vector<std::string> props;
            if (obj->get_type() == Object::ObjectType::Proxy) {
                props = static_cast<Proxy*>(obj)->own_keys_trap();
            } else {
                props = obj->get_own_property_keys();
            }
            uint32_t result_index = 0;
            for (size_t i = 0; i < props.size(); i++) {
                // Skip symbol-keyed properties
                if (props[i].find("@@sym:") == 0 || props[i].find("Symbol.") == 0) continue;
                result->set_element(result_index++, Value(props[i]));
            }
            result->set_property("length", Value(static_cast<double>(result_index)));

            return Value(result.release());
        }, 1);
    object_constructor->set_property("getOwnPropertyNames", Value(getOwnPropertyNames_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: Object.getOwnPropertySymbols
    auto getOwnPropertySymbols_fn = ObjectFactory::create_native_function("getOwnPropertySymbols",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("Object.getOwnPropertySymbols requires 1 argument");
                return Value();
            }
            if (!args[0].is_object() && !args[0].is_function()) {
                return Value(ObjectFactory::create_array().release());
            }
            Object* obj = args[0].is_function()
                ? static_cast<Object*>(args[0].as_function())
                : args[0].as_object();
            auto result = ObjectFactory::create_array();
            auto props = obj->get_own_property_keys();
            uint32_t idx = 0;
            for (const auto& key : props) {
                if (key.find("@@sym:") == 0) {
                    // Find the symbol by its property key
                    Value prop_val = obj->get_property(key);
                    // We need to return the Symbol object, look it up from registry
                    Symbol* sym = Symbol::find_by_property_key(key);
                    if (sym) {
                        result->set_element(idx++, Value(sym));
                    }
                }
            }
            result->set_property("length", Value(static_cast<double>(idx)));
            return Value(result.release());
        }, 1);
    object_constructor->set_property("getOwnPropertySymbols", Value(getOwnPropertySymbols_fn.release()), PropertyAttributes::BuiltinFunction);

    auto defineProperties_fn = ObjectFactory::create_native_function("defineProperties",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("Object.defineProperties requires 2 arguments");
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_type_error("Object.defineProperties called on non-object");
                return Value();
            }

            Object* obj = args[0].as_object();

            if (!args[1].is_object()) {
                ctx.throw_type_error("Properties argument must be an object");
                return Value();
            }

            Object* properties = args[1].as_object();
            // For Proxy, use ownKeys trap so the target's own keys are enumerated
            std::vector<std::string> prop_names;
            if (properties->get_type() == Object::ObjectType::Proxy) {
                prop_names = static_cast<Proxy*>(properties)->own_keys_trap();
            } else {
                prop_names = properties->get_own_property_keys();
            }

            for (const auto& prop_name : prop_names) {
                Value descriptor_val = properties->get_property(prop_name);
                if (!descriptor_val.is_object()) {
                    continue;
                }

                Object* desc = descriptor_val.as_object();
                PropertyDescriptor prop_desc;

                if (desc->has_own_property("get")) {
                    Value getter = desc->get_property("get");
                    if (getter.is_function()) {
                        prop_desc.set_getter(getter.as_object());
                    }
                }
                if (desc->has_own_property("set")) {
                    Value setter = desc->get_property("set");
                    if (setter.is_function()) {
                        prop_desc.set_setter(setter.as_object());
                    }
                }

                if (desc->has_own_property("value")) {
                    prop_desc.set_value(desc->get_property("value"));
                }
                if (desc->has_own_property("writable")) {
                    prop_desc.set_writable(desc->get_property("writable").to_boolean());
                } else {
                    prop_desc.set_writable(false);
                }

                if (desc->has_own_property("enumerable")) {
                    prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                } else {
                    prop_desc.set_enumerable(false);
                }
                if (desc->has_own_property("configurable")) {
                    prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                } else {
                    prop_desc.set_configurable(false);
                }

                obj->set_property_descriptor(prop_name, prop_desc);
            }

            return args[0];
        }, 2);
    object_constructor->set_property("defineProperties", Value(defineProperties_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyDescriptors_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptors",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyDescriptors requires 1 argument")));
                return Value();
            }

            if (!args[0].is_object()) {
                return Value(ObjectFactory::create_object().release());
            }

            Object* obj = args[0].as_object();
            auto result = ObjectFactory::create_object();
            auto prop_names = obj->get_own_property_keys();

            for (const auto& prop_name : prop_names) {
                PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
                auto descriptor = ObjectFactory::create_object();

                if (desc.is_data_descriptor()) {
                    descriptor->set_property("value", desc.get_value());
                    descriptor->set_property("writable", Value(desc.is_writable()));
                    descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                    descriptor->set_property("configurable", Value(desc.is_configurable()));
                } else if (desc.is_accessor_descriptor()) {
                    if (desc.has_getter()) {
                        descriptor->set_property("get", Value(desc.get_getter()));
                    } else {
                        descriptor->set_property("get", Value());
                    }
                    if (desc.has_setter()) {
                        descriptor->set_property("set", Value(desc.get_setter()));
                    } else {
                        descriptor->set_property("set", Value());
                    }
                    descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                    descriptor->set_property("configurable", Value(desc.is_configurable()));
                } else {
                    Value prop_value = obj->get_property(prop_name);
                    descriptor->set_property("value", prop_value);
                    descriptor->set_property("writable", Value(true));
                    descriptor->set_property("enumerable", Value(true));
                    descriptor->set_property("configurable", Value(true));
                }

                result->set_property(prop_name, Value(descriptor.release()));
            }

            return Value(result.release());
        }, 1);
    object_constructor->set_property("getOwnPropertyDescriptors", Value(getOwnPropertyDescriptors_fn.release()), PropertyAttributes::BuiltinFunction);

    auto seal_fn = ObjectFactory::create_native_function("seal",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            obj->seal();

            return args[0];
        }, 1);
    object_constructor->set_property("seal", Value(seal_fn.release()), PropertyAttributes::BuiltinFunction);

    auto freeze_fn = ObjectFactory::create_native_function("freeze",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            if (obj->get_type() == Object::ObjectType::Proxy) {
                // SetIntegrityLevel("frozen") via proxy traps
                Proxy* proxy = static_cast<Proxy*>(obj);
                proxy->prevent_extensions_trap();
                if (ctx.has_exception()) return Value();
                std::vector<std::string> keys = proxy->own_keys_trap();
                if (ctx.has_exception()) return Value();
                Object* target = proxy->get_proxy_target();
                for (const auto& key : keys) {
                    PropertyDescriptor desc;
                    desc.set_configurable(false);
                    // For data properties also set writable:false; for accessors only configurable:false
                    if (target) {
                        PropertyDescriptor td = target->get_property_descriptor(key);
                        if (!td.is_accessor_descriptor()) {
                            desc.set_writable(false);
                        }
                    } else {
                        desc.set_writable(false);
                    }
                    proxy->define_property_trap(Value(key), desc);
                    if (ctx.has_exception()) return Value();
                }
            } else {
                obj->freeze();
            }

            return args[0];
        }, 1);
    object_constructor->set_property("freeze", Value(freeze_fn.release()), PropertyAttributes::BuiltinFunction);

    auto preventExtensions_fn = ObjectFactory::create_native_function("preventExtensions",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            if (obj->get_type() == Object::ObjectType::Proxy) {
                static_cast<Proxy*>(obj)->prevent_extensions_trap();
            } else {
                obj->prevent_extensions();
            }

            return args[0];
        }, 1);
    object_constructor->set_property("preventExtensions", Value(preventExtensions_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isSealed_fn = ObjectFactory::create_native_function("isSealed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value(true);

            Object* obj = args[0].as_object();
            return Value(obj->is_sealed());
        }, 1);
    object_constructor->set_property("isSealed", Value(isSealed_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isFrozen_fn = ObjectFactory::create_native_function("isFrozen",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value(true);

            Object* obj = args[0].as_object();
            if (obj->get_type() == Object::ObjectType::Proxy) {
                // TestIntegrityLevel("frozen") via proxy traps
                Proxy* proxy = static_cast<Proxy*>(obj);
                if (proxy->is_extensible_trap()) return Value(false);
                if (ctx.has_exception()) return Value();
                std::vector<std::string> keys = proxy->own_keys_trap();
                if (ctx.has_exception()) return Value();
                for (const auto& key : keys) {
                    PropertyDescriptor desc = proxy->get_own_property_descriptor_trap(Value(key));
                    if (ctx.has_exception()) return Value();
                    if (desc.is_configurable()) return Value(false);
                    if (desc.is_data_descriptor() && desc.is_writable()) return Value(false);
                }
                return Value(true);
            }
            return Value(obj->is_frozen());
        }, 1);
    object_constructor->set_property("isFrozen", Value(isFrozen_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isExtensible_fn = ObjectFactory::create_native_function("isExtensible",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) return Value(false);
            Object* obj = args[0].as_object();
            if (obj->get_type() == Object::ObjectType::Proxy) {
                return Value(static_cast<Proxy*>(obj)->is_extensible_trap());
            }
            return Value(obj->is_extensible());
        }, 1);
    object_constructor->set_property("isExtensible", Value(isExtensible_fn.release()), PropertyAttributes::BuiltinFunction);

    auto hasOwn_fn = ObjectFactory::create_native_function("hasOwn",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) return Value(false);

            if (args[0].is_null() || args[0].is_undefined()) {
                ctx.throw_type_error("Cannot convert undefined or null to object");
                return Value();
            }

            if (!args[0].is_object()) return Value(false);

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            return Value(obj->has_own_property(prop_name));
        }, 2);
    object_constructor->set_property("hasOwn", Value(hasOwn_fn.release()), PropertyAttributes::BuiltinFunction);

    auto groupBy_fn = ObjectFactory::create_native_function("groupBy",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("Object.groupBy requires an iterable");
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_type_error("Object.groupBy expects an array");
                return Value();
            }

            if (args.size() < 2 || !args[1].is_function()) {
                ctx.throw_type_error("Object.groupBy requires a callback function");
                return Value();
            }

            Function* callback = args[1].as_function();
            auto result = ObjectFactory::create_object();
            uint32_t length = iterable->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), args[0] };
                Value key = callback->call(ctx, callback_args);
                std::string key_str = key.to_string();

                Value group = result->get_property(key_str);
                Object* group_array;
                if (group.is_object()) {
                    group_array = group.as_object();
                } else {
                    auto new_array = ObjectFactory::create_array();
                    group_array = new_array.get();
                    result->set_property(key_str, Value(new_array.release()));
                }

                uint32_t group_length = group_array->get_length();
                group_array->set_element(group_length, element);
                group_array->set_length(group_length + 1);
            }

            return Value(result.release());
        }, 2);
    object_constructor->set_property("groupBy", Value(groupBy_fn.release()), PropertyAttributes::BuiltinFunction);

    auto object_prototype = ObjectFactory::create_object();

    auto proto_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;

            Value this_val;

            try {
                this_val = ctx.get_binding("this");
            } catch (...) {
                Object* this_obj = ctx.get_this_binding();
                if (this_obj) {
                    this_val = Value(this_obj);
                }
            }


            if (this_val.is_undefined()) {
                return Value(std::string("[object Undefined]"));
            }
            if (this_val.is_null()) {
                return Value(std::string("[object Null]"));
            }

            std::string builtinTag;
            Object* tag_obj = nullptr;  // object to check Symbol.toStringTag on

            auto get_proto_from_global = [&ctx](const std::string& name) -> Object* {
                Value ctor = ctx.get_binding(name);
                if (ctor.is_function()) {
                    Value proto = ctor.as_function()->get_property("prototype");
                    if (proto.is_object()) return proto.as_object();
                }
                return nullptr;
            };

            if (this_val.is_string()) {
                builtinTag = "String";
                tag_obj = get_proto_from_global("String");
            } else if (this_val.is_number()) {
                builtinTag = "Number";
                tag_obj = get_proto_from_global("Number");
            } else if (this_val.is_boolean()) {
                builtinTag = "Boolean";
                tag_obj = get_proto_from_global("Boolean");
            } else if (this_val.is_object() || this_val.is_function()) {
                Object* this_obj = this_val.is_function()
                    ? static_cast<Object*>(this_val.as_function())
                    : this_val.as_object();
                tag_obj = this_obj;

                Object::ObjectType obj_type = this_obj->get_type();

                if (obj_type == Object::ObjectType::Arguments) {
                    builtinTag = "Arguments";
                } else if (this_obj->is_array()) {
                    builtinTag = "Array";
                } else if (obj_type == Object::ObjectType::String) {
                    builtinTag = "String";
                } else if (obj_type == Object::ObjectType::Number) {
                    builtinTag = "Number";
                } else if (obj_type == Object::ObjectType::Boolean) {
                    builtinTag = "Boolean";
                } else if (obj_type == Object::ObjectType::Date) {
                    builtinTag = "Date";
                } else if (obj_type == Object::ObjectType::RegExp) {
                    builtinTag = "RegExp";
                } else if (obj_type == Object::ObjectType::Error) {
                    builtinTag = "Error";
                } else if (obj_type == Object::ObjectType::Function || this_obj->is_function()) {
                    builtinTag = "Function";
                } else {
                    builtinTag = "Object";
                }
            } else {
                builtinTag = "Object";
            }

            // ES6: Check Symbol.toStringTag (overrides builtinTag)
            if (tag_obj) {
                Value tag = tag_obj->get_property("Symbol.toStringTag");
                if (tag.is_string()) {
                    builtinTag = tag.to_string();
                }
            }

            return Value("[object " + builtinTag + "]");
        });

    PropertyDescriptor toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    toString_name_desc.set_configurable(true);
    toString_name_desc.set_enumerable(false);
    toString_name_desc.set_writable(false);
    proto_toString_fn->set_property_descriptor("name", toString_name_desc);

    PropertyDescriptor toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    proto_toString_fn->set_property_descriptor("length", toString_length_desc);

    auto proto_hasOwnProperty_fn = ObjectFactory::create_native_function("hasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(false);
            }

            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: hasOwnProperty called on null or undefined")));
                return Value(false);
            }

            std::string prop_name;
            if (args[0].is_symbol()) {
                prop_name = args[0].as_symbol()->to_property_key();
            } else {
                prop_name = args[0].to_string();
            }
            // Spec: HasOwnProperty calls [[GetOwnProperty]], which fires getOwnPropertyDescriptor trap
            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                PropertyDescriptor desc = static_cast<Proxy*>(this_obj)->get_own_property_descriptor_trap(Value(prop_name));
                return Value(desc.is_data_descriptor() || desc.is_accessor_descriptor());
            }
            return Value(this_obj->has_own_property(prop_name));
        }, 1);

    PropertyDescriptor hasOwnProperty_name_desc(Value(std::string("hasOwnProperty")), PropertyAttributes::None);
    hasOwnProperty_name_desc.set_configurable(true);
    hasOwnProperty_name_desc.set_enumerable(false);
    hasOwnProperty_name_desc.set_writable(false);
    proto_hasOwnProperty_fn->set_property_descriptor("name", hasOwnProperty_name_desc);

    PropertyDescriptor hasOwnProperty_length_desc(Value(1.0), PropertyAttributes::Configurable);
    proto_hasOwnProperty_fn->set_property_descriptor("length", hasOwnProperty_length_desc);

    auto proto_isPrototypeOf_fn = ObjectFactory::create_native_function("isPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(false);
            }

            if (args.empty() || !args[0].is_object_like()) {
                return Value(false);
            }

            Object* obj = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
            
            Object* current = obj->get_prototype();
            while (current) {
                if (current == this_obj) {
                    return Value(true);
                }
                current = current->get_prototype();
            }
            
            return Value(false);
        });

    PropertyDescriptor isPrototypeOf_name_desc(Value(std::string("isPrototypeOf")), PropertyAttributes::None);
    isPrototypeOf_name_desc.set_configurable(true);
    isPrototypeOf_name_desc.set_enumerable(false);
    isPrototypeOf_name_desc.set_writable(false);
    proto_isPrototypeOf_fn->set_property_descriptor("name", isPrototypeOf_name_desc);

    PropertyDescriptor isPrototypeOf_length_desc(Value(1.0), PropertyAttributes::Configurable);
    isPrototypeOf_length_desc.set_enumerable(false);
    isPrototypeOf_length_desc.set_writable(false);
    proto_isPrototypeOf_fn->set_property_descriptor("length", isPrototypeOf_length_desc);

    auto proto_propertyIsEnumerable_fn = ObjectFactory::create_native_function("propertyIsEnumerable",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(false);
            }

            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: propertyIsEnumerable called on null or undefined")));
                return Value(false);
            }

            std::string prop_name = args[0].to_string();

            // Check if property exists and is own property
            if (!this_obj->has_own_property(prop_name)) {
                return Value(false);
            }

            // Check if property is enumerable
            PropertyDescriptor desc = this_obj->get_property_descriptor(prop_name);
            return Value(desc.is_enumerable());
        }, 1);

    PropertyDescriptor propertyIsEnumerable_name_desc(Value(std::string("propertyIsEnumerable")), PropertyAttributes::None);
    propertyIsEnumerable_name_desc.set_configurable(true);
    propertyIsEnumerable_name_desc.set_enumerable(false);
    propertyIsEnumerable_name_desc.set_writable(false);
    proto_propertyIsEnumerable_fn->set_property_descriptor("name", propertyIsEnumerable_name_desc);

    PropertyDescriptor propertyIsEnumerable_length_desc(Value(1.0), PropertyAttributes::Configurable);
    propertyIsEnumerable_length_desc.set_enumerable(false);
    propertyIsEnumerable_length_desc.set_writable(false);
    proto_propertyIsEnumerable_fn->set_property_descriptor("length", propertyIsEnumerable_length_desc);

    auto proto_valueOf_fn = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                return Value(this_obj);
            }
            return Value();
        }, 0);

    object_prototype->set_property("toString", Value(proto_toString_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("valueOf", Value(proto_valueOf_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("hasOwnProperty", Value(proto_hasOwnProperty_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("isPrototypeOf", Value(proto_isPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("propertyIsEnumerable", Value(proto_propertyIsEnumerable_fn.release()), PropertyAttributes::BuiltinFunction);

    auto obj_toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_object()) {
                Object* obj = this_value.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    Function* fn = toString_method.as_function();
                    std::vector<Value> empty_args;
                    return fn->call(ctx, empty_args, this_value);
                }
            }
            return Value(this_value.to_string());
        });
    object_prototype->set_property("toLocaleString", Value(obj_toLocaleString_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* object_proto_ptr = object_prototype.get();
    ObjectFactory::set_object_prototype(object_proto_ptr);

    PropertyDescriptor object_proto_ctor_desc(Value(object_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    object_proto_ptr->set_property_descriptor("constructor", object_proto_ctor_desc);

    // ES6 Annex B: Object.prototype.__proto__ accessor property
    auto proto_getter = ObjectFactory::create_native_function("get __proto__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                Object* proto = this_obj->get_prototype();
                if (proto) {
                    return Value(static_cast<Object*>(proto));
                }
                return Value::null();
            }
            return Value::null();
        }, 0);
    auto proto_setter = ObjectFactory::create_native_function("set __proto__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (this_obj && !args.empty()) {
                if (args[0].is_object()) {
                    this_obj->set_prototype(args[0].as_object());
                } else if (args[0].is_null()) {
                    this_obj->set_prototype(nullptr);
                }
            }
            return Value();
        }, 1);
    PropertyDescriptor proto_desc;
    proto_desc.set_getter(proto_getter.release());
    proto_desc.set_setter(proto_setter.release());
    proto_desc.set_enumerable(false);
    proto_desc.set_configurable(true);
    object_proto_ptr->set_property_descriptor("__proto__", proto_desc);

    // ES2017 Annex B: Object.prototype.__defineGetter__ / __defineSetter__
    auto define_getter_fn = ObjectFactory::create_native_function("__defineGetter__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // get_this_binding() returns nullptr when this was null/undefined (see Function::call)
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("Cannot convert undefined or null to object");
                return Value();
            }
            if (args.size() < 2 || !args[1].is_function()) {
                ctx.throw_type_error("__defineGetter__: getter must be a function");
                return Value();
            }
            std::string key = args[0].to_property_key();
            PropertyDescriptor desc;
            desc.set_getter(args[1].as_function());
            desc.set_enumerable(true);
            desc.set_configurable(true);
            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                static_cast<Proxy*>(this_obj)->define_property_trap(Value(key), desc);
            } else {
                this_obj->set_property_descriptor(key, desc);
            }
            return Value();
        }, 2);

    auto define_setter_fn = ObjectFactory::create_native_function("__defineSetter__",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // get_this_binding() returns nullptr when this was null/undefined (see Function::call)
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("Cannot convert undefined or null to object");
                return Value();
            }
            if (args.size() < 2 || !args[1].is_function()) {
                ctx.throw_type_error("__defineSetter__: setter must be a function");
                return Value();
            }
            std::string key = args[0].to_property_key();
            PropertyDescriptor desc;
            desc.set_setter(args[1].as_function());
            desc.set_enumerable(true);
            desc.set_configurable(true);
            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                static_cast<Proxy*>(this_obj)->define_property_trap(Value(key), desc);
            } else {
                this_obj->set_property_descriptor(key, desc);
            }
            return Value();
        }, 2);

    PropertyAttributes define_getter_setter_attrs = static_cast<PropertyAttributes>(
        PropertyAttributes::Writable | PropertyAttributes::Configurable);
    object_proto_ptr->set_property_descriptor("__defineGetter__",
        PropertyDescriptor(Value(define_getter_fn.release()), define_getter_setter_attrs));
    object_proto_ptr->set_property_descriptor("__defineSetter__",
        PropertyDescriptor(Value(define_setter_fn.release()), define_getter_setter_attrs));

    object_constructor->set_property("prototype", Value(object_prototype.release()), PropertyAttributes::None);

    ctx.get_global_object()->set_property("__addHasOwnProperty", Value(ObjectFactory::create_native_function("__addHasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value();

            Object* obj = args[0].as_object();
            auto hasOwn = ObjectFactory::create_native_function("hasOwnProperty",
                [obj](Context& ctx, const std::vector<Value>& args) -> Value {
                    if (args.empty()) return Value(false);
                    std::string prop = args[0].to_string();
                    return Value(obj->has_own_property(prop));
                });
            obj->set_property("hasOwnProperty", Value(hasOwn.release()));
            return args[0];
        }).release()));

    ctx.register_built_in_object("Object", object_constructor.release());
}

} // namespace Quanta
