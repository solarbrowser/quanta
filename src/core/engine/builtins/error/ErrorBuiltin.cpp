/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/ErrorBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"

namespace Quanta {

// OrdinaryCreateFromConstructor's prototype source: new.target.prototype, else a subclass `this` already wired up by super(), else the intrinsic default.
static Object* resolve_error_prototype(Context& ctx, Object* default_proto) {
    Value new_target = ctx.get_new_target();
    if (new_target.is_function()) {
        Value nt_proto = new_target.as_function()->get_property("prototype");
        if (nt_proto.is_object()) return nt_proto.as_object();
    }
    Object* this_obj = ctx.get_this_binding();
    if (this_obj) {
        Object* this_proto = this_obj->get_prototype();
        if (this_proto && this_proto != default_proto) return this_proto;
    }
    return default_proto;
}

// ToString(argument): unlike to_property_key(), throws when the ToPrimitive result is a Symbol.
static bool error_arg_to_string(Context& ctx, const Value& v, std::string& out) {
    if (v.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return false; }
    if (!v.is_object() && !v.is_function()) { out = v.to_string(); return true; }
    Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
    Value prim;
    Value toPrim_fn = obj->get_property("Symbol.toPrimitive");
    if (ctx.has_exception()) return false;
    if (toPrim_fn.is_function()) {
        prim = toPrim_fn.as_function()->call(ctx, {Value(std::string("string"))}, v);
        if (ctx.has_exception()) return false;
        if (prim.is_object() || prim.is_function()) { ctx.throw_type_error("Cannot convert object to primitive value"); return false; }
    } else {
        Value toString_fn = obj->get_property("toString");
        if (ctx.has_exception()) return false;
        bool got = false;
        if (toString_fn.is_function()) {
            prim = toString_fn.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return false;
            got = !prim.is_object() && !prim.is_function();
        }
        if (!got) {
            Value valueOf_fn = obj->get_property("valueOf");
            if (ctx.has_exception()) return false;
            if (valueOf_fn.is_function()) {
                prim = valueOf_fn.as_function()->call(ctx, {}, v);
                if (ctx.has_exception()) return false;
                got = !prim.is_object() && !prim.is_function();
            }
        }
        if (!got) { ctx.throw_type_error("Cannot convert object to primitive value"); return false; }
    }
    if (prim.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return false; }
    out = prim.to_string();
    return true;
}

// IterableToList(errors): GetIterator then IteratorStep/IteratorValue, propagating any abrupt step.
static bool error_iterable_to_list(Context& ctx, const Value& iterable, std::vector<Value>& out) {
    Object* obj = iterable.is_function() ? static_cast<Object*>(iterable.as_function())
                                          : iterable.is_object() ? iterable.as_object() : nullptr;
    if (!obj) { ctx.throw_type_error("errors is not iterable"); return false; }
    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (!iter_sym) { ctx.throw_type_error("Symbol.iterator unavailable"); return false; }
    Value iter_method = obj->get_property(iter_sym->to_property_key());
    if (ctx.has_exception()) return false;
    if (!iter_method.is_function()) { ctx.throw_type_error("errors is not iterable"); return false; }
    Value iter_val = iter_method.as_function()->call(ctx, {}, iterable);
    if (ctx.has_exception()) return false;
    if (!iter_val.is_object() && !iter_val.is_function()) { ctx.throw_type_error("Result of the Symbol.iterator method is not an object"); return false; }
    Object* iterator = iter_val.is_function() ? static_cast<Object*>(iter_val.as_function()) : iter_val.as_object();
    Value next_fn = iterator->get_property("next");
    if (ctx.has_exception()) return false;
    if (!next_fn.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return false; }
    while (true) {
        Value res = next_fn.as_function()->call(ctx, {}, Value(iterator));
        if (ctx.has_exception()) return false;
        if (!res.is_object() && !res.is_function()) { ctx.throw_type_error("Iterator result is not an object"); return false; }
        Object* res_obj = res.is_function() ? static_cast<Object*>(res.as_function()) : res.as_object();
        Value done_v = res_obj->get_property("done");
        if (ctx.has_exception()) return false;
        if (done_v.to_boolean()) return true;
        Value val = res_obj->get_property("value");
        if (ctx.has_exception()) return false;
        out.push_back(val);
    }
}

void register_error_builtins(Context& ctx) {
    auto error_prototype = ObjectFactory::create_object();

    PropertyDescriptor error_proto_name_desc(Value(std::string("Error")),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("name", error_proto_name_desc);
    error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);

    // Add Error.prototype.toString method
    auto error_proto_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || ctx.original_this_was_nullish() || ctx.original_this_was_primitive()) {
                ctx.throw_type_error("Error.prototype.toString called on non-object");
                return Value();
            }

            Value name_val = this_obj->get_property("name");
            if (name_val.is_symbol()) { ctx.throw_type_error("Error name cannot be a Symbol"); return Value(); }
            Value message_val = this_obj->get_property("message");
            if (message_val.is_symbol()) { ctx.throw_type_error("Error message cannot be a Symbol"); return Value(); }

            std::string name = name_val.is_undefined() ? "Error" : name_val.to_string();
            if (ctx.has_exception()) return Value();
            std::string message;
            if (message_val.is_undefined()) {
                message = "";
            } else if (message_val.is_object() && message_val.as_object()) {
                Object* mobj = message_val.as_object();
                Value mts = mobj->get_property("toString");
                if (ctx.has_exception()) return Value();
                if (mts.is_function()) {
                    Value r = mts.as_function()->call(ctx, {}, message_val);
                    if (ctx.has_exception()) return Value();
                    message = r.to_string();
                } else {
                    Value mvs = mobj->get_property("valueOf");
                    if (ctx.has_exception()) return Value();
                    if (mvs.is_function()) {
                        Value r = mvs.as_function()->call(ctx, {}, message_val);
                        if (ctx.has_exception()) return Value();
                        message = r.to_string();
                    } else {
                        ctx.throw_type_error("Cannot convert message to primitive value");
                        return Value();
                    }
                }
            } else {
                message = message_val.to_string();
                if (ctx.has_exception()) return Value();
            }

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
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::Error, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);

            error_obj->set_prototype(resolve_error_prototype(ctx, error_prototype_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    {
        PropertyDescriptor isError_desc(Value(error_isError.release()), PropertyAttributes::BuiltinFunction);
        error_constructor->set_property_descriptor("isError", isError_desc);
    }

    {
        auto stack_get = ObjectFactory::create_native_function("get stack",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Object* self = ctx.get_this_binding();
                if (!self || ctx.original_this_was_nullish() || ctx.original_this_was_primitive()) {
                    ctx.throw_type_error("Error.prototype.stack getter: this is not an object");
                    return Value();
                }
                if (self->get_type() != Object::ObjectType::Error) return Value();
                if (self->has_own_property("stack")) {
                    PropertyDescriptor d = self->get_property_descriptor("stack");
                    if (d.is_data_descriptor()) return d.get_value();
                }
                return Value(static_cast<Error*>(self)->get_stack_trace());
            }, 0);
        auto stack_set = ObjectFactory::create_native_function("set stack",
            [error_prototype_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* self = ctx.get_this_binding();
                if (!self || ctx.original_this_was_nullish() || ctx.original_this_was_primitive()) {
                    ctx.throw_type_error("Error.prototype.stack setter: this is not an object");
                    return Value();
                }
                if (self == error_prototype_ptr) {
                    ctx.throw_type_error("Cannot set stack on Error.prototype");
                    return Value();
                }
                Value v = args.empty() ? Value() : args[0];
                if (!v.is_string()) {
                    ctx.throw_type_error("Error.prototype.stack setter: value must be a string");
                    return Value();
                }
                // SetterThatIgnoresPrototypeProperties semantics:
                // 1. [[GetOwnProperty]] to check for own "stack".
                PropertyDescriptor existing = self->get_property_descriptor("stack");
                if (ctx.has_exception()) return Value();
                bool has_own = existing.has_value() || existing.is_accessor_descriptor();
                if (has_own) {
                    if (existing.is_data_descriptor() && !existing.is_writable()) {
                        ctx.throw_type_error("Error.prototype.stack setter: stack is non-writable");
                        return Value();
                    }
                    // 2. [[Set]] with Throw=true -- triggers set trap on Proxy.
                    bool set_ok = self->set_property("stack", v);
                    if (ctx.has_exception()) return Value();
                    if (!set_ok) {
                        ctx.throw_type_error("Error.prototype.stack setter: [[Set]] failed");
                    }
                    return Value();
                }
                // 3. No own property: [[DefineOwnProperty]] -- triggers defineProperty trap.
                PropertyDescriptor d(v, static_cast<PropertyAttributes>(
                    PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
                bool ok = self->set_property_descriptor("stack", d);
                if (ctx.has_exception()) return Value();
                if (!ok) {
                    ctx.throw_type_error("Error.prototype.stack setter: [[DefineOwnProperty]] failed");
                }
                return Value();
            }, 1);
        PropertyDescriptor stack_desc;
        stack_desc.set_getter(stack_get.release());
        stack_desc.set_setter(stack_set.release());
        stack_desc.set_enumerable(false);
        stack_desc.set_configurable(true);
        error_prototype->set_property_descriptor("stack", stack_desc);
    }

    PropertyDescriptor error_constructor_desc(Value(error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("constructor", error_constructor_desc);

    error_constructor->set_property("prototype", Value(error_prototype_ptr), PropertyAttributes::None);

    Function* error_ctor = error_constructor.get();
    (void)error_ctor;

    ctx.register_built_in_object("Error", error_constructor.release());

    error_prototype.release();

    auto type_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    type_error_prototype->set_property("name", Value(std::string("TypeError")), PropertyAttributes::BuiltinFunction);
    type_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* type_error_proto_ptr = type_error_prototype.get();

    auto type_error_constructor = ObjectFactory::create_native_constructor("TypeError",
        [type_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::TypeError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, type_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor type_error_constructor_desc(Value(type_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    type_error_prototype->set_property_descriptor("constructor", type_error_constructor_desc);

    type_error_constructor->set_property("prototype", Value(type_error_prototype.release()), PropertyAttributes::None);

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
    reference_error_prototype->set_property("name", Value(std::string("ReferenceError")), PropertyAttributes::BuiltinFunction);
    reference_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* reference_error_proto_ptr = reference_error_prototype.get();

    auto reference_error_constructor = ObjectFactory::create_native_constructor("ReferenceError",
        [reference_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::ReferenceError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, reference_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor reference_error_constructor_desc(Value(reference_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    reference_error_prototype->set_property_descriptor("constructor", reference_error_constructor_desc);
    reference_error_constructor->set_property("prototype", Value(reference_error_prototype.release()), PropertyAttributes::None);

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
    syntax_error_prototype->set_property("name", Value(std::string("SyntaxError")), PropertyAttributes::BuiltinFunction);
    syntax_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* syntax_error_proto_ptr = syntax_error_prototype.get();

    auto syntax_error_constructor = ObjectFactory::create_native_constructor("SyntaxError",
        [syntax_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::SyntaxError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, syntax_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor syntax_error_constructor_desc(Value(syntax_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    syntax_error_prototype->set_property_descriptor("constructor", syntax_error_constructor_desc);
    syntax_error_constructor->set_property("prototype", Value(syntax_error_prototype.release()), PropertyAttributes::None);

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
    range_error_prototype->set_property("name", Value(std::string("RangeError")), PropertyAttributes::BuiltinFunction);
    range_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* range_error_proto_ptr = range_error_prototype.get();

    auto range_error_constructor = ObjectFactory::create_native_constructor("RangeError",
        [range_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::RangeError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, range_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor range_error_constructor_desc(Value(range_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    range_error_prototype->set_property_descriptor("constructor", range_error_constructor_desc);

    range_error_constructor->set_property("prototype", Value(range_error_prototype.release()), PropertyAttributes::None);

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
    uri_error_prototype->set_property("name", Value(std::string("URIError")), PropertyAttributes::BuiltinFunction);
    uri_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* uri_error_proto_ptr = uri_error_prototype.get();

    auto uri_error_constructor = ObjectFactory::create_native_constructor("URIError",
        [uri_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::URIError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, uri_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor uri_error_constructor_desc(Value(uri_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    uri_error_prototype->set_property_descriptor("constructor", uri_error_constructor_desc);

    uri_error_constructor->set_property("prototype", Value(uri_error_prototype.release()), PropertyAttributes::None);

    if (error_ctor) {
        uri_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("URIError", uri_error_constructor.release());

    auto eval_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    eval_error_prototype->set_property("name", Value(std::string("EvalError")), PropertyAttributes::BuiltinFunction);
    eval_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* eval_error_proto_ptr = eval_error_prototype.get();

    auto eval_error_constructor = ObjectFactory::create_native_constructor("EvalError",
        [eval_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                if (!error_arg_to_string(ctx, args[0], message)) return Value();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::EvalError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, eval_error_proto_ptr));

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }



            return Value(error_obj.release());
        });

    PropertyDescriptor eval_error_constructor_desc(Value(eval_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    eval_error_prototype->set_property_descriptor("constructor", eval_error_constructor_desc);

    eval_error_constructor->set_property("prototype", Value(eval_error_prototype.release()), PropertyAttributes::None);

    if (error_ctor) {
        eval_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("EvalError", eval_error_constructor.release());

    auto aggregate_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    aggregate_error_prototype->set_property("name", Value(std::string("AggregateError")), PropertyAttributes::BuiltinFunction);
    aggregate_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);

    Object* agg_error_proto_ptr = aggregate_error_prototype.get();

    auto aggregate_error_constructor = ObjectFactory::create_native_constructor("AggregateError",
        [agg_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec order: message ToString before errors iteration (order-of-args-evaluation).
            Value message_arg = args.size() > 1 ? args[1] : Value();
            bool has_message = !message_arg.is_undefined();
            std::string message;
            if (has_message && !error_arg_to_string(ctx, message_arg, message)) return Value();

            std::vector<Value> errors_list;
            if (!error_iterable_to_list(ctx, args.empty() ? Value() : args[0], errors_list)) return Value();

            auto error_obj = std::make_unique<Error>(Error::Type::AggregateError, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_prototype(resolve_error_prototype(ctx, agg_error_proto_ptr));

            if (has_message) {
                error_obj->set_property_descriptor("message",
                    PropertyDescriptor(Value(message), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));
            }

            auto errors_array = ObjectFactory::create_array(static_cast<uint32_t>(errors_list.size()));
            for (size_t i = 0; i < errors_list.size(); i++) errors_array->set_element(static_cast<uint32_t>(i), errors_list[i]);
            error_obj->set_property_descriptor("errors",
                PropertyDescriptor(Value(errors_array.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));

            if (args.size() > 2 && args[2].is_object()) {
                Object* options = args[2].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    if (ctx.has_exception()) return Value();
                    error_obj->set_property_descriptor("cause",
                        PropertyDescriptor(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));
                }
            }

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

    aggregate_error_constructor->set_property("prototype", Value(aggregate_error_prototype.release()), PropertyAttributes::None);

    if (error_ctor) {
        aggregate_error_constructor->set_prototype(error_ctor);
    }

    ctx.register_built_in_object("AggregateError", aggregate_error_constructor.release());

    // SuppressedError: wraps two errors when both a body and disposal throw
    Object* error_prototype_ptr2 = nullptr;
    {
        Value ep = ctx.get_global_object()->get_property("Error");
        if (ep.is_function()) {
            Value proto = ep.as_function()->get_property("prototype");
            if (proto.is_object()) error_prototype_ptr2 = proto.as_object();
        }
    }

    auto suppressed_error_prototype = ObjectFactory::create_object(error_prototype_ptr2);
    suppressed_error_prototype->set_property("name", Value(std::string("SuppressedError")), PropertyAttributes::BuiltinFunction);
    suppressed_error_prototype->set_property("message", Value(std::string("")), PropertyAttributes::BuiltinFunction);
    Object* suppressed_proto_ptr = suppressed_error_prototype.get();

    auto suppressed_error_constructor = ObjectFactory::create_native_constructor("SuppressedError",
        [suppressed_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value message_arg = args.size() > 2 ? args[2] : Value();
            bool has_message = !message_arg.is_undefined();
            std::string message;
            if (has_message && !error_arg_to_string(ctx, message_arg, message)) return Value();

            auto error_obj = std::make_unique<Error>(Error::Type::Error, message);
            error_obj->set_property("_isError", Value(true), PropertyAttributes::Writable);
            error_obj->set_property("name", Value(std::string("SuppressedError")));
            error_obj->set_prototype(resolve_error_prototype(ctx, suppressed_proto_ptr));
            // Insertion order matters (order-of-args-evaluation): message, then error, then suppressed.
            if (has_message) {
                error_obj->set_property("message", Value(message), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            }
            error_obj->set_property("error", args.size() > 0 ? args[0] : Value(), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            error_obj->set_property("suppressed", args.size() > 1 ? args[1] : Value(), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            return Value(error_obj.release());
        }, 3);

    PropertyDescriptor se_constructor_desc(Value(suppressed_error_constructor.get()), PropertyAttributes::BuiltinFunction);
    suppressed_error_prototype->set_property_descriptor("constructor", se_constructor_desc);
    suppressed_error_constructor->set_property("prototype", Value(suppressed_error_prototype.release()), PropertyAttributes::None);

    Object* error_ctor2 = nullptr;
    {
        Value ep = ctx.get_global_object()->get_property("Error");
        if (ep.is_function()) error_ctor2 = static_cast<Object*>(ep.as_function());
    }
    if (error_ctor2) suppressed_error_constructor->set_prototype(error_ctor2);

    ctx.register_built_in_object("SuppressedError", suppressed_error_constructor.release());
}

} // namespace Quanta
