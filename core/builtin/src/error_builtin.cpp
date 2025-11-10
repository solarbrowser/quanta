/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "error_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../include/Error.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>

namespace Quanta {

void ErrorBuiltin::register_error_builtins(Context& ctx) {
    register_error_constructor(ctx);
    register_type_error_constructor(ctx);
    register_reference_error_constructor(ctx);
    register_syntax_error_constructor(ctx);
    register_range_error_constructor(ctx);
}

void ErrorBuiltin::register_error_constructor(Context& ctx) {
    // Error constructor
    auto error_constructor = ObjectFactory::create_native_function("Error",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].to_string();

            // Check if called as constructor
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                // Set up Error properties
                this_obj->set_property("name", Value("Error"));
                this_obj->set_property("message", Value(message));

                // Add toString method
                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding) {
                            std::string name = this_binding->has_property("name") ?
                                this_binding->get_property("name").to_string() : "Error";
                            std::string message = this_binding->has_property("message") ?
                                this_binding->get_property("message").to_string() : "";
                            return Value(name + (message.empty() ? "" : ": " + message));
                        }
                        return Value("Error");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));

                return Value(); // Constructor returns undefined
            }

            // Called as function - return new Error object
            auto error_obj = ObjectFactory::create_object();
            error_obj->set_property("name", Value("Error"));
            error_obj->set_property("message", Value(message));
            return Value(error_obj.release());
        });

    // Add Error.isError static method
    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    error_constructor->set_property("isError", Value(error_isError.release()));

    // Create Error.prototype
    auto error_prototype = ObjectFactory::create_object();
    add_error_prototype_methods(*error_prototype);
    error_constructor->set_property("prototype", Value(error_prototype.release()));

    // Register Error globally
    ctx.register_built_in_object("Error", error_constructor.release());
}

void ErrorBuiltin::register_type_error_constructor(Context& ctx) {
    // TypeError constructor
    auto type_error_constructor = ObjectFactory::create_native_function("TypeError",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("name", Value("TypeError"));
                this_obj->set_property("message", Value(message));

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding) {
                            std::string name = this_binding->get_property("name").to_string();
                            std::string message = this_binding->get_property("message").to_string();
                            return Value(name + ": " + message);
                        }
                        return Value("TypeError");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
                return Value();
            }

            auto error_obj = ObjectFactory::create_object();
            error_obj->set_property("name", Value("TypeError"));
            error_obj->set_property("message", Value(message));
            return Value(error_obj.release());
        });

    ctx.register_built_in_object("TypeError", type_error_constructor.release());
}

void ErrorBuiltin::register_reference_error_constructor(Context& ctx) {
    // ReferenceError constructor
    auto reference_error_constructor = ObjectFactory::create_native_function("ReferenceError",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("name", Value("ReferenceError"));
                this_obj->set_property("message", Value(message));

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding) {
                            std::string name = this_binding->get_property("name").to_string();
                            std::string message = this_binding->get_property("message").to_string();
                            return Value(name + ": " + message);
                        }
                        return Value("ReferenceError");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
                return Value();
            }

            auto error_obj = ObjectFactory::create_object();
            error_obj->set_property("name", Value("ReferenceError"));
            error_obj->set_property("message", Value(message));
            return Value(error_obj.release());
        });

    ctx.register_built_in_object("ReferenceError", reference_error_constructor.release());
}

void ErrorBuiltin::register_syntax_error_constructor(Context& ctx) {
    // SyntaxError constructor
    auto syntax_error_constructor = ObjectFactory::create_native_function("SyntaxError",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("name", Value("SyntaxError"));
                this_obj->set_property("message", Value(message));

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding) {
                            std::string name = this_binding->get_property("name").to_string();
                            std::string message = this_binding->get_property("message").to_string();
                            return Value(name + ": " + message);
                        }
                        return Value("SyntaxError");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
                return Value();
            }

            auto error_obj = ObjectFactory::create_object();
            error_obj->set_property("name", Value("SyntaxError"));
            error_obj->set_property("message", Value(message));
            return Value(error_obj.release());
        });

    ctx.register_built_in_object("SyntaxError", syntax_error_constructor.release());
}

void ErrorBuiltin::register_range_error_constructor(Context& ctx) {
    // RangeError constructor
    auto range_error_constructor = ObjectFactory::create_native_function("RangeError",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("name", Value("RangeError"));
                this_obj->set_property("message", Value(message));

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding) {
                            std::string name = this_binding->get_property("name").to_string();
                            std::string message = this_binding->get_property("message").to_string();
                            return Value(name + ": " + message);
                        }
                        return Value("RangeError");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
                return Value();
            }

            auto error_obj = ObjectFactory::create_object();
            error_obj->set_property("name", Value("RangeError"));
            error_obj->set_property("message", Value(message));
            return Value(error_obj.release());
        });

    ctx.register_built_in_object("RangeError", range_error_constructor.release());
}

void ErrorBuiltin::add_error_prototype_methods(Object& prototype) {
    // Error.prototype.toString
    auto toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_binding = ctx.get_this_binding();
            if (this_binding) {
                std::string name = this_binding->has_property("name") ?
                    this_binding->get_property("name").to_string() : "Error";
                std::string message = this_binding->has_property("message") ?
                    this_binding->get_property("message").to_string() : "";
                return Value(name + (message.empty() ? "" : ": " + message));
            }
            return Value("Error");
        });
    prototype.set_property("toString", Value(toString_fn.release()));
}

} // namespace Quanta