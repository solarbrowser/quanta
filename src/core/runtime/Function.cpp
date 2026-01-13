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
      prototype_(nullptr), is_native_(false), execution_count_(0), is_hot_(false) {
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
      prototype_(nullptr), is_native_(false), execution_count_(0), is_hot_(false) {
    for (const auto& param : parameter_objects_) {
        parameters_.push_back(param->get_name()->get_name());
    }
    
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();
    
    this->set_property("prototype", Value(prototype_));

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);
    PropertyDescriptor length_desc(Value(static_cast<double>(parameters_.size())), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   bool create_prototype)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr),
      prototype_(nullptr), is_native_(true), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
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
      prototype_(nullptr), is_native_(true), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
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

    // JIT: Try to execute with JIT compiler for hot functions
    if (!is_native_ && ctx.get_engine() && ctx.get_engine()->get_jit_compiler()) {
        auto* jit = ctx.get_engine()->get_jit_compiler();

        // Try JIT function execution
        Value jit_result;
        if (jit->try_execute_jit_function(this, ctx, args, jit_result)) {
            // JIT executed successfully
            return jit_result;
        }
    }

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
    
    if (is_native_) {
        if (!ctx.check_execution_depth()) {
            ctx.throw_exception(Value("call stack size exceeded"));
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
    
    Context* parent_context = nullptr;
    
    if (closure_context_ && closure_context_->get_engine() == ctx.get_engine()) {
        parent_context = closure_context_;
    } else {
        parent_context = &ctx;
    }
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    Context& function_context = *function_context_ptr;

    Value actual_this = this_value;

    if (!function_context.is_strict_mode() && (this_value.is_undefined() || this_value.is_null())) {
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
                
                if (i < args.size()) {
                    arg_value = args[i];
                } else if (param->has_default()) {
                    arg_value = param->get_default_value()->evaluate(function_context);
                    if (function_context.has_exception()) return Value();
                } else {
                    arg_value = Value();
                }
                
                function_context.create_binding(param->get_name()->get_name(), arg_value, false);
            }
        }
    } else {
        for (size_t i = 0; i < parameters_.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value();
            function_context.create_binding(parameters_[i], arg_value, false);
        }
    }
    
    auto arguments_obj = ObjectFactory::create_array(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        arguments_obj->set_element(i, args[i]);
    }
    arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
    function_context.create_binding("arguments", Value(arguments_obj.release()), false);
    
    function_context.create_binding("this", this_value, false);
    
    if (this->has_property("__super_constructor__")) {
        Value super_constructor = this->get_property("__super_constructor__");
        if (super_constructor.is_function()) {
            function_context.create_binding("__super__", super_constructor, false);
        }
    }
    
    if (body_) {
        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            scan_for_var_declarations(body_.get(), function_context);
        }

        Value result = body_->evaluate(function_context);

        auto prop_keys = this->get_own_property_keys();
        for (const auto& key : prop_keys) {
            if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
                std::string var_name = key.substr(10);

                if (function_context.has_binding(var_name)) {
                    Value current_value = function_context.get_binding(var_name);
                    Value original_value = this->get_property(key);

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
                        values_different = true;
                    }

                    if (values_different) {
                        this->set_property(key, current_value);

                        if (closure_context_) {
                            closure_context_->set_binding(var_name, current_value);
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
    
    if (key == "call") {
        auto call_fn = ObjectFactory::create_native_function("call",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.call called on non-function"));
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
        return Value(call_fn.release());
    }
    
    if (key == "apply") {
        auto apply_fn = ObjectFactory::create_native_function("apply",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.apply called on non-function"));
                    return Value();
                }
                
                Function* func = static_cast<Function*>(function_obj);
                Value this_arg = args.size() > 0 ? args[0] : Value();
                
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
                Object* function_obj = ctx.get_this_binding();
                if (!function_obj || !function_obj->is_function()) {
                    ctx.throw_exception(Value("Function.bind called on non-function"));
                    return Value();
                }
                
                Function* original_func = static_cast<Function*>(function_obj);
                Value bound_this = args.size() > 0 ? args[0] : Value();
                
                std::vector<Value> bound_args;
                for (size_t i = 1; i < args.size(); i++) {
                    bound_args.push_back(args[i]);
                }
                
                auto bound_fn = ObjectFactory::create_native_function("bound " + original_func->get_name(),
                    [original_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                        std::vector<Value> final_args = bound_args;
                        final_args.insert(final_args.end(), call_args.begin(), call_args.end());
                        
                        return original_func->call(ctx, final_args, bound_this);
                    });
                return Value(bound_fn.release());
            });
        return Value(bind_fn.release());
    }
    
    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }
    
    Object* current = get_prototype();
    while (current) {
        Value result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }
    
    return Value();
}

bool Function::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (key == "prototype") {
        if (value.is_object()) {
            prototype_ = value.as_object();
            return true;
        }
        prototype_ = nullptr;
        return true;
    }
    
    return Object::set_property(key, value, attrs);
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    Value constructor_prototype = get_property("prototype");
    if (constructor_prototype.is_object()) {
        Object* proto_obj = constructor_prototype.as_object();
        new_object->set_prototype(proto_obj);
        new_object->set_property("__proto__", constructor_prototype);
    }
    
    Value super_constructor_prop = get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx.create_binding("__super__", super_constructor_prop);
    }
    
    std::vector<std::string> initial_properties = new_object->get_own_property_keys();
    size_t initial_prop_count = initial_properties.size();
    
    Value result = call(ctx, args, this_value);
    
    std::vector<std::string> final_properties = new_object->get_own_property_keys();
    bool constructor_did_work = (final_properties.size() > initial_prop_count);
    
    if (!constructor_did_work && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        Function* super_constructor = super_constructor_prop.as_function();
        Value super_result = super_constructor->call(ctx, args, this_value);
        
        if (!super_result.is_undefined()) {
            result = super_result;
        }
    }
    
    // If constructor returned a function, return it directly
    if (result.is_function()) {
        return result;
    }

    if (result.is_object() && result.as_object() != new_object.get()) {
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
