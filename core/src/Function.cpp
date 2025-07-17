#include "../include/Object.h"
#include "../include/Context.h"
#include "../../parser/include/AST.h"
#include <sstream>
#include <iostream>

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
      prototype_(nullptr), is_native_(false) {
    std::cerr << "DEBUG: Function constructor 1 called for: " << name << std::endl;
    // Create default prototype object
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties
    this->set_property("name", Value(name_));
    this->set_property("length", Value(static_cast<double>(parameters_.size())));
    
    // DEBUG: Print what we're setting
    std::cerr << "DEBUG: Setting function properties - name: " << name_ << ", length: " << parameters_.size() << std::endl;
    
    // DEBUG: Force property setting using Object base class
    Object::set_property("prototype", Value(prototype_));
    Object::set_property("name", Value(name_));
    Object::set_property("length", Value(static_cast<double>(parameters_.size())));
    
    // DEBUG: Verify the properties were set
    std::cout << "DEBUG: Checking properties after setting (constructor 2):" << std::endl;
    Value name_check = this->get_property("name");
    Value length_check = this->get_property("length");
    Value prototype_check = this->get_property("prototype");
    std::cout << "DEBUG: name = " << name_check.to_string() << std::endl;
    std::cout << "DEBUG: length = " << length_check.to_string() << std::endl;
    std::cout << "DEBUG: prototype = " << prototype_check.to_string() << std::endl;
}

Function::Function(const std::string& name,
                   std::vector<std::unique_ptr<Parameter>> params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameter_objects_(std::move(params)),
      body_(std::move(body)), closure_context_(closure_context), 
      prototype_(nullptr), is_native_(false) {
    std::cerr << "DEBUG: Function constructor 2 called for: " << name << std::endl;
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
      prototype_(nullptr), is_native_(true), native_fn_(native_fn) {
    // Create default prototype object for native functions too
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    // Make prototype accessible as a property
    this->set_property("prototype", Value(prototype_));
    
    // Add standard function properties for native functions
    this->set_property("name", Value(name_));
    this->set_property("length", Value(0.0));  // Native functions default to 0
    
    // DEBUG: Force property setting using Object base class
    Object::set_property("prototype", Value(prototype_));
    Object::set_property("name", Value(name_));
    Object::set_property("length", Value(0.0));
}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    if (is_native_) {
        // Call native C++ function
        return native_fn_(ctx, args);
    }
    
    // Create new execution context for function
    // Use closure_context_ for proper closure variable capture, fallback to current context
    Context* parent_context = closure_context_ ? closure_context_ : &ctx;
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    
    // CLOSURE FIX: Use captured closure variables stored in function properties
    auto property_names = this->get_own_property_keys();
    for (const auto& prop_name : property_names) {
        if (prop_name.substr(0, 10) == "__closure_") {
            std::string var_name = prop_name.substr(10); // Remove "__closure_" prefix
            Value captured_value = this->get_property(prop_name);
            if (!function_context_ptr->has_binding(var_name)) {
                function_context_ptr->create_binding(var_name, captured_value, false);
            }
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
                auto rest_array = std::make_unique<Object>();
                
                // Add remaining arguments to the rest array
                size_t rest_index = 0;
                for (size_t j = regular_param_count; j < args.size(); ++j) {
                    rest_array->set_property(std::to_string(rest_index), args[j]);
                    rest_index++;
                }
                
                // Set array length
                rest_array->set_property("length", Value(static_cast<double>(rest_index)));
                
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
    
    // Bind 'this' value
    function_context.create_binding("this", this_value, false);
    
    // Execute function body
    if (body_) {
        Value result = body_->evaluate(function_context);
        
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
    // Handle standard function properties
    if (key == "name") {
        return Value(name_);
    }
    if (key == "length") {
        return Value(static_cast<double>(parameters_.size()));
    }
    if (key == "prototype") {
        return Value(prototype_);
    }
    
    // For other properties, use the base Object implementation
    return Object::get_property(key);
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    // Create new object instance
    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    // Set up prototype chain
    if (prototype_) {
        new_object->set_prototype(prototype_);
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
    std::cerr << "DEBUG: ObjectFactory::create_js_function called for: " << name << std::endl;
    return std::make_unique<Function>(name, std::move(params), std::move(body), closure_context);
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    return std::make_unique<Function>(name, fn);
}

} // namespace ObjectFactory

} // namespace Quanta