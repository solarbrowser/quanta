/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "string_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>

namespace Quanta {

void StringBuiltin::register_string_builtin(Context& ctx) {
    // String constructor
    auto string_constructor = ObjectFactory::create_native_function("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str_value = args.empty() ? "" : args[0].to_string();

            // Check if called as constructor (with 'new')
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                // Called as constructor - set up the String object
                this_obj->set_property("value", Value(str_value));
                this_obj->set_property("length", Value(static_cast<double>(str_value.length())));

                // Add toString method to the object
                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args; // Suppress unused warning
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding && this_binding->has_property("value")) {
                            return this_binding->get_property("value");
                        }
                        return Value("");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
            }

            // Always return the string value (construct() will use the object if called as constructor)
            return Value(str_value);
        });

    // Add String static methods
    add_string_static_methods(*string_constructor);

    // Create String.prototype with instance methods
    auto string_prototype = ObjectFactory::create_object();
    add_string_prototype_methods(*string_prototype);

    // Set String.prototype
    string_constructor->set_property("prototype", Value(string_prototype.release()));

    // Register the String constructor globally
    ctx.register_built_in_object("String", string_constructor.release());
}

void StringBuiltin::add_string_static_methods(Function& constructor) {
    // Add String.concat static method
    auto string_concat_static = ObjectFactory::create_native_function("concat",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            std::string result = "";
            for (const Value& arg : args) {
                result += arg.to_string();
            }
            return Value(result);
        });
    constructor.set_property("concat", Value(string_concat_static.release()));
}

void StringBuiltin::add_string_prototype_methods(Object& prototype) {
    // String.prototype.padStart
    auto padStart_fn = ObjectFactory::create_native_function("padStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the string)
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(str);

            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";

            if (target_length <= str.length()) {
                return Value(str);
            }

            uint32_t pad_length = target_length - str.length();
            std::string padding = "";

            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }

            return Value(padding + str);
        });
    prototype.set_property("padStart", Value(padStart_fn.release()));

    // String.prototype.padEnd
    auto padEnd_fn = ObjectFactory::create_native_function("padEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the string)
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(str);

            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";

            if (target_length <= str.length()) {
                return Value(str);
            }

            uint32_t pad_length = target_length - str.length();
            std::string padding = "";

            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }

            return Value(str + padding);
        });
    prototype.set_property("padEnd", Value(padEnd_fn.release()));

    // TODO: Add more String.prototype methods:
    // - charAt, charCodeAt, substring, substr, slice
    // - indexOf, lastIndexOf, search
    // - match, replace, split
    // - toLowerCase, toUpperCase
    // - trim, trimStart, trimEnd
    // - concat, repeat
}

} // namespace Quanta