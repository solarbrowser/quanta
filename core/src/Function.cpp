/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Object.h"
#include "../include/Context.h"
#include "../include/InlineCache.h"
#include "../../parser/include/AST.h"
#include <sstream>
#include <iostream>
#include <chrono>

// Forward declarations to avoid circular dependencies
namespace Quanta {
    class Engine;
    class JITCompiler;
}

namespace Quanta {

//=============================================================================
// Function Implementation
//=============================================================================

Function::Function(const std::string& name, 
                   const std::vector<std::string>& params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameters_(params), 
      body_(std::move(body)), closure_context_(closure_context), 
      prototype_(nullptr), is_native_(false), execution_count_(0), is_hot_(false) {
    // Create default prototype object
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties
    this->set_property("name", Value(name_));
    this->set_property("length", Value(static_cast<double>(parameters_.size())));
    
}

Function::Function(const std::string& name,
                   std::vector<std::unique_ptr<Parameter>> params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameter_objects_(std::move(params)),
      body_(std::move(body)), closure_context_(closure_context), 
      prototype_(nullptr), is_native_(false), execution_count_(0), is_hot_(false) {
    // Extract parameter names for compatibility
    for (const auto& param : parameter_objects_) {
        parameters_.push_back(param->get_name()->get_name());
    }
    
    // Create default prototype object
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties
    this->set_property("name", Value(name_));
    this->set_property("length", Value(static_cast<double>(parameters_.size())));
    
    // Set standard function properties
    Object::set_property("name", Value(name_), PropertyAttributes::Default);
    Object::set_property("length", Value(static_cast<double>(parameters_.size())), PropertyAttributes::Default);
    Object::set_property("prototype", Value(prototype_), PropertyAttributes::Default);
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr), 
      prototype_(nullptr), is_native_(true), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    // Create default prototype object for native functions too
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties for native functions
    this->set_property("name", Value(name_));
    this->set_property("length", Value(0.0));  // Native functions default to 0
    
}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // optimized: Track function execution for hot function detection
    execution_count_++;
    last_call_time_ = std::chrono::high_resolution_clock::now();
    
    // Ultra-aggressive optimization for hot functions
    if (execution_count_ >= 3) {
        // Enable maximum inlining and loop unrolling hints
        __builtin_prefetch(this, 0, 3); // Prefetch function object
        __builtin_prefetch(body_.get(), 0, 3); // Prefetch AST body
        __builtin_prefetch(&args, 0, 2); // Prefetch arguments
        __builtin_prefetch(&ctx, 0, 2); // Prefetch context
    }
    
    // V8-STYLE OPTIMIZATION PIPELINE (simplified for stability)
    // Phase 1: Hot function detection after 2 calls  
    if (execution_count_ >= 2 && !is_hot_) {
        is_hot_ = true;
    }
    
    if (is_native_) {
        // Check for excessive recursion depth to prevent infinite loops
        if (!ctx.check_execution_depth()) {
            ctx.throw_exception(Value("Maximum call stack size exceeded"));
            return Value();
        }
        
        // Set up 'this' binding for native function
        Object* old_this = ctx.get_this_binding();
        if (this_value.is_object() || this_value.is_function()) {
            Object* this_obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
            ctx.set_this_binding(this_obj);
        }
        
        // Call native C++ function
        Value result = native_fn_(ctx, args);
        
        // Restore old 'this' binding
        ctx.set_this_binding(old_this);
        
        return result;
    }
    
    // Create new execution context for function with proper closure context management
    Context* parent_context = nullptr;
    
    // Use closure_context_ if it exists and is from the same engine, otherwise use current context
    if (closure_context_ && closure_context_->get_engine() == ctx.get_engine()) {
        parent_context = closure_context_;
    } else {
        // Closure context is invalid, null, or from different engine - use current context
        parent_context = &ctx;
        
        // Don't update closure_context_ to preserve original closure semantics when possible
    }
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    
    // CLOSURE FIX: Use captured closure variables stored in function properties
    // But maintain persistent state between calls by using the function object as storage
    auto property_names = this->get_own_property_keys();
    for (const auto& prop_name : property_names) {
        if (prop_name.substr(0, 10) == "__closure_") {
            std::string var_name = prop_name.substr(10); // Remove "__closure_" prefix
            Value captured_value = this->get_property(prop_name);
            
            // Create binding that references the function's property for persistence
            function_context_ptr->create_binding(var_name, captured_value, false);
        }
    }
    Context& function_context = *function_context_ptr;
    
    // Bind parameters to arguments with default value support
    if (!parameter_objects_.empty()) {
        // Use parameter objects with default values and rest parameters
        size_t regular_param_count = 0;
        
        // First pass: count regular parameters and find rest parameter
        for (const auto& param : parameter_objects_) {
            if (!param->is_rest()) {
                regular_param_count++;
            }
        }
        
        // Second pass: bind parameters
        for (size_t i = 0; i < parameter_objects_.size(); ++i) {
            const auto& param = parameter_objects_[i];
            
            if (param->is_rest()) {
                // Rest parameter - create array with remaining arguments
                auto rest_array = ObjectFactory::create_array(0);
                
                // Add remaining arguments to the rest array
                for (size_t j = regular_param_count; j < args.size(); ++j) {
                    rest_array->push(args[j]);
                }
                
                function_context.create_binding(param->get_name()->get_name(), Value(rest_array.release()), false);
            } else {
                // Regular parameter
                Value arg_value;
                
                if (i < args.size()) {
                    // Use provided argument
                    arg_value = args[i];
                } else if (param->has_default()) {
                    // Use default value
                    arg_value = param->get_default_value()->evaluate(function_context);
                    if (function_context.has_exception()) return Value();
                } else {
                    // No argument and no default - undefined
                    arg_value = Value();
                }
                
                function_context.create_binding(param->get_name()->get_name(), arg_value, false);
            }
        }
    } else {
        // Fallback to old parameter binding for compatibility
        for (size_t i = 0; i < parameters_.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value(); // undefined if not provided
            function_context.create_binding(parameters_[i], arg_value, false);
        }
    }
    
    // Create arguments object (ES5 feature)
    auto arguments_obj = ObjectFactory::create_array(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        arguments_obj->set_element(i, args[i]);
    }
    arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
    function_context.create_binding("arguments", Value(arguments_obj.release()), false);
    
    // Bind 'this' value
    function_context.create_binding("this", this_value, false);
    
    // Bind super constructor for super() calls if this function has one
    if (this->has_property("__super_constructor__")) {
        Value super_constructor = this->get_property("__super_constructor__");
        if (super_constructor.is_function()) {
            function_context.create_binding("__super__", super_constructor, false);
        }
    }
    
    // Execute function body
    if (body_) {
        Value result = body_->evaluate(function_context);
        
        // CLOSURE FIX: Write closure variable changes back to function properties for persistence
        auto property_names_after = this->get_own_property_keys();
        for (const auto& prop_name : property_names_after) {
            if (prop_name.substr(0, 10) == "__closure_") {
                std::string var_name = prop_name.substr(10); // Remove "__closure_" prefix
                if (function_context.has_binding(var_name)) {
                    Value updated_value = function_context.get_binding(var_name);
                    this->set_property(prop_name, updated_value); // Write back to function property
                }
            }
        }
        
        // Handle return statements or exceptions
        if (function_context.has_return_value()) {
            return function_context.get_return_value();
        }
        
        if (function_context.has_exception()) {
            ctx.throw_exception(function_context.get_exception());
            return Value();
        }
        
        return result.is_undefined() ? Value() : result; // Default return undefined
    }
    
    return Value(); // undefined
}

Value Function::get_property(const std::string& key) const {
    // Handle standard function properties first
    if (key == "name") {
        return Value(name_);
    }
    if (key == "length") {
        return Value(static_cast<double>(parameters_.size()));
    }
    if (key == "prototype") {
        return Value(prototype_);
    }
    
    // Handle Function.prototype methods - available on all function instances
    if (key == "call") {
        auto call_fn = ObjectFactory::create_native_function("call",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                // Get the function that call was invoked on
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.call called on non-function"));
                    return Value();
                }
                
                Function* func = static_cast<Function*>(function_obj);
                Value this_arg = args.size() > 0 ? args[0] : Value();
                
                // Prepare arguments (skip the first 'this' argument)
                std::vector<Value> call_args;
                for (size_t i = 1; i < args.size(); i++) {
                    call_args.push_back(args[i]);
                }
                
                return func->call(ctx, call_args, this_arg);
            });
        return Value(call_fn.release());
    }
    
    if (key == "apply") {
        auto apply_fn = ObjectFactory::create_native_function("apply",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                // Get the function that apply was invoked on
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.apply called on non-function"));
                    return Value();
                }
                
                Function* func = static_cast<Function*>(function_obj);
                Value this_arg = args.size() > 0 ? args[0] : Value();
                
                // Prepare arguments from array
                std::vector<Value> call_args;
                if (args.size() > 1 && args[1].is_object()) {
                    Object* args_array = args[1].as_object();
                    if (args_array->is_array()) {
                        uint32_t length = args_array->get_length();
                        for (uint32_t i = 0; i < length; i++) {
                            call_args.push_back(args_array->get_element(i));
                        }
                    }
                }
                
                return func->call(ctx, call_args, this_arg);
            });
        return Value(apply_fn.release());
    }
    
    if (key == "bind") {
        auto bind_fn = ObjectFactory::create_native_function("bind",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                // Get the function that bind was invoked on
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.bind called on non-function"));
                    return Value();
                }
                
                Function* original_func = static_cast<Function*>(function_obj);
                Value bound_this = args.size() > 0 ? args[0] : Value();
                
                // Create bound arguments (skip the first 'this' argument)
                std::vector<Value> bound_args;
                for (size_t i = 1; i < args.size(); i++) {
                    bound_args.push_back(args[i]);
                }
                
                // Create a new function that when called, calls the original with bound this and args
                auto bound_fn = ObjectFactory::create_native_function("bound " + original_func->get_name(),
                    [original_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                        // Combine bound args with call args
                        std::vector<Value> final_args = bound_args;
                        final_args.insert(final_args.end(), call_args.begin(), call_args.end());
                        
                        return original_func->call(ctx, final_args, bound_this);
                    });
                return Value(bound_fn.release());
            });
        return Value(bind_fn.release());
    }
    
    // For other properties, check own properties directly
    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }
    
    // Check prototype chain manually to avoid calling Object::get_property
    Object* current = get_prototype();
    while (current) {
        Value result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }
    
    return Value(); // undefined
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    // Create new object instance
    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    // Set up prototype chain
    if (prototype_) {
        new_object->set_prototype(prototype_);
    }
    
    // Set up super constructor binding for inheritance
    Value super_constructor_prop = get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        // Temporarily bind super constructor as __super__ in the context for constructor execution
        ctx.create_binding("__super__", super_constructor_prop);
    }
    
    // Call function with 'this' bound to new object
    Value result = call(ctx, args, this_value);
    
    // If constructor returns an object, use that; otherwise use the new object
    if (result.is_object() && result.as_object() != new_object.get()) {
        // Constructor returned a different object, use that
        return result;
    } else {
        // Constructor returned nothing, undefined, or 'this' - use the new object
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

//=============================================================================
// ObjectFactory Function Creation
//=============================================================================

namespace ObjectFactory {

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             const std::vector<std::string>& params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    return std::make_unique<Function>(name, params, std::move(body), closure_context);
}

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             std::vector<std::unique_ptr<Parameter>> params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    return std::make_unique<Function>(name, std::move(params), std::move(body), closure_context);
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    return std::make_unique<Function>(name, fn);
}

} // namespace ObjectFactory

} // namespace Quanta