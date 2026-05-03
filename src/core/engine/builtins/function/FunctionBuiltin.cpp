/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/FunctionBuiltin.h"
#include "quanta/core/engine/builtins/ArrayBuiltin.h"
#include "quanta/core/engine/builtins/StringBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ProxyReflect.h"

namespace Quanta {

void register_function_builtins(Context& ctx) {
    auto function_constructor = ObjectFactory::create_native_constructor("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // ES1: new Function([arg1[, arg2[, ... argN]],] functionBody)
            // Last argument is the function body, previous arguments are parameter names

            std::string params = "";
            std::string body = "";

            if (args.size() == 0) {
                // new Function() - empty function
                body = "";
            } else if (args.size() == 1) {
                // new Function(body) - no parameters
                body = args[0].to_string();
            } else {
                // new Function(param1, param2, ..., body)
                for (size_t i = 0; i < args.size() - 1; i++) {
                    if (i > 0) params += ",";
                    params += args[i].to_string();
                }
                body = args[args.size() - 1].to_string();
            }

            // Check if body contains "use strict" directive
            bool is_strict = false;
            {
                std::string trimmed = body;
                size_t start = trimmed.find_first_not_of(" \t\n\r");
                if (start != std::string::npos) {
                    std::string first = trimmed.substr(start);
                    if (first.find("\"use strict\"") == 0 || first.find("'use strict'") == 0) {
                        is_strict = true;
                    }
                }
            }

            // ES5: Check for duplicate parameters in strict mode
            if (is_strict && args.size() > 2) {
                std::vector<std::string> param_list;
                for (size_t i = 0; i < args.size() - 1; i++) {
                    std::string p = args[i].to_string();
                    for (size_t j = 0; j < param_list.size(); j++) {
                        if (param_list[j] == p) {
                            ctx.throw_syntax_error("Duplicate parameter name not allowed in strict mode");
                            return Value();
                        }
                    }
                    param_list.push_back(p);
                }
            }

            // Spec-compliant Function constructor toString format
            std::string toString_src = "function anonymous(" + params + "\n) {\n" + body + "\n}";
            std::string func_code = "(" + toString_src + ")";

            // Parse and create the function
            try {
                Lexer lexer(func_code);
                TokenSequence tokens = lexer.tokenize();
                Parser parser(tokens);
                parser.set_source(func_code);
                auto expr = parser.parse_expression();

                // Check for parser errors (e.g. invalid syntax in body)
                if (parser.has_errors()) {
                    ctx.throw_syntax_error("Invalid function body");
                    return Value();
                }

                // The expression should be a function expression
                if (expr && expr->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                    FunctionExpression* func_expr = static_cast<FunctionExpression*>(expr.get());

                    // Clone parameters to preserve rest/default info
                    std::vector<std::unique_ptr<Parameter>> cloned_params;
                    for (const auto& param : func_expr->get_params()) {
                        cloned_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                    }

                    std::unique_ptr<Function> func = ObjectFactory::create_js_function(
                        "anonymous", // ES6: new Function creates "anonymous" named function
                        std::move(cloned_params),
                        func_expr->get_body()->clone(),
                        &ctx
                    );
                    if (!func) {
                        ctx.throw_syntax_error("Failed to create function object");
                        return Value();
                    }
                    func->set_source_text(toString_src);

                    Function* raw_func = func.release();
                    Value new_target = ctx.get_new_target();
                    if (!new_target.is_undefined()) {
                        Object* nt_obj = new_target.is_function()
                            ? static_cast<Object*>(new_target.as_function())
                            : new_target.is_object() ? new_target.as_object() : nullptr;
                        if (nt_obj) {
                            Value nt_proto = nt_obj->get_property("prototype");
                            if (nt_proto.is_object()) raw_func->set_prototype(nt_proto.as_object());
                        }
                    }
                    return Value{raw_func};
                }
            } catch (...) {
                ctx.throw_syntax_error("Invalid function body in Function constructor");
                return Value();
            }

            ctx.throw_syntax_error("Failed to create function");
            return Value();
        });
    
    // Function.prototype must be callable (spec: it's an intrinsic function object)
    // Create it as a native function that returns undefined when called.
    // NOTE: create_native_function checks get_function_prototype() which is null at this point,
    // so the proto of function_prototype won't be set here; it will be set later (Object.prototype).
    auto function_prototype_fn = ObjectFactory::create_native_function("",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args; return Value();
        });
    auto function_prototype = std::unique_ptr<Object>(static_cast<Object*>(function_prototype_fn.release()));
    
    // Array builtin - moved to builtins/array/ArrayBuiltin.cpp
    register_array_builtins(ctx, function_prototype.get());
    

    // Set function prototype early so create_native_function can use it
    Object* function_proto_ptr = function_prototype.get();
    ObjectFactory::set_function_prototype(function_proto_ptr);
    
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.call called on non-function");
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            std::vector<Value> call_args;
            for (size_t i = 1; i < args.size(); i++) {
                call_args.push_back(args[i]);
            }
            
            return func->call(ctx, call_args, this_arg);
        });

    PropertyDescriptor call_length_desc(Value(1.0), PropertyAttributes::Configurable);
    call_length_desc.set_enumerable(false);
    call_length_desc.set_writable(false);
    call_fn->set_property_descriptor("length", call_length_desc);

    call_fn->set_property("name", Value(std::string("call")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("call", Value(call_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            Function* func = nullptr;
            if (function_obj && function_obj->is_function()) {
                func = static_cast<Function*>(function_obj);
            } else if (function_obj && function_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                Object* proxy_target = proxy_obj->get_proxy_target();
                if (proxy_target && proxy_target->is_function()) {
                    func = static_cast<Function*>(proxy_target);
                }
            }
            if (!func) {
                ctx.throw_type_error("Function.prototype.apply called on non-function");
                return Value();
            }
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            std::vector<Value> call_args;
            if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
                if (args[1].is_object()) {
                    Object* args_array = args[1].as_object();
                    // ES5: Accept any array-like object (object with length property)
                    Value length_val = args_array->get_property("length");
                    if (length_val.is_number()) {
                        uint32_t length = static_cast<uint32_t>(length_val.to_number());
                        for (uint32_t i = 0; i < length; i++) {
                            // Use get_property for array-like objects (not just arrays)
                            Value element = args_array->get_property(std::to_string(i));
                            call_args.push_back(element);
                        }
                    }
                }
            }
            
            return func->call(ctx, call_args, this_arg);
        });

    PropertyDescriptor apply_length_desc(Value(2.0), PropertyAttributes::Configurable);
    apply_length_desc.set_enumerable(false);
    apply_length_desc.set_writable(false);
    apply_fn->set_property_descriptor("length", apply_length_desc);

    apply_fn->set_property("name", Value(std::string("apply")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("apply", Value(apply_fn.release()), PropertyAttributes::BuiltinFunction);

    auto bind_fn = ObjectFactory::create_native_function("bind",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            // Accept plain functions or Proxy objects wrapping a function
            Function* target_func = nullptr;
            if (function_obj && function_obj->is_function()) {
                target_func = static_cast<Function*>(function_obj);
            } else if (function_obj && function_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                Object* proxy_target = proxy_obj->get_proxy_target();
                if (proxy_target && proxy_target->is_function()) {
                    target_func = static_cast<Function*>(proxy_target);
                }
            }
            if (!target_func) {
                ctx.throw_type_error("Function.prototype.bind called on non-function");
                return Value();
            }

            Value bound_this = args.size() > 0 ? args[0] : Value();

            std::vector<Value> bound_args;
            for (size_t i = 1; i < args.size(); i++) {
                bound_args.push_back(args[i]);
            }

            // Spec: HasOwnProperty(Target,"length") fires getOwnPropertyDescriptor trap,
            // then Get(Target,"length") fires get trap on Proxy
            double target_length = 0.0;
            {
                if (function_obj->get_type() == Object::ObjectType::Proxy) {
                    Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                    proxy_obj->get_own_property_descriptor_trap(Value(std::string("length")));
                }
                Value target_length_val = function_obj->get_property("length");
                target_length = target_length_val.is_number() ? target_length_val.as_number() : 0.0;
            }
            double bound_length = target_length - static_cast<double>(bound_args.size());
            if (bound_length < 0) bound_length = 0;
            uint32_t bound_arity = static_cast<uint32_t>(bound_length);

            Value name_val = function_obj->get_property("name");
            std::string bound_name = "bound " + (name_val.is_string() ? name_val.to_string() : target_func->get_name());
            auto bound_function = ObjectFactory::create_native_constructor(bound_name,
                [target_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                    std::vector<Value> final_args = bound_args;
                    final_args.insert(final_args.end(), call_args.begin(), call_args.end());

                    // If called as constructor, ignore bound this and use new object
                    if (ctx.is_in_constructor_call()) {
                        return target_func->construct(ctx, final_args);
                    } else {
                        return target_func->call(ctx, final_args, bound_this);
                    }
                }, bound_arity);

            // ES6 19.2.3.2 step 12: bound function's [[Prototype]] should be target's [[Prototype]]
            bound_function->set_prototype(target_func->get_prototype());

            return Value(bound_function.release());
        });

    PropertyDescriptor bind_length_desc(Value(1.0), PropertyAttributes::Configurable);
    bind_length_desc.set_enumerable(false);
    bind_length_desc.set_writable(false);
    bind_fn->set_property_descriptor("length", bind_length_desc);

    bind_fn->set_property("name", Value(std::string("bind")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("bind", Value(bind_fn.release()), PropertyAttributes::BuiltinFunction);

    auto function_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.toString called on non-function");
                return Value();
            }

            Function* func = static_cast<Function*>(function_obj);
            return Value(func->to_string());
        });

    PropertyDescriptor function_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    function_toString_length_desc.set_enumerable(false);
    function_toString_length_desc.set_writable(false);
    function_toString_fn->set_property_descriptor("length", function_toString_length_desc);

    function_toString_fn->set_property("name", Value(std::string("toString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("toString", Value(function_toString_fn.release()), PropertyAttributes::BuiltinFunction);

    function_prototype->set_property("name", Value(std::string("")), PropertyAttributes::Configurable);

    // Set Function.prototype's prototype to Object.prototype so Function objects inherit Object methods
    Object* object_proto = ObjectFactory::get_object_prototype();
    if (object_proto) {
        function_prototype->set_prototype(object_proto);
    }

    PropertyDescriptor function_proto_ctor_desc(Value(function_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    function_proto_ptr->set_property_descriptor("constructor", function_proto_ctor_desc);

    {
        Object* fp = function_prototype.release();
        Value fp_val = fp->is_function() ? Value(static_cast<Function*>(fp)) : Value(fp);
        PropertyDescriptor fp_desc(fp_val, PropertyAttributes::None);
        function_constructor->set_property_descriptor("prototype", fp_desc);
    }

    static_cast<Object*>(function_constructor.get())->set_prototype(function_proto_ptr);

    ctx.register_built_in_object("Function", function_constructor.release());

    // String builtin - moved to builtins/string/StringBuiltin.cpp
    register_string_builtins(ctx);
}

} // namespace Quanta
