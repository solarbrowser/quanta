/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "array_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>

namespace Quanta {

void ArrayBuiltin::register_array_builtin(Context& ctx) {
    // Array constructor - create as a proper native function
    auto array_constructor = ObjectFactory::create_native_function("Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                // new Array() - empty array
                return Value(ObjectFactory::create_array().release());
            } else if (args.size() == 1 && args[0].is_number()) {
                // new Array(length) - array with specified length
                uint32_t length = static_cast<uint32_t>(args[0].to_number());
                auto array = ObjectFactory::create_array();
                array->set_property("length", Value(static_cast<double>(length)));
                return Value(array.release());
            } else {
                // new Array(item1, item2, ...) - array with initial elements
                auto array = ObjectFactory::create_array();
                for (size_t i = 0; i < args.size(); i++) {
                    array->set_element(static_cast<uint32_t>(i), args[i]);
                }
                array->set_property("length", Value(static_cast<double>(args.size())));
                return Value(array.release());
            }
        });

    // Add Array static methods (Array.from, Array.of, Array.isArray)
    add_array_static_methods(*array_constructor);

    // Create Array.prototype with instance methods
    auto array_prototype = ObjectFactory::create_object();
    add_array_prototype_methods(*array_prototype);

    // Set Array.prototype
    array_constructor->set_property("prototype", Value(array_prototype.release()));

    // Register the Array constructor globally
    ctx.register_built_in_object("Array", array_constructor.release());
}

void ArrayBuiltin::add_array_static_methods(Function& constructor) {
    // Array.isArray(obj) - check if value is an array
    auto isArray_fn = ObjectFactory::create_native_function("isArray",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].is_object() && args[0].as_object()->is_array());
        });
    constructor.set_property("isArray", Value(isArray_fn.release()));

    // Array.from(arrayLike) - create array from array-like object
    auto from_fn = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(ObjectFactory::create_array().release());

            Value arrayLike = args[0];

            // Handle strings specially
            if (arrayLike.is_string()) {
                std::string str = arrayLike.to_string();
                auto array = ObjectFactory::create_array();

                for (size_t i = 0; i < str.length(); i++) {
                    array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str[i])));
                }

                array->set_property("length", Value(static_cast<double>(str.length())));
                return Value(array.release());
            }

            // Handle array-like objects
            if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                if (obj->has_property("length")) {
                    uint32_t length = static_cast<uint32_t>(obj->get_property("length").to_number());
                    auto array = ObjectFactory::create_array();

                    for (uint32_t i = 0; i < length; i++) {
                        if (obj->has_property(std::to_string(i))) {
                            Value item = obj->get_property(std::to_string(i));
                            array->set_element(i, item);
                        }
                    }

                    array->set_property("length", Value(static_cast<double>(length)));
                    return Value(array.release());
                }
            }

            return Value(ObjectFactory::create_array().release());
        });
    constructor.set_property("from", Value(from_fn.release()));

    // Array.of(...items) - create array from arguments
    auto of_fn = ObjectFactory::create_native_function("of",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            auto array = ObjectFactory::create_array();
            for (size_t i = 0; i < args.size(); i++) {
                array->set_element(static_cast<uint32_t>(i), args[i]);
            }
            array->set_property("length", Value(static_cast<double>(args.size())));
            return Value(array.release());
        });
    constructor.set_property("of", Value(of_fn.release()));
}

void ArrayBuiltin::add_array_prototype_methods(Object& prototype) {
    // Array.prototype.find(callback) - find first matching element
    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Implementation placeholder for Array.prototype.find
            (void)ctx;
            (void)args;
            return Value();
        });
    prototype.set_property("find", Value(find_fn.release()));

    // Array.prototype.includes(searchElement) - check if array includes element
    auto includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            // Implementation placeholder for Array.prototype.includes
            (void)args;
            return Value(false);
        });
    prototype.set_property("includes", Value(includes_fn.release()));

    // Array.prototype.flat(depth) - flatten array
    auto flat_fn = ObjectFactory::create_native_function("flat",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            // Implementation placeholder for Array.prototype.flat
            (void)args;
            auto result = ObjectFactory::create_array();
            return Value(result.release());
        });
    prototype.set_property("flat", Value(flat_fn.release()));

    // Array.prototype.concat(...arrays) - concatenate arrays
    auto concat_fn = ObjectFactory::create_native_function("concat",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            // Implementation placeholder for Array.prototype.concat
            (void)args;
            auto result = ObjectFactory::create_array(0);
            return Value(result.release());
        });
    prototype.set_property("concat", Value(concat_fn.release()));

    // TODO: Add more Array.prototype methods:
    // - push, pop, shift, unshift
    // - slice, splice
    // - forEach, map, filter, reduce
    // - sort, reverse
    // - join, toString
    // - indexOf, lastIndexOf
    // - keys, values, entries
}

} // namespace Quanta