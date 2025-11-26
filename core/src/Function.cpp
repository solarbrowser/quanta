/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Object.h"
#include "../include/Context.h"
#include "../include/Engine.h"
#include "../include/CallStack.h"
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

// Backward compatibility constructor
Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr),
      prototype_(nullptr), is_native_(true), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    // Create default prototype object for native functions too
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();

    // Add standard function properties for native functions
    // Set name property with proper descriptor per ES7 spec: { writable: false, enumerable: false, configurable: true }
    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    // Set length property with proper descriptor per ES7 spec: { writable: false, enumerable: false, configurable: true }
    PropertyDescriptor length_desc(Value(static_cast<double>(0)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

// New constructor with arity
Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   uint32_t arity)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr),
      prototype_(nullptr), is_native_(true), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    // Create default prototype object for native functions too
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties for native functions
    // Set name property with proper descriptor per ES7 spec: { writable: false, enumerable: false, configurable: true }
    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    // Set length property with proper descriptor per ES7 spec: { writable: false, enumerable: false, configurable: true }
    PropertyDescriptor length_desc(Value(static_cast<double>(arity)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);
    
}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // Push function call onto stack trace
    CallStack& stack = CallStack::instance();
    Position call_position(1, 1, 0); // TODO: Get actual position from call site
    CallStackFrameGuard frame_guard(stack, get_name(), ctx.get_current_filename(), call_position, this);
    
    // optimized: Track function execution for hot function detection
    execution_count_++;
    last_call_time_ = std::chrono::high_resolution_clock::now();
    
    // Advanced optimization for hot functions
    if (execution_count_ >= 3) {
        // Enable maximum inlining and loop unrolling hints
        __builtin_prefetch(this, 0, 3); // Prefetch function object
        __builtin_prefetch(body_.get(), 0, 3); // Prefetch AST body
        __builtin_prefetch(&args, 0, 2); // Prefetch arguments
        __builtin_prefetch(&ctx, 0, 2); // Prefetch context
    }
    
    // Optimization pipeline with hot function detection
    // Hot function detection after 2 calls  
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

        // PRIMITIVE WRAPPER: Also bind 'this' for primitive values in context
        Value old_this_value = Value();
        bool had_this_binding = false;
        try {
            old_this_value = ctx.get_binding("this");
            had_this_binding = true;
        } catch (...) {
            // No existing 'this' binding
        }

        // DEBUG: Check this_value before binding (disabled for performance)
        // if (this_value.is_object()) {
        //     Object* this_obj = this_value.as_object();
        //     std::cout << "DEBUG Function::call: this_value is object with type = " << static_cast<int>(this_obj->get_type())
        //              << ", is_array() = " << this_obj->is_array() << std::endl;
        // } else {
        //     std::cout << "DEBUG Function::call: this_value is not object, type = "
        //              << (this_value.is_null() ? "null" : this_value.is_undefined() ? "undefined" : "other") << std::endl;
        // }

        // Set 'this' binding for ALL values including null and undefined
        ctx.set_binding("this", this_value);

        // SPECIAL CASE: For primitive values, also set a special binding that preserves the type
        if (this_value.is_number() || this_value.is_string() || this_value.is_boolean() ||
            this_value.is_null() || this_value.is_undefined()) {
            ctx.set_binding("__primitive_this__", this_value);
        }
        
        // Call native C++ function
        Value result = native_fn_(ctx, args);

        // Restore old 'this' binding
        ctx.set_this_binding(old_this);

        // Restore old primitive 'this' binding
        if (had_this_binding) {
            ctx.set_binding("this", old_this_value);
        } else {
            // Remove the 'this' binding if it didn't exist before
            try {
                ctx.delete_binding("this");
            } catch (...) {
                // Ignore if deletion fails
            }
        }

        return result;
    }
    
    // Create new execution context for function with proper closure context management
    Context* parent_context = nullptr;
    
    // CLOSURE FIX: Always prefer closure_context_ if it exists for proper closure behavior
    if (closure_context_ && closure_context_->get_engine() == ctx.get_engine()) {
        parent_context = closure_context_;
    } else {
        // Closure context is invalid, null, or from different engine - use current context
        parent_context = &ctx;
    }
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    Context& function_context = *function_context_ptr;

    // Set up 'this' binding for JavaScript function
    if (this_value.is_object() || this_value.is_function()) {
        Object* this_obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
        function_context.set_this_binding(this_obj);
    }

    // CLOSURE FIX: Restore captured closure variables to function context
    auto prop_keys = this->get_own_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
            std::string var_name = key.substr(10); // Remove "__closure_" prefix
            Value closure_value = this->get_property(key);
            function_context.create_binding(var_name, closure_value, true);
        }
    }

    // GLOBAL VARIABLE ACCESS FIX: Function context should now inherit from global context
    
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
        // VARIABLE HOISTING: Pre-declare all var variables with undefined
        // This is required for proper JavaScript semantics
        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            // If body is a BlockStatement, we can scan for var declarations
            scan_for_var_declarations(body_.get(), function_context);
        }

        Value result = body_->evaluate(function_context);

        // CLOSURE WRITE-BACK: Update captured closure variables that were modified
        auto prop_keys = this->get_own_property_keys();
        for (const auto& key : prop_keys) {
            if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
                std::string var_name = key.substr(10); // Remove "__closure_" prefix

                // Check if the closure variable was modified in the function context
                if (function_context.has_binding(var_name)) {
                    Value current_value = function_context.get_binding(var_name);
                    Value original_value = this->get_property(key);

                    // If the value changed, update the captured property
                    // Use simple comparison - if values are different types or values, update
                    bool values_different = false;
                    if (current_value.get_type() != original_value.get_type()) {
                        values_different = true;
                    } else if (current_value.is_number() && original_value.is_number()) {
                        values_different = (current_value.as_number() != original_value.as_number());
                    } else if (current_value.is_string() && original_value.is_string()) {
                        values_different = (current_value.as_string() != original_value.as_string());
                    } else if (current_value.is_boolean() && original_value.is_boolean()) {
                        values_different = (current_value.as_boolean() != original_value.as_boolean());
                    } else {
                        // For other types, assume they're different if we got here
                        values_different = true;
                    }

                    if (values_different) {
                        this->set_property(key, current_value);

                        // CRITICAL FIX: Also update the original closure context
                        if (closure_context_) {
                            closure_context_->set_binding(var_name, current_value);
                        }
                    }
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
        // ALWAYS check descriptor first for length property
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.has_value() && desc.is_data_descriptor()) {
            return desc.get_value();
        }
        // Fallback to function arity only if no descriptor
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

// Override set_property to handle "prototype" specially
bool Function::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    // Special handling for "prototype" property
    if (key == "prototype") {
        if (value.is_object()) {
            prototype_ = value.as_object();
            return true;
        }
        // If setting to non-object, clear prototype
        prototype_ = nullptr;
        return true;
    }
    
    // For all other properties, use parent class implementation
    return Object::set_property(key, value, attrs);
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    // Create new object instance
    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    // Set up prototype chain
    Value constructor_prototype = get_property("prototype");
    if (constructor_prototype.is_object()) {
        Object* proto_obj = constructor_prototype.as_object();
        new_object->set_prototype(proto_obj);
        // Also set __proto__ property for JavaScript access
        new_object->set_property("__proto__", constructor_prototype);
    }
    
    // Set up super constructor binding for inheritance
    Value super_constructor_prop = get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        // Temporarily bind super constructor as __super__ in the context for constructor execution
        ctx.create_binding("__super__", super_constructor_prop);
    }
    
    // Store initial object state to detect if constructor did anything
    std::vector<std::string> initial_properties = new_object->get_own_property_keys();
    size_t initial_prop_count = initial_properties.size();
    
    // Call function with 'this' bound to new object
    Value result = call(ctx, args, this_value);
    
    // Check if constructor did anything (added properties to this)
    std::vector<std::string> final_properties = new_object->get_own_property_keys();
    bool constructor_did_work = (final_properties.size() > initial_prop_count);
    
    // If constructor didn't do anything and we have a super constructor, call it
    if (!constructor_did_work && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        Function* super_constructor = super_constructor_prop.as_function();
        Value super_result = super_constructor->call(ctx, args, this_value);
        
        // Update result if super constructor returned something meaningful
        if (!super_result.is_undefined()) {
            result = super_result;
        }
    }
    
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

// Backward compatibility overload
std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    return std::make_unique<Function>(name, fn, 0);
}

// New overload with arity
std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                 uint32_t arity) {
    return std::make_unique<Function>(name, fn, arity);
}

} // namespace ObjectFactory

void Function::scan_for_var_declarations(ASTNode* node, Context& ctx) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        // Only hoist var declarations, not let/const
        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                // Create binding with undefined value if it doesn't already exist
                if (!ctx.has_binding(name)) {
                    ctx.create_var_binding(name, Value(), true); // undefined, mutable
                }
            }
        }
    }

    // Recursively scan child nodes for nested var declarations
    // Note: This is a simplified version - a full implementation would need
    // to handle all possible AST node types that can contain statements
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
    // Add more node types as needed for complete hoisting coverage
}

} // namespace Quanta