/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "object_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>

namespace Quanta {

void ObjectBuiltin::register_object_builtin(Context& ctx) {
    // Object constructor - create as a proper native function
    auto object_constructor = ObjectFactory::create_native_function("Object",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            // Object constructor implementation
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }

            Value value = args[0];

            // If value is null or undefined, return new empty object
            if (value.is_null() || value.is_undefined()) {
                return Value(ObjectFactory::create_object().release());
            }

            // If value is already an object or function, return it unchanged
            if (value.is_object() || value.is_function()) {
                return value;
            }

            // Convert primitive values to objects
            if (value.is_string()) {
                // Create String object wrapper
                auto string_obj = ObjectFactory::create_string(value.to_string());
                return Value(string_obj.release());
            } else if (value.is_number()) {
                // Create Number object wrapper (use generic object for now)
                auto number_obj = ObjectFactory::create_object();
                number_obj->set_property("valueOf", value);
                return Value(number_obj.release());
            } else if (value.is_boolean()) {
                // Create Boolean object wrapper
                auto boolean_obj = ObjectFactory::create_boolean(value.to_boolean());
                return Value(boolean_obj.release());
            } else if (value.is_symbol()) {
                // Create Symbol object wrapper (use generic object for now)
                auto symbol_obj = ObjectFactory::create_object();
                symbol_obj->set_property("valueOf", value);
                return Value(symbol_obj.release());
            } else if (value.is_bigint()) {
                // Create BigInt object wrapper (use generic object for now)
                auto bigint_obj = ObjectFactory::create_object();
                bigint_obj->set_property("valueOf", value);
                return Value(bigint_obj.release());
            }

            // Fallback: create empty object
            return Value(ObjectFactory::create_object().release());
        });

    // Add Object static methods (keys, values, entries, create, assign, etc.)
    add_object_static_methods(*object_constructor);

    // Create Object.prototype with instance methods
    auto object_prototype = ObjectFactory::create_object();
    add_object_prototype_methods(*object_prototype);

    // Set Object.prototype
    object_constructor->set_property("prototype", Value(object_prototype.release()));

    // Register the Object constructor globally
    ctx.register_built_in_object("Object", object_constructor.release());
}

void ObjectBuiltin::add_object_static_methods(Function& constructor) {
    // Object.keys(obj) - returns array of own enumerable property keys
    auto keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.keys requires at least 1 argument"));
                return Value();
            }

            // Check for null and undefined specifically
            if (args[0].is_null()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.keys called on non-object"));
                return Value();
            }

            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();  // Use direct method instead of enumerable

            auto result_array = ObjectFactory::create_array(keys.size());
            if (!result_array) {
                // Fallback: create empty array
                result_array = ObjectFactory::create_array(0);
            }

            for (size_t i = 0; i < keys.size(); i++) {
                result_array->set_element(i, Value(keys[i]));
            }

            return Value(result_array.release());
        });
    constructor.set_property("keys", Value(keys_fn.release()));

    // Object.values(obj) - returns array of own enumerable property values
    auto values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.values requires at least 1 argument"));
                return Value();
            }

            // Check for null and undefined specifically
            if (args[0].is_null()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.values called on non-object"));
                return Value();
            }

            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();  // Use direct method instead of enumerable

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                Value value = obj->get_property(keys[i]);
                result_array->set_element(i, value);
            }

            return Value(result_array.release());
        });
    constructor.set_property("values", Value(values_fn.release()));

    // Object.entries(obj) - returns array of [key, value] pairs
    auto entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.entries requires at least 1 argument"));
                return Value();
            }

            // Check for null and undefined specifically
            if (args[0].is_null()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.entries called on non-object"));
                return Value();
            }

            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                // Create [key, value] pair array
                auto pair_array = ObjectFactory::create_array(2);
                pair_array->set_element(0, Value(keys[i]));
                pair_array->set_element(1, obj->get_property(keys[i]));
                result_array->set_element(i, Value(pair_array.release()));
            }

            return Value(result_array.release());
        });
    constructor.set_property("entries", Value(entries_fn.release()));

    // Object.create(prototype) - creates new object with specified prototype
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.create requires at least 1 argument"));
                return Value();
            }

            // For Object.create(null), use no prototype
            if (args[0].is_null()) {
                auto new_obj = ObjectFactory::create_object();  // Use default constructor
                if (!new_obj) {
                    ctx.throw_exception(Value("Error: Failed to create object"));
                    return Value();
                }
                return Value(new_obj.release());
            }

            // For Object.create(obj), use obj as prototype
            if (args[0].is_object()) {
                Object* prototype = args[0].as_object();
                auto new_obj = ObjectFactory::create_object(prototype);
                if (!new_obj) {
                    ctx.throw_exception(Value("Error: Failed to create object with prototype"));
                    return Value();
                }
                // Also set __proto__ property for JavaScript access
                new_obj->set_property("__proto__", args[0]);
                return Value(new_obj.release());
            }

            // Invalid prototype argument
            ctx.throw_exception(Value("TypeError: Object prototype may only be an Object or null"));
            return Value();
        });
    constructor.set_property("create", Value(create_fn.release()));

    // Object.assign
    auto assign_fn = ObjectFactory::create_native_function("assign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: Object.assign requires at least one argument"));
                return Value();
            }

            Value target = args[0];
            if (!target.is_object()) {
                // Convert to object if not already
                if (target.is_null() || target.is_undefined()) {
                    ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                    return Value();
                }
                // Create wrapper object for primitives
                auto obj = ObjectFactory::create_object();
                obj->set_property("valueOf", Value(target));
                target = Value(obj.release());
            }

            Object* target_obj = target.as_object();

            // Copy properties from each source object
            for (size_t i = 1; i < args.size(); i++) {
                Value source = args[i];
                if (source.is_null() || source.is_undefined()) {
                    continue; // Skip null/undefined sources
                }

                if (source.is_object()) {
                    Object* source_obj = source.as_object();
                    // Copy all enumerable own properties
                    std::vector<std::string> property_keys = source_obj->get_own_property_keys();

                    for (const std::string& prop : property_keys) {
                        // Only copy enumerable properties
                        PropertyDescriptor desc = source_obj->get_property_descriptor(prop);
                        if (desc.is_enumerable()) {
                            Value value = source_obj->get_property(prop);
                            target_obj->set_property(prop, value);
                        }
                    }
                }
            }

            return target;
        });
    constructor.set_property("assign", Value(assign_fn.release()));
}

void ObjectBuiltin::add_object_prototype_methods(Object& prototype) {
    // NOTE: This is a placeholder for now - the actual implementation will be moved
    // from Context.cpp in the next step. For now, we keep the old implementation.

    // TODO: Move these from Context.cpp:
    // - Object.prototype.hasOwnProperty()
    // - Object.prototype.toString()
    // - Object.prototype.valueOf()
    // - Other prototype methods
}

} // namespace Quanta