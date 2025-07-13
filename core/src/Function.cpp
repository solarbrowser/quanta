#include "../include/Object.h"
#include "../include/Context.h"
#include "../../parser/include/AST.h"
#include <sstream>

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
    // Create default prototype object
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr), 
      prototype_(nullptr), is_native_(true), native_fn_(native_fn) {
    // Create default prototype object for native functions too
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    if (is_native_) {
        // Call native C++ function
        return native_fn_(ctx, args);
    }
    
    // Create new execution context for function
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);
    Context& function_context = *function_context_ptr;
    
    // Bind parameters to arguments
    for (size_t i = 0; i < parameters_.size(); ++i) {
        Value arg_value = (i < args.size()) ? args[i] : Value(); // undefined if not provided
        function_context.create_binding(parameters_[i], arg_value, false);
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
    if (result.is_object()) {
        return result;
    } else {
        // Transfer ownership to Value
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

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    return std::make_unique<Function>(name, fn);
}

} // namespace ObjectFactory

} // namespace Quanta