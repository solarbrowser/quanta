/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/ErrorBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_error_builtins(Context& ctx) {
    auto error_prototype = ObjectFactory::create_object();

    PropertyDescriptor error_proto_name_desc(Value(std::string("Error")),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("name", error_proto_name_desc);
    error_prototype->set_property("message", Value(std::string("")));

    // Add Error.prototype.toString method
    auto error_proto_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(std::string("Error"));
            }

            Value name_val = this_obj->get_property("name");
            Value message_val = this_obj->get_property("message");

            std::string name = name_val.is_undefined() ? "Error" : name_val.to_string();
            std::string message = message_val.is_undefined() ? "" : message_val.to_string();

            if (message.empty()) {
                return Value(name);
            }
            if (name.empty()) {
                return Value(message);
            }
            return Value(name + ": " + message);
        }, 0);

    PropertyDescriptor error_proto_toString_desc(Value(error_proto_toString.release()),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("toString", error_proto_toString_desc);

    Object* error_prototype_ptr = error_prototype.get();

    auto error_constructor = ObjectFactory::create_native_constructor("Error",
        [error_prototype_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty()) {
                if (args[0].is_undefined()) {
                    message = "";
                } else if (args[0].is_object()) {
                    Object* obj = args[0].as_object();
                    if (obj->has_property("toString")) {
                        Value toString_val = obj->get_property("toString");
                        if (toString_val.is_function()) {
                            Function* toString_fn = toString_val.as_function();
                            Value result = toString_fn->call(ctx, {}, Value(obj));
                            message = result.to_string();
                        } else {
                            message = args[0].to_string();
                        }
                    } else {
                        message = args[0].to_string();
                    }
                } else {
                    message = args[0].to_string();
                }
            }
            auto error_obj = std::make_unique<Error>(Error::Type::Error, message);
            error_obj->set_property("_isError", Value(true));

            // Support subclassing: when called via super() from a derived class,
            // ctx.get_this_binding() is the subclass instance with its prototype.
            // Use that prototype so c instanceof C works alongside c instanceof Error.
            Object* proto_to_use = error_prototype_ptr;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                Object* this_proto = this_obj->get_prototype();
                if (this_proto && this_proto != error_prototype_ptr) {
                    proto_to_use = this_proto;
                }
            }
            error_obj->set_prototype(proto_to_use);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* this_obj = ctx.get_this_binding();
                    if (!this_obj) {
                        return Value(std::string("Error"));
                    }

                    Value name_val = this_obj->get_property("name");
                    Value message_val = this_obj->get_property("message");

                    std::string name = name_val.is_string() ? name_val.to_string() : "Error";
                    std::string message = message_val.is_string() ? message_val.to_string() : "";

                    if (message.empty()) {
                        return Value(name);
                    }
                    if (name.empty()) {
                        return Value(message);
                    }
                    return Value(name + ": " + message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    error_constructor->set_property("isError", Value(error_isError.release()));

    PropertyDescriptor error_constructor_desc(Value(error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("constructor", error_constructor_desc);

    error_constructor->set_property("prototype", Value(error_prototype_ptr), PropertyAttributes::None);

    Function* error_ctor = error_constructor.get();
    (void)error_ctor;

    ctx.register_built_in_object("Error", error_constructor.release());

    error_prototype.release();

    auto type_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    type_error_prototype->set_property("name", Value(std::string("TypeError")));
    Object* type_error_proto_ptr = type_error_prototype.get();

    auto type_error_constructor = ObjectFactory::create_native_constructor("TypeError",
        [type_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::TypeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(type_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor type_error_constructor_desc(Value(type_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    type_error_prototype->set_property_descriptor("constructor", type_error_constructor_desc);

    type_error_constructor->set_property("prototype", Value(type_error_prototype.release()));

    PropertyDescriptor type_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    type_error_length_desc.set_configurable(true);
    type_error_length_desc.set_enumerable(false);
    type_error_length_desc.set_writable(false);
    type_error_constructor->set_property_descriptor("length", type_error_length_desc);

    type_error_constructor->set_property("name", Value(std::string("TypeError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        type_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("TypeError", type_error_constructor.release());

    auto reference_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    reference_error_prototype->set_property("name", Value(std::string("ReferenceError")));
    Object* reference_error_proto_ptr = reference_error_prototype.get();

    auto reference_error_constructor = ObjectFactory::create_native_constructor("ReferenceError",
        [reference_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::ReferenceError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(reference_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor reference_error_constructor_desc(Value(reference_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    reference_error_prototype->set_property_descriptor("constructor", reference_error_constructor_desc);
    reference_error_constructor->set_property("prototype", Value(reference_error_prototype.release()));

    PropertyDescriptor reference_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    reference_error_length_desc.set_configurable(true);
    reference_error_length_desc.set_enumerable(false);
    reference_error_length_desc.set_writable(false);
    reference_error_constructor->set_property_descriptor("length", reference_error_length_desc);

    reference_error_constructor->set_property("name", Value(std::string("ReferenceError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        reference_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("ReferenceError", reference_error_constructor.release());

    auto syntax_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    syntax_error_prototype->set_property("name", Value(std::string("SyntaxError")));
    Object* syntax_error_proto_ptr = syntax_error_prototype.get();

    auto syntax_error_constructor = ObjectFactory::create_native_constructor("SyntaxError",
        [syntax_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::SyntaxError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(syntax_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor syntax_error_constructor_desc(Value(syntax_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    syntax_error_prototype->set_property_descriptor("constructor", syntax_error_constructor_desc);
    syntax_error_constructor->set_property("prototype", Value(syntax_error_prototype.release()));

    PropertyDescriptor syntax_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    syntax_error_length_desc.set_configurable(true);
    syntax_error_length_desc.set_enumerable(false);
    syntax_error_length_desc.set_writable(false);
    syntax_error_constructor->set_property_descriptor("length", syntax_error_length_desc);

    syntax_error_constructor->set_property("name", Value(std::string("SyntaxError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        syntax_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("SyntaxError", syntax_error_constructor.release());

    auto range_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    range_error_prototype->set_property("name", Value(std::string("RangeError")));
    Object* range_error_proto_ptr = range_error_prototype.get();

    auto range_error_constructor = ObjectFactory::create_native_constructor("RangeError",
        [range_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::RangeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(range_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor range_error_constructor_desc(Value(range_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    range_error_prototype->set_property_descriptor("constructor", range_error_constructor_desc);

    range_error_constructor->set_property("prototype", Value(range_error_prototype.release()));

    PropertyDescriptor range_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    range_error_length_desc.set_configurable(true);
    range_error_length_desc.set_enumerable(false);
    range_error_length_desc.set_writable(false);
    range_error_constructor->set_property_descriptor("length", range_error_length_desc);

    range_error_constructor->set_property("name", Value(std::string("RangeError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        range_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("RangeError", range_error_constructor.release());

    auto uri_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    uri_error_prototype->set_property("name", Value(std::string("URIError")));
    Object* uri_error_proto_ptr = uri_error_prototype.get();

    auto uri_error_constructor = ObjectFactory::create_native_constructor("URIError",
        [uri_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::URIError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(uri_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor uri_error_constructor_desc(Value(uri_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    uri_error_prototype->set_property_descriptor("constructor", uri_error_constructor_desc);

    uri_error_constructor->set_property("prototype", Value(uri_error_prototype.release()));

    if (error_ctor) {
        uri_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("URIError", uri_error_constructor.release());

    auto eval_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    eval_error_prototype->set_property("name", Value(std::string("EvalError")));
    Object* eval_error_proto_ptr = eval_error_prototype.get();

    auto eval_error_constructor = ObjectFactory::create_native_constructor("EvalError",
        [eval_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::EvalError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(eval_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor eval_error_constructor_desc(Value(eval_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    eval_error_prototype->set_property_descriptor("constructor", eval_error_constructor_desc);

    eval_error_constructor->set_property("prototype", Value(eval_error_prototype.release()));

    if (error_ctor) {
        eval_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("EvalError", eval_error_constructor.release());

    auto aggregate_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    aggregate_error_prototype->set_property("name", Value(std::string("AggregateError")));

    Object* agg_error_proto_ptr = aggregate_error_prototype.get();

    auto aggregate_error_constructor = ObjectFactory::create_native_constructor("AggregateError",
        [agg_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (args.size() > 1 && !args[1].is_undefined()) {
                Value msg_value = args[1];
                if (msg_value.is_object()) {
                    Object* obj = msg_value.as_object();
                    Value toString_method = obj->get_property("toString");
                    if (toString_method.is_function()) {
                        try {
                            Function* func = toString_method.as_function();
                            Value result = func->call(ctx, {}, msg_value);
                            if (!ctx.has_exception()) {
                                message = result.to_string();
                            } else {
                                ctx.clear_exception();
                                message = msg_value.to_string();
                            }
                        } catch (...) {
                            message = msg_value.to_string();
                        }
                    } else {
                        message = msg_value.to_string();
                    }
                } else {
                    message = msg_value.to_string();
                }
            }
            auto error_obj = std::make_unique<Error>(Error::Type::AggregateError, message);
            error_obj->set_property("_isError", Value(true));

            error_obj->set_prototype(agg_error_proto_ptr);

            if (args.size() > 0 && args[0].is_object()) {
                error_obj->set_property("errors", args[0]);
            } else {
                auto empty_array = ObjectFactory::create_array();
                error_obj->set_property("errors", Value(empty_array.release()));
            }

            if (args.size() > 2 && args[2].is_object()) {
                Object* options = args[2].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        }, 2);

    PropertyDescriptor constructor_desc(Value(aggregate_error_constructor.get()), PropertyAttributes::None);
    constructor_desc.set_writable(true);
    constructor_desc.set_enumerable(false);
    constructor_desc.set_configurable(true);
    aggregate_error_prototype->set_property_descriptor("constructor", constructor_desc);

    PropertyDescriptor name_desc(Value(std::string("AggregateError")), PropertyAttributes::None);
    name_desc.set_configurable(true);
    name_desc.set_enumerable(false);
    name_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(2.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    length_desc.set_configurable(true);
    length_desc.set_enumerable(false);
    length_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("length", length_desc);

    aggregate_error_constructor->set_property("prototype", Value(aggregate_error_prototype.release()));

    if (error_ctor) {
        aggregate_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("AggregateError", aggregate_error_constructor.release());
}

} // namespace Quanta
