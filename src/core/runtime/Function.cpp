/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/parser/AST.h"
#include <sstream>
#include <iostream>
#include <chrono>
#include <unordered_set>

#ifdef _MSC_VER
#include <xmmintrin.h>
#endif

namespace Quanta {
    class Engine;
    class JITCompiler;
}

namespace Quanta {


Function::Function(const std::string& name,
                   const std::vector<std::string>& params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameters_(params),
      body_(std::move(body)), closure_context_(closure_context),
      prototype_(nullptr), is_native_(false), is_constructor_(true), is_arrow_(false), is_class_constructor_(false), is_strict_(false), execution_count_(0), is_hot_(false) {
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();

    this->set_property("prototype", Value(prototype_));

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);
    PropertyDescriptor length_desc(Value(static_cast<double>(parameters_.size())), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

Function::Function(const std::string& name,
                   std::vector<std::unique_ptr<Parameter>> params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameter_objects_(std::move(params)),
      body_(std::move(body)), closure_context_(closure_context),
      prototype_(nullptr), is_native_(false), is_constructor_(true), is_arrow_(false), is_class_constructor_(false), is_strict_(false), execution_count_(0), is_hot_(false) {
    for (const auto& param : parameter_objects_) {
        parameters_.push_back(param->get_name()->get_name());
    }
    
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    this->set_property("prototype", Value(prototype_));

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);
    // ES6: length = number of params before first rest or default
    size_t formal_length = 0;
    for (const auto& param : parameter_objects_) {
        if (param->is_rest() || param->has_default()) break;
        formal_length++;
    }
    PropertyDescriptor length_desc(Value(static_cast<double>(formal_length)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   bool create_prototype)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false), is_strict_(false), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(static_cast<double>(0)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   uint32_t arity,
                   bool create_prototype)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false), is_strict_(false), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(static_cast<double>(arity)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    CallStack& stack = CallStack::instance();
    Position call_position(1, 1, 0);
    CallStackFrameGuard frame_guard(stack, get_name(), ctx.get_current_filename(), call_position, this);

    execution_count_++;
    last_call_time_ = std::chrono::high_resolution_clock::now();

    if (execution_count_ >= 3) {
        #ifdef __GNUC__
        __builtin_prefetch(this, 0, 3);
        __builtin_prefetch(body_.get(), 0, 3);
        __builtin_prefetch(&args, 0, 2);
        __builtin_prefetch(&ctx, 0, 2);
        #elif defined(_MSC_VER)
        _mm_prefetch((const char*)this, _MM_HINT_T0);
        _mm_prefetch((const char*)body_.get(), _MM_HINT_T0);
        _mm_prefetch((const char*)&args, _MM_HINT_T0);
        _mm_prefetch((const char*)&ctx, _MM_HINT_T0);
        #endif
    }
    
    if (execution_count_ >= 2 && !is_hot_) {
        is_hot_ = true;
    }

    // Class constructors must be called with new
    if (is_class_constructor_ && !ctx.is_in_constructor_call()) {
        ctx.throw_exception(Value("TypeError: Class constructor " + name_ + " cannot be invoked without 'new'"));
        return Value();
    }

    if (is_native_) {
        if (!ctx.check_execution_depth()) {
            ctx.throw_exception(Value(std::string("call stack size exceeded")));
            return Value();
        }
        
        Object* old_this = ctx.get_this_binding();
        if (this_value.is_object() || this_value.is_function()) {
            Object* this_obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
            ctx.set_this_binding(this_obj);
        }

        Value old_this_value = Value();
        bool had_this_binding = false;
        try {
            old_this_value = ctx.get_binding("this");
            had_this_binding = true;
        } catch (...) {
        }


        Value actual_this = this_value;

        if (!ctx.is_strict_mode() && (this_value.is_undefined() || this_value.is_null())) {
            Object* global = ctx.get_global_object();
            if (global) {
                actual_this = Value(global);
            }
        }

        if (actual_this.is_object() || actual_this.is_function()) {
            Object* this_obj = actual_this.is_object() ? actual_this.as_object() : actual_this.as_function();
            ctx.set_this_binding(this_obj);
        }

        ctx.set_binding("this", actual_this);

        if (actual_this.is_number() || actual_this.is_string() || actual_this.is_boolean() ||
            actual_this.is_null() || actual_this.is_undefined()) {
            ctx.set_binding("__primitive_this__", actual_this);
        }

        Value result = native_fn_(ctx, args);

        ctx.set_this_binding(old_this);

        if (had_this_binding) {
            ctx.set_binding("this", old_this_value);
        } else {
            try {
                ctx.delete_binding("this");
            } catch (...) {
            }
        }

        return result;
    }
    
    Context* parent_context = &ctx;
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    Context& function_context = *function_context_ptr;

    // RAII guard: transfer function context to Engine's survivor pool on return
    // instead of destroying it. Keeps the context alive for Promise async callbacks
    // that need context_ to point to the defining scope for closure variable lookups.
    Engine* fn_engine = ctx.get_engine();
    struct ContextSurvivorGuard {
        std::unique_ptr<Context>& ptr;
        Engine* eng;
        ContextSurvivorGuard(std::unique_ptr<Context>& p, Engine* e) : ptr(p), eng(e) {}
        ~ContextSurvivorGuard() {
            if (eng && ptr) eng->add_survivor_context(ptr.release());
        }
    } survivor_guard(function_context_ptr, fn_engine);

    // Propagate new.target into function scope
    if (ctx.is_in_constructor_call() && !ctx.get_new_target().is_undefined()) {
        function_context.set_new_target(ctx.get_new_target());
    }

    // Arrow functions capture new.target from enclosing scope
    if (is_arrow_ && this->has_property("__arrow_new_target__")) {
        function_context.set_new_target(this->get_property("__arrow_new_target__"));
    }

    // Check for strict mode BEFORE setting up 'this' binding
    if (is_strict_) {
        function_context.set_strict_mode(true);
    }
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(body_.get());
        block->check_use_strict_directive(function_context);
    }

    Value actual_this = this_value;

    // Arrow functions use their lexical this, ignoring the passed this_value
    if (is_arrow_ && this->has_property("__arrow_this__")) {
        actual_this = this->get_property("__arrow_this__");
    }

    if (!is_arrow_ && !function_context.is_strict_mode() && (this_value.is_undefined() || this_value.is_null())) {
        Object* global = function_context.get_global_object();
        if (global) {
            actual_this = Value(global);
        }
    }

    if (actual_this.is_object() || actual_this.is_function()) {
        Object* this_obj = actual_this.is_object() ? actual_this.as_object() : actual_this.as_function();
        function_context.set_this_binding(this_obj);
    }

    try {
        function_context.create_binding("this", actual_this, true);
    } catch (...) {
        function_context.set_binding("this", actual_this);
    }

    auto prop_keys = this->get_own_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
            std::string var_name = key.substr(10);
            Value closure_value = this->get_property(key);
            if (var_name != "arguments" && var_name != "this") {
                if (parent_context->has_binding(var_name)) {
                    Value parent_val = parent_context->get_binding(var_name);
                    if (!parent_val.is_undefined() && !parent_val.is_function()) {
                        closure_value = parent_val;
                    }
                }
            }
            function_context.create_binding(var_name, closure_value, true);
        }
    }

    
    if (!parameter_objects_.empty()) {
        size_t regular_param_count = 0;
        
        for (const auto& param : parameter_objects_) {
            if (!param->is_rest()) {
                regular_param_count++;
            }
        }
        
        for (size_t i = 0; i < parameter_objects_.size(); ++i) {
            const auto& param = parameter_objects_[i];
            
            if (param->is_rest()) {
                auto rest_array = ObjectFactory::create_array(0);
                
                for (size_t j = regular_param_count; j < args.size(); ++j) {
                    rest_array->push(args[j]);
                }
                
                function_context.create_binding(param->get_name()->get_name(), Value(rest_array.release()), false);
            } else {
                Value arg_value;

                if (i < args.size() && !args[i].is_undefined()) {
                    arg_value = args[i];
                } else if (param->has_default()) {
                    arg_value = param->get_default_value()->evaluate(function_context);
                    if (function_context.has_exception()) {
                        ctx.throw_exception(function_context.get_exception());
                        return Value();
                    }
                } else {
                    arg_value = Value();
                }

                if (param->has_destructuring()) {
                    // ES6: Destructuring parameter - evaluate the pattern with the arg value
                    auto* pattern = param->get_destructuring_pattern();
                    auto* destructuring = dynamic_cast<DestructuringAssignment*>(pattern);
                    if (destructuring) {
                        destructuring->evaluate_with_value(function_context, arg_value);
                        if (function_context.has_exception()) {
                            ctx.throw_exception(function_context.get_exception());
                            return Value();
                        }
                    }
                } else {
                    // ES1: Function parameters are mutable bindings
                    function_context.create_binding(param->get_name()->get_name(), arg_value, true);
                }
            }
        }
    } else {
        for (size_t i = 0; i < parameters_.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value();
            // ES1: Function parameters are mutable bindings
            function_context.create_binding(parameters_[i], arg_value, true);
        }
    }
    
    // Arrow functions don't have their own arguments object - they use the
    // lexical arguments captured from the enclosing scope via __closure_arguments
    if (!is_arrow_) {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        for (size_t i = 0; i < args.size(); i++) {
            arguments_obj->set_element(i, args[i]);
        }
        arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
        // ES5: Arguments object [[Class]] is "Arguments"
        arguments_obj->set_type(Object::ObjectType::Arguments);
        // Arguments should inherit from Object.prototype, not Array.prototype
        Object* obj_proto = ObjectFactory::get_object_prototype();
        if (obj_proto) {
            arguments_obj->set_prototype(obj_proto);
        }

        // ES6: arguments[Symbol.iterator] - own property, array-like iterator
        {
            Object* args_ptr = arguments_obj.get();
            auto iter_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
                [args_ptr](Context& ctx, const std::vector<Value>& fn_args) -> Value {
                    (void)fn_args;
                    uint32_t length = 0;
                    Value len_val = args_ptr->get_property("length");
                    if (!len_val.is_undefined()) length = static_cast<uint32_t>(len_val.to_number());
                    auto index = std::make_shared<uint32_t>(0);
                    auto iterator = ObjectFactory::create_object();
                    auto next_fn = ObjectFactory::create_native_function("next",
                        [args_ptr, length, index](Context& ctx2, const std::vector<Value>& a) -> Value {
                            (void)a;
                            auto result = ObjectFactory::create_object();
                            if (*index >= length) {
                                result->set_property("done", Value(true));
                                result->set_property("value", Value());
                            } else {
                                result->set_property("done", Value(false));
                                result->set_property("value", args_ptr->get_element(*index));
                                (*index)++;
                            }
                            return Value(result.release());
                        }, 0);
                    iterator->set_property("next", Value(next_fn.release()));
                    return Value(iterator.release());
                }, 0);
            arguments_obj->set_property("Symbol.iterator", Value(iter_fn.release()), PropertyAttributes::BuiltinFunction);
        }

        // In strict mode, arguments.callee and arguments.caller throw TypeError
        if (function_context.is_strict_mode()) {
            auto thrower = ObjectFactory::create_native_function("ThrowTypeError",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    ctx.throw_type_error("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
                    return Value();
                });

            PropertyDescriptor callee_desc;
            callee_desc.set_getter(thrower.get());
            callee_desc.set_setter(thrower.get());
            callee_desc.set_configurable(false);
            callee_desc.set_enumerable(false);
            arguments_obj->set_property_descriptor("callee", callee_desc);

            PropertyDescriptor caller_desc;
            caller_desc.set_getter(thrower.get());
            caller_desc.set_setter(thrower.get());
            caller_desc.set_configurable(false);
            caller_desc.set_enumerable(false);
            arguments_obj->set_property_descriptor("caller", caller_desc);

            thrower.release();
        } else {
            // ES1: In non-strict mode, arguments.callee is the function itself
            PropertyDescriptor callee_desc(Value(this), PropertyAttributes::Default);
            arguments_obj->set_property_descriptor("callee", callee_desc);
        }

        function_context.create_binding("arguments", Value(arguments_obj.release()), false);
    }

    // Use actual_this which respects strict mode (can be undefined in strict mode)
    function_context.create_binding("this", actual_this, false);
    
    if (this->has_property("__super_constructor__")) {
        Value super_constructor = this->get_property("__super_constructor__");
        if (super_constructor.is_function()) {
            function_context.create_binding("__super__", super_constructor, false);
        }
    }
    
    if (body_) {
        // ES5: Named function expressions have their name as an immutable binding
        if (!name_.empty() && name_ != "<anonymous>" && !function_context.has_binding(name_)) {
            function_context.create_binding(name_, Value(this), false);
        }

        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            scan_for_var_declarations(body_.get(), function_context);
        }

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &function_context;
        Value result = body_->evaluate(function_context);
        Object::current_context_ = prev_context;

        // Propagate super_called flag to parent context
        if (function_context.was_super_called()) {
            ctx.set_super_called(true);
        }

        // Re-capture closure variables on function objects in this scope.
        // This handles the case where function declarations are hoisted before
        // var initializations, so their __closure_ properties had stale values.
        {
            auto var_env = function_context.get_variable_environment();
            if (var_env) {
                auto binding_names = var_env->get_binding_names();

                std::vector<std::pair<std::string, Value>> var_values;
                std::vector<Function*> func_objects;

                // Build set of parameter names to exclude from sibling closure propagation
                std::unordered_set<std::string> param_name_set(parameters_.begin(), parameters_.end());
                if (parameter_objects_.empty() == false) {
                    for (const auto& p : parameter_objects_) {
                        param_name_set.insert(p->get_name()->get_name());
                    }
                }

                for (const auto& bname : binding_names) {
                    if (bname == "this" || bname == "arguments") continue;
                    // Parameters are local to this function - don't propagate to siblings
                    if (param_name_set.count(bname)) continue;
                    Value val = function_context.get_binding(bname);
                    if (val.is_function()) {
                        Function* fn = val.as_function();
                        // Skip the current function itself (named function expression
                        // binding) to avoid capturing parameters as stale closures
                        if (fn != this) {
                            func_objects.push_back(fn);
                        }
                    } else {
                        var_values.push_back({bname, val});
                    }
                }

                for (auto* func : func_objects) {
                    for (auto& [vname, vval] : var_values) {
                        func->set_property("__closure_" + vname, vval);
                    }
                }

                // Also update the return value if it's a function not already in scope
                if (function_context.has_return_value()) {
                    Value ret_val = function_context.get_return_value();
                    if (ret_val.is_function()) {
                        Function* ret_func = ret_val.as_function();
                        bool already_updated = false;
                        for (auto* func : func_objects) {
                            if (func == ret_func) {
                                already_updated = true;
                                break;
                            }
                        }
                        if (!already_updated) {
                            for (auto& [vname, vval] : var_values) {
                                ret_func->set_property("__closure_" + vname, vval);
                            }
                        }
                    }
                }
            }
        }

        // Write back modified closure variables to this function object,
        // propagate to parent context, and update sibling closures
        std::vector<std::pair<std::string, Value>> modified_closures;
        auto prop_keys2 = this->get_own_property_keys();
        for (const auto& key : prop_keys2) {
            if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
                std::string var_name = key.substr(10);

                if (function_context.has_binding(var_name)) {
                    Value current_value = function_context.get_binding(var_name);
                    Value old_value = this->get_property(key);
                    this->set_property(key, current_value);

                    if (parent_context->has_binding(var_name)) {
                        parent_context->set_binding(var_name, current_value);
                    }

                    if (!current_value.strict_equals(old_value)) {
                        modified_closures.push_back({var_name, current_value});
                    }
                }
            }
        }

        if (!modified_closures.empty()) {
            auto* var_env = parent_context->get_variable_environment();
            if (var_env) {
                auto sibling_names = var_env->get_binding_names();
                for (const auto& sname : sibling_names) {
                    Value sval = parent_context->get_binding(sname);
                    if (sval.is_function() && sval.as_function() != this) {
                        Function* sibling = sval.as_function();
                        for (auto& [vname, vval] : modified_closures) {
                            if (sibling->has_property("__closure_" + vname)) {
                                sibling->set_property("__closure_" + vname, vval);
                            }
                        }
                    }
                }
            }
        }

        
        if (function_context.has_return_value()) {
            return function_context.get_return_value();
        }
        
        if (function_context.has_exception()) {
            ctx.throw_exception(function_context.get_exception());
            return Value();
        }
        
        return result.is_undefined() ? Value() : result;
    }
    
    return Value();
}

Value Function::get_property(const std::string& key) const {
    if (key == "name") {
        // Check if a "name" property was explicitly overridden (e.g. static name() in class)
        if (descriptors_) {
            auto it = descriptors_->find("name");
            if (it != descriptors_->end()) {
                if (it->second.is_data_descriptor()) {
                    Value desc_val = it->second.get_value();
                    if (desc_val.is_function()) {
                        return desc_val;
                    }
                }
            }
        }
        return Value(name_);
    }
    if (key == "length") {
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.has_value() && desc.is_data_descriptor()) {
            return desc.get_value();
        }
        return Value(static_cast<double>(parameters_.size()));
    }
    if (key == "prototype") {
        return Value(prototype_);
    }

    // call, apply, bind are now handled via Function.prototype
    // No need for special handling here anymore

    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }

    // Lazy initialization: if our internal prototype is not set yet,
    // try to get Function.prototype (may be available now even if it wasn't during construction)
    Object* current = get_prototype();
    if (!current) {
        Object* func_proto = ObjectFactory::get_function_prototype();
        if (func_proto) {
            const_cast<Function*>(this)->set_prototype(func_proto);
            current = func_proto;
        }
    }

    while (current) {
        Value result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }
    
    return Value();
}

void Function::set_name(const std::string& name) {
    name_ = name;
    // Force-update the name in descriptors and shape (bypasses writable check)
    // But don't overwrite if the descriptor was explicitly set to a function (e.g. static name())
    if (descriptors_) {
        auto it = descriptors_->find("name");
        if (it != descriptors_->end() && it->second.is_data_descriptor()) {
            if (!it->second.get_value().is_function()) {
                it->second = PropertyDescriptor(Value(name_), it->second.get_attributes());
            }
        }
    }
    if (header_.shape && header_.shape->has_property("name")) {
        auto info = header_.shape->get_property_info("name");
        if (info.offset < properties_.size()) {
            properties_[info.offset] = Value(name_);
        }
    }
}

bool Function::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (key == "prototype") {
        if (value.is_object()) {
            prototype_ = value.as_object();
            return true;
        }
        if (value.is_function()) {
            prototype_ = value.as_function();
            return true;
        }
        prototype_ = nullptr;
        return true;
    }
    
    return Object::set_property(key, value, attrs);
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    // Check if this function is a constructor
    if (!is_constructor_) {
        ctx.throw_exception(Value("TypeError: " + name_ + " is not a constructor"));
        return Value();
    }

    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    Value constructor_prototype = get_property("prototype");
    if (constructor_prototype.is_object()) {
        Object* proto_obj = constructor_prototype.as_object();
        new_object->set_prototype(proto_obj);
    }
    
    Value super_constructor_prop = get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx.create_binding("__super__", super_constructor_prop);
    }
    
    ctx.set_in_constructor_call(true);
    ctx.set_super_called(false);
    ctx.set_new_target(Value(static_cast<Object*>(this)));
    Value result = call(ctx, args, this_value);
    bool super_was_called = ctx.was_super_called();
    ctx.set_in_constructor_call(false);
    ctx.set_new_target(Value());

    // Auto-call super if the constructor didn't explicitly call super()
    // This handles classes with no explicit constructor (default constructor)
    if (!super_was_called && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        Function* super_constructor = super_constructor_prop.as_function();
        ctx.set_in_constructor_call(true);
        ctx.set_new_target(Value(static_cast<Object*>(this)));
        Value super_result = super_constructor->call(ctx, args, this_value);
        ctx.set_in_constructor_call(false);
        ctx.set_new_target(Value());

        if (!super_result.is_undefined()) {
            result = super_result;
        }
    }

    // If constructor explicitly returned an object or function, use that
    if ((result.is_object() || result.is_function()) && result.as_object() != new_object.get()) {
        return result;
    } else {
        return Value(new_object.release());
    }
}

std::string Function::to_string() const {
    if (is_native_) {
        return "[native function " + name_ + "]";
    }
    
    std::ostringstream oss;
    oss << "function " << name_ << "(";
    for (size_t i = 0; i < parameters_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << parameters_[i];
    }
    oss << ") { [native code] }";
    return oss.str();
}


namespace ObjectFactory {

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             const std::vector<std::string>& params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    auto func = std::make_unique<Function>(name, params, std::move(body), closure_context);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    } else {
        // If function_prototype not set yet, delay prototype assignment
        // It will be set when the function is accessed
    }
    return func;
}

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             std::vector<std::unique_ptr<Parameter>> params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    auto func = std::make_unique<Function>(name, std::move(params), std::move(body), closure_context);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    auto func = std::make_unique<Function>(name, fn, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                 uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_constructor(const std::string& name,
                                                    std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                    uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, true);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

}

void Function::scan_for_var_declarations(ASTNode* node, Context& ctx) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                if (!ctx.has_binding(name)) {
                    ctx.create_var_binding(name, Value(), true);
                }
            }
        }
    }

    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(node);
        for (const auto& stmt : block->get_statements()) {
            scan_for_var_declarations(stmt.get(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::IF_STATEMENT) {
        IfStatement* if_stmt = static_cast<IfStatement*>(node);
        scan_for_var_declarations(if_stmt->get_consequent(), ctx);
        if (if_stmt->get_alternate()) {
            scan_for_var_declarations(if_stmt->get_alternate(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        ForStatement* for_stmt = static_cast<ForStatement*>(node);
        if (for_stmt->get_init()) {
            scan_for_var_declarations(for_stmt->get_init(), ctx);
        }
        scan_for_var_declarations(for_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WHILE_STATEMENT) {
        WhileStatement* while_stmt = static_cast<WhileStatement*>(node);
        scan_for_var_declarations(while_stmt->get_body(), ctx);
    }
}

}
