/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "function_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>

namespace Quanta {

void FunctionBuiltin::register_function_builtin(Context& ctx) {
    // Function constructor
    auto function_constructor = ObjectFactory::create_native_function("Function",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            (void)args; // Suppress unused warning
            // For now, return a simple function that does nothing
            return Value(ObjectFactory::create_function().release());
        });

    // Create Function.prototype with methods
    auto function_prototype = ObjectFactory::create_object();
    add_function_prototype_methods(*function_prototype);

    // Set Function.prototype
    function_constructor->set_property("prototype", Value(function_prototype.release()));

    // Register Function globally
    ctx.register_built_in_object("Function", function_constructor.release());
}

void FunctionBuiltin::add_function_prototype_methods(Object& prototype) {
    // Function.prototype.call
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that .call was called on
            Object* this_binding = ctx.get_this_binding();
            if (!this_binding || !this_binding->is_function()) {
                ctx.throw_exception(Value("TypeError: Function.prototype.call called on non-function"));
                return Value();
            }

            Function* func = static_cast<Function*>(this_binding);

            // First argument is 'this' value, rest are function arguments
            Value this_value = args.empty() ? Value() : args[0];
            std::vector<Value> func_args;
            if (args.size() > 1) {
                func_args.assign(args.begin() + 1, args.end());
            }

            // Call the function with the specified 'this' value
            return func->call(ctx, func_args, this_value);
        });
    prototype.set_property("call", Value(call_fn.release()));

    // Function.prototype.apply
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that .apply was called on
            Object* this_binding = ctx.get_this_binding();
            if (!this_binding || !this_binding->is_function()) {
                ctx.throw_exception(Value("TypeError: Function.prototype.apply called on non-function"));
                return Value();
            }

            Function* func = static_cast<Function*>(this_binding);

            // First argument is 'this' value
            Value this_value = args.empty() ? Value() : args[0];

            // Second argument should be array-like object containing arguments
            std::vector<Value> func_args;
            if (args.size() > 1 && args[1].is_object()) {
                Object* args_array = args[1].as_object();
                if (args_array->is_array()) {
                    uint32_t length = args_array->get_length();
                    for (uint32_t i = 0; i < length; i++) {
                        func_args.push_back(args_array->get_element(i));
                    }
                }
            }

            // Call the function with the specified 'this' value and arguments
            return func->call(ctx, func_args, this_value);
        });
    prototype.set_property("apply", Value(apply_fn.release()));

    // TODO: Add Function.prototype.bind
    // This is more complex as it needs to create a new bound function
}

} // namespace Quanta