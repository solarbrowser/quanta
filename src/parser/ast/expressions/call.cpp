/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Math.h"
#include "../ast_internal.h"
#include <sstream>
#include <set>
#include <cmath>
#include <climits>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <cstdlib>
#include <cstdio>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

std::vector<Value> process_arguments_with_spread(const std::vector<std::unique_ptr<ASTNode>>& arguments, Context& ctx) {
    std::vector<Value> arg_values;

    for (const auto& arg : arguments) {
        if (arg->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            SpreadElement* spread = static_cast<SpreadElement*>(arg.get());
            Value spread_value = spread->get_argument()->evaluate(ctx);
            if (ctx.has_exception()) return arg_values;

            if (spread_value.is_object() || spread_value.is_function()) {
                Object* spread_obj = spread_value.is_function()
                    ? static_cast<Object*>(spread_value.as_function())
                    : spread_value.as_object();
                bool used_iterator = false;
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym && !spread_obj->is_array()) {
                    Value iter_method = spread_obj->get_property(iter_sym->to_property_key());
                    if (ctx.has_exception()) return arg_values;
                    if (!iter_method.is_undefined()) {
                        // GetMethod: non-null/undefined non-callable throws TypeError
                        if (!iter_method.is_null() && !iter_method.is_function()) {
                            ctx.throw_type_error("Symbol.iterator is not callable");
                            return arg_values;
                        }
                        if (iter_method.is_null()) {
                            // null means GetMethod returns undefined, Call(undefined) throws TypeError
                            ctx.throw_type_error("Symbol.iterator is not a function");
                            return arg_values;
                        }
                        // iter_method is a function -- call it
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, spread_value);
                        if (ctx.has_exception()) return arg_values;
                        if (!iter_obj.is_object()) {
                            ctx.throw_type_error("Symbol.iterator must return an Object");
                            return arg_values;
                        }
                        Value next_fn = iter_obj.as_object()->get_property("next");
                        if (next_fn.is_function()) {
                            used_iterator = true;
                            for (uint32_t ii = 0; ii < 100000; ii++) {
                                Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                if (ctx.has_exception()) return arg_values;
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                arg_values.push_back(res.as_object()->get_property("value"));
                            }
                        }
                    }
                }
                if (!used_iterator) {
                    uint32_t spread_length = spread_obj->get_length();
                    for (uint32_t j = 0; j < spread_length; ++j) {
                        arg_values.push_back(spread_obj->get_element(j));
                    }
                }
            } else if (spread_value.is_string()) {
                const std::string& str = spread_value.as_string()->str();
                size_t i = 0;
                while (i < str.size()) {
                    unsigned char c = str[i];
                    size_t char_len = 1;
                    if (c >= 0xF0) char_len = 4;
                    else if (c >= 0xE0) char_len = 3;
                    else if (c >= 0xC0) char_len = 2;
                    std::string ch = str.substr(i, char_len);
                    arg_values.push_back(Value(ch));
                    i += char_len;
                }
            } else if (!spread_value.is_null() && !spread_value.is_undefined()) {
                // Non-iterable, non-null: TypeError
                ctx.throw_type_error("Spread syntax requires an iterable");
                return arg_values;
            } else {
                // null/undefined in call spread: TypeError (unlike object spread)
                ctx.throw_type_error("Spread syntax requires an iterable, got " +
                    std::string(spread_value.is_null() ? "null" : "undefined"));
                return arg_values;
            }
        } else {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return arg_values;
            arg_values.push_back(arg_value);
        }
    }

    return arg_values;
}

Value CallExpression::evaluate(Context& ctx) {
    if (callee_->get_type() == ASTNode::Type::OPTIONAL_CHAINING_EXPRESSION) {
        OptionalChainingExpression* opt = static_cast<OptionalChainingExpression*>(callee_.get());
        Value base = opt->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (base.is_null() || base.is_undefined()) {
            return Value();
        }
        // base is non-null: get property and call with base as this
        std::string prop_name;
        if (opt->is_computed()) {
            Value prop_val = opt->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            prop_name = prop_val.to_string();
        } else if (opt->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
            prop_name = static_cast<Identifier*>(opt->get_property())->get_name();
        }
        Value method_val;
        if (base.is_object()) {
            method_val = base.as_object()->get_property(prop_name);
        } else if (base.is_function()) {
            method_val = base.as_function()->get_property(prop_name);
        }
        if (!method_val.is_function()) {
            ctx.throw_type_error(prop_name + " is not a function");
            return Value();
        }
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return method_val.as_function()->call(ctx, arg_values, base);
    }

    if (is_optional_) {
        Value callee_val = callee_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (callee_val.is_null() || callee_val.is_undefined()) {
            return Value();
        }
        if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            return handle_member_expression_call(ctx);
        }
        if (!callee_val.is_function()) {
            ctx.throw_type_error("is not a function");
            return Value();
        }
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return callee_val.as_function()->call(ctx, arg_values);
    }

    // Tagged template calls with MemberExpression callees (e.g. String.raw`...`) must be
    // intercepted here before handle_member_expression_call takes over, since that path
    // uses process_arguments_with_spread which doesn't know about template literals.
    if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION && is_tagged_template_ &&
        arguments_.size() == 1 && arguments_[0]->get_type() == ASTNode::Type::TEMPLATE_LITERAL) {
        // fall through to tagged template block
    } else if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        return handle_member_expression_call(ctx);
    }
    
    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* identifier = static_cast<Identifier*>(callee_.get());
        if (identifier->get_name() == "super") {
            // Second super() call: this is already initialized -> ReferenceError
            if (ctx.was_super_called()) {
                ctx.throw_reference_error("Super constructor called twice");
                return Value();
            }

            // Check for class extends null -- super() always throws TypeError
            Value super_is_null = ctx.get_binding("__super_is_null__");
            if (super_is_null.to_boolean()) {
                ctx.throw_type_error("Super constructor is not a constructor");
                return Value();
            }

            Value parent_constructor = ctx.get_binding("__super__");

            if (parent_constructor.is_undefined()) {
                parent_constructor = ctx.get_binding("__super_constructor__");
            }


            if ((parent_constructor.is_undefined() && parent_constructor.is_function()) ||
                (parent_constructor.is_function() && parent_constructor.as_function() == nullptr)) {
                return Value();
            }

            if (parent_constructor.is_function()) {
                std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                if (ctx.has_exception()) return Value();

                try {
                    Function* parent_func = parent_constructor.as_function();
                    if (!parent_func) {
                        return Value();
                    }

                    // IsConstructor check: calling super() with a non-constructor throws TypeError
                    if (!parent_func->is_constructor()) {
                        ctx.throw_type_error("Super constructor is not a constructor");
                        return Value();
                    }

                    Object* this_obj = ctx.get_this_binding();

                    bool was_in_ctor = ctx.is_in_constructor_call();
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_in_constructor_call(true);
                    if (old_new_target.is_undefined()) {
                        ctx.set_new_target(Value(static_cast<Object*>(parent_func)));
                    }

                    ctx.set_pending_construct_call(true);
                    Value result;
                    if (this_obj) {
                        Value this_value(this_obj);
                        result = parent_func->call(ctx, arg_values, this_value);
                    } else {
                        result = parent_func->call(ctx, arg_values);
                    }
                    ctx.clear_return_value();
                    if (ctx.has_exception()) return Value();

                    ctx.set_in_constructor_call(was_in_ctor);
                    ctx.set_new_target(old_new_target);
                    ctx.set_super_called(true);
                    ctx.set_this_needs_super(false);

                    // InitializeInstanceElements: add per-instance private method brand slot.
                    // This must happen after super() returns so that accessing private methods
                    // before super() correctly throws TypeError (brand slot not yet present).
                    {
                        CallStack& pm_cs = CallStack::instance();
                        if (!pm_cs.is_empty() && pm_cs.top().function_ptr) {
                            Value pm_slot_val = pm_cs.top().function_ptr->get_property("__pm_brand_slot__");
                            if (pm_slot_val.is_string()) {
                                std::string pm_slot = pm_slot_val.to_string();
                                Object* pm_this = this_obj ? this_obj : ctx.get_this_binding();
                                if (pm_this) pm_this->add_private_field(pm_slot);
                            }
                        }
                    }

                    // If parent constructor explicitly returned an object, use that as new this
                    if ((result.is_object() || result.is_function()) && this_obj) {
                        Object* new_this = result.as_object();
                        if (new_this && new_this != this_obj) {
                            ctx.set_this_binding(new_this);
                            ctx.set_binding("this", result);
                        }
                        return result;
                    }

                    // Return the this value
                    if (this_obj) {
                        return Value(this_obj);
                    }
                    return Value();
                } catch (...) {
                    return Value();
                }
            } else {
                return Value();
            }
        }
    }

    Value callee_value = callee_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (callee_value.is_undefined() && callee_value.is_function()) {
        throw std::runtime_error("Invalid Value state: NaN-boxing corruption detected");
    }
    
    if (callee_value.is_function()) {
        // Tagged template literal handling
        if (is_tagged_template_ && arguments_.size() == 1 &&
            arguments_[0]->get_type() == ASTNode::Type::TEMPLATE_LITERAL) {

            TemplateLiteral* tmpl = static_cast<TemplateLiteral*>(arguments_[0].get());
            const auto& elements = tmpl->get_elements();

            // Build the strings array from TEXT elements (no raw-pointer caching to avoid GC issues).
            std::vector<std::string> cooked_parts;
            std::vector<std::string> raw_parts;
            for (const auto& el : elements) {
                if (el.type == TemplateLiteral::Element::Type::TEXT) {
                    cooked_parts.push_back(el.text);
                    raw_parts.push_back(el.raw_text);
                }
            }

            auto strings_obj = ObjectFactory::create_array(static_cast<int>(cooked_parts.size()));
            Object* strings_array = strings_obj.get();
            for (size_t i = 0; i < cooked_parts.size(); i++) {
                if (cooked_parts[i] == "\x01") {
                    strings_array->set_property(std::to_string(i), Value());
                } else {
                    strings_array->set_property(std::to_string(i), Value(cooked_parts[i]));
                }
            }
            strings_array->set_property("length", Value(static_cast<double>(cooked_parts.size())));

            auto raw_obj = ObjectFactory::create_array(static_cast<int>(raw_parts.size()));
            Object* raw_array = raw_obj.get();
            for (size_t i = 0; i < raw_parts.size(); i++) {
                raw_array->set_property(std::to_string(i), Value(raw_parts[i]));
            }
            raw_array->set_property("length", Value(static_cast<double>(raw_parts.size())));

            strings_array->set_property("raw", Value(raw_obj.release()));

            // Build argument list: [strings_array, expr1, expr2, ...]
            std::vector<Value> arg_values;
            arg_values.push_back(Value(strings_obj.release()));

            // Evaluate expression elements
            for (const auto& el : elements) {
                if (el.type == TemplateLiteral::Element::Type::EXPRESSION) {
                    Value expr_val = el.expression->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    arg_values.push_back(expr_val);
                }
            }

            Function* function = callee_value.as_function();
            Value this_value = Value();
            return function->call(ctx, arg_values, this_value);
        }

        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();

        Function* function = callee_value.as_function();

        // In ES5, 'this' should be undefined for non-method calls
        // The function itself will convert to global object if not in strict mode
        Value this_value = Value();  // undefined

        // Direct eval detection: eval(code) called via plain Identifier lookup
        bool is_direct_eval = (callee_->get_type() == ASTNode::Type::IDENTIFIER &&
                               static_cast<Identifier*>(callee_.get())->get_name() == "eval");
        bool saved_direct_eval = ctx.is_direct_eval_call();
        if (is_direct_eval) ctx.set_direct_eval_call(true);
        Value result = function->call(ctx, arg_values, this_value);
        ctx.set_direct_eval_call(saved_direct_eval);
        return result;
    }

    if (callee_value.is_object() &&
        callee_value.as_object()->get_type() == Object::ObjectType::Proxy) {
        Proxy* proxy = static_cast<Proxy*>(callee_value.as_object());
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        return proxy->apply_trap(arg_values, Value());
    }

    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* func_id = static_cast<Identifier*>(callee_.get());
        std::string func_name = func_id->get_name();

        if (false && func_name == "super") {
            Value super_constructor = ctx.get_binding("__super__");
            
            if (super_constructor.is_function()) {
                std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                if (ctx.has_exception()) return Value();
                
                Value this_value = ctx.get_binding("this");
                
                Function* parent_constructor = super_constructor.as_function();
                Value result = parent_constructor->call(ctx, arg_values, this_value);
                return result;
            } else {
                ctx.throw_exception(Value(std::string("super() called but no parent constructor found")));
                return Value();
            }
        }
        
        Value function_value = ctx.get_binding(func_name);

        if (function_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            Function* func = function_value.as_function();
            return func->call(ctx, arg_values);
        } else if (function_value.is_object() &&
                   function_value.as_object()->get_type() == Object::ObjectType::Proxy) {
            Proxy* proxy = static_cast<Proxy*>(function_value.as_object());
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            return proxy->apply_trap(arg_values, Value());
        } else {
            ctx.throw_type_error(func_name + " is not a function");
            return Value();
        }
    }

    if (callee_->get_type() == ASTNode::Type::CALL_EXPRESSION) {
        Value callee_result = callee_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        
        if (callee_result.is_function()) {
            Function* func = callee_result.as_function();
            
            static thread_local int super_call_depth = 0;
            const int MAX_SUPER_DEPTH = 32;
            
            if (ctx.has_binding("__super__") && super_call_depth < MAX_SUPER_DEPTH) {
                Value super_constructor = ctx.get_binding("__super__");
                if (super_constructor.is_function() && super_constructor.as_function() == func) {

                    std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                    if (ctx.has_exception()) return Value();

                    Value this_value = ctx.get_binding("this");

                    super_call_depth++;
                    bool was_in_ctor = ctx.is_in_constructor_call();
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_in_constructor_call(true);
                    if (old_new_target.is_undefined()) {
                        ctx.set_new_target(Value(static_cast<Object*>(func)));
                    }
                    ctx.set_pending_construct_call(true);
                    try {
                        Value result = func->call(ctx, arg_values, this_value);
                        super_call_depth--;
                        ctx.set_in_constructor_call(was_in_ctor);
                        ctx.set_new_target(old_new_target);
                        return result;
                    } catch (...) {
                        super_call_depth--;
                        ctx.set_in_constructor_call(was_in_ctor);
                        ctx.set_new_target(old_new_target);
                        throw;
                    }
                }
            }
            
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            
            return func->call(ctx, arg_values);
        }
    }
    
    ctx.throw_type_error(callee_->to_string() + " is not a function");
    return Value();
}

std::string CallExpression::to_string() const {
    std::ostringstream oss;
    oss << callee_->to_string() << "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << arguments_[i]->to_string();
    }
    oss << ")";
    return oss.str();
}

std::unique_ptr<ASTNode> CallExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    auto cloned = std::make_unique<CallExpression>(callee_->clone(), std::move(cloned_args), start_, end_, is_optional_);
    cloned->set_tagged_template(is_tagged_template_);
    return cloned;
}

Value CallExpression::handle_array_method_call(Object* array, const std::string& method_name, Context& ctx) {
    if (method_name == "push") {
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->push(arg_value);
        }
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "pop") {
        if (array->get_length() > 0) {
            return array->pop();
        } else {
            return Value();
        }
        
    } else if (method_name == "shift") {
        if (array->get_length() > 0) {
            return array->shift();
        } else {
            return Value();
        }
        
    } else if (method_name == "unshift") {
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->unshift(arg_value);
        }
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "join") {
        std::string separator = ",";
        if (arguments_.size() > 0) {
            Value sep_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            separator = sep_value.to_string();
        }
        
        std::ostringstream result;
        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length; ++i) {
            if (i > 0) result << separator;
            Value element = array->get_element(i);
            if (!element.is_undefined() && !element.is_null()) {
                result << element.to_string();
            }
        }
        return Value(result.str());
        
    } else if (method_name == "indexOf") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                Value element = array->get_element(i);
                if (element.strict_equals(search_value)) {
                    return Value(static_cast<double>(i));
                }
            }
        }
        return Value(-1.0);
        
    } else if (method_name == "map") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                auto result_array = ObjectFactory::create_array(0);
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value mapped_value = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    result_array->set_element(i, mapped_value);
                }
                std::string array_data = "ARRAY:[";
                uint32_t result_length = result_array->get_length();
                for (uint32_t i = 0; i < result_length; i++) {
                    if (i > 0) array_data += ",";
                    Value element = result_array->get_element(i);
                    array_data += element.to_string();
                }
                array_data += "]";
                return Value(array_data);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.map requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "filter") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                auto result_array = ObjectFactory::create_array(0);
                uint32_t result_index = 0;
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value test_result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (test_result.to_boolean()) {
                        result_array->set_element(result_index++, element);
                    }
                }
                std::string array_data = "ARRAY:[";
                uint32_t result_length = result_array->get_length();
                for (uint32_t i = 0; i < result_length; i++) {
                    if (i > 0) array_data += ",";
                    Value element = result_array->get_element(i);
                    array_data += element.to_string();
                }
                array_data += "]";
                return Value(array_data);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.filter requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "reduce") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                if (length == 0 && arguments_.size() < 2) {
                    ctx.throw_exception(Value(std::string("Reduce of empty array with no initial value")));
                    return Value();
                }
                
                Value accumulator;
                uint32_t start_index = 0;
                
                if (arguments_.size() >= 2) {
                    accumulator = arguments_[1]->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                } else {
                    accumulator = array->get_element(0);
                    start_index = 1;
                }
                
                for (uint32_t i = start_index; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(array)};
                    
                    accumulator = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return accumulator;
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.reduce requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "forEach") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return Value();
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.forEach requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "slice") {
        uint32_t length = array->get_length();
        int32_t start = 0;
        int32_t end = static_cast<int32_t>(length);
        
        if (arguments_.size() > 0) {
            Value start_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            start = static_cast<int32_t>(start_val.to_number());
            if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
            if (start >= static_cast<int32_t>(length)) start = length;
        }
        
        if (arguments_.size() > 1) {
            Value end_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            end = static_cast<int32_t>(end_val.to_number());
            if (end < 0) end = std::max(0, static_cast<int32_t>(length) + end);
            if (end > static_cast<int32_t>(length)) end = length;
        }
        
        auto result_array = ObjectFactory::create_array(0);
        uint32_t result_index = 0;
        
        for (int32_t i = start; i < end; ++i) {
            Value element = array->get_element(static_cast<uint32_t>(i));
            result_array->set_element(result_index++, element);
        }
        
        std::string array_data = "ARRAY:[";
        uint32_t result_length = result_array->get_length();
        for (uint32_t i = 0; i < result_length; i++) {
            if (i > 0) array_data += ",";
            Value element = result_array->get_element(i);
            array_data += element.to_string();
        }
        array_data += "]";
        return Value(array_data);
        
    } else if (method_name == "concat") {
        auto result_array = ObjectFactory::create_array(0);
        uint32_t result_index = 0;

        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length; ++i) {
            result_array->set_element(result_index++, array->get_element(i));
        }

        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (arg_value.is_object() && arg_value.as_object()->is_array()) {
                Object* arg_array = arg_value.as_object();
                uint32_t arg_length = arg_array->get_length();
                for (uint32_t i = 0; i < arg_length; ++i) {
                    result_array->set_element(result_index++, arg_array->get_element(i));
                }
            } else {
                result_array->set_element(result_index++, arg_value);
            }
        }

        result_array->set_length(result_index);
        return Value(result_array.release());
        
    } else if (method_name == "lastIndexOf") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t length = array->get_length();
            if (length == 0) return Value(-1.0);
            
            int32_t start_pos = static_cast<int32_t>(length) - 1;
            
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                start_pos = static_cast<int32_t>(start_val.to_number());
                if (start_pos < 0) {
                    start_pos = static_cast<int32_t>(length) + start_pos;
                    if (start_pos < 0) return Value(-1.0);
                }
                if (start_pos >= static_cast<int32_t>(length)) {
                    start_pos = static_cast<int32_t>(length) - 1;
                }
            }
            
            for (int32_t i = start_pos; i >= 0; --i) {
                Value element = array->get_element(static_cast<uint32_t>(i));
                if (element.strict_equals(search_value)) {
                    return Value(static_cast<double>(i));
                }
            }
        }
        return Value(-1.0);
        
    } else if (method_name == "reduceRight") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                if (length == 0 && arguments_.size() < 2) {
                    ctx.throw_exception(Value(std::string("ReduceRight of empty array with no initial value")));
                    return Value();
                }
                
                Value accumulator;
                int32_t start_index;
                
                if (arguments_.size() > 1) {
                    accumulator = arguments_[1]->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    start_index = static_cast<int32_t>(length) - 1;
                } else {
                    if (length == 0) {
                        ctx.throw_exception(Value(std::string("ReduceRight of empty array with no initial value")));
                        return Value();
                    }
                    accumulator = array->get_element(length - 1);
                    start_index = static_cast<int32_t>(length) - 2;
                }
                
                for (int32_t i = start_index; i >= 0; --i) {
                    Value element = array->get_element(static_cast<uint32_t>(i));
                    std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(array)};
                    
                    accumulator = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                }
                
                return accumulator;
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.reduceRight requires a callback function")));
            return Value();
        }
        
        
    } else if (method_name == "splice") {
        uint32_t length = array->get_length();

        if (arguments_.size() == 0) {
            // No arguments: return empty array, don't modify
            auto result_array = ObjectFactory::create_array(0);
            return Value(result_array.release());
        }

        int32_t start = 0;
        uint32_t delete_count = 0;

        Value start_val = arguments_[0]->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        start = static_cast<int32_t>(start_val.to_number());
        if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
        if (start >= static_cast<int32_t>(length)) start = length;

        if (arguments_.size() > 1) {
            Value delete_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            delete_count = std::max(0, static_cast<int32_t>(delete_val.to_number()));
            delete_count = std::min(delete_count, length - static_cast<uint32_t>(start));
        } else {
            // Only start provided: delete to end
            delete_count = length - static_cast<uint32_t>(start);
        }
        
        auto result_array = ObjectFactory::create_array(0);
        for (uint32_t i = 0; i < delete_count; ++i) {
            result_array->set_element(i, array->get_element(static_cast<uint32_t>(start) + i));
        }
        
        for (uint32_t i = static_cast<uint32_t>(start) + delete_count; i < length; ++i) {
            array->set_element(static_cast<uint32_t>(start) + i - delete_count, array->get_element(i));
        }
        
        uint32_t new_length = length - delete_count;
        
        for (size_t i = 2; i < arguments_.size(); ++i) {
            Value new_val = arguments_[i]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            for (uint32_t j = new_length; j > static_cast<uint32_t>(start) + (i - 2); --j) {
                array->set_element(j, array->get_element(j - 1));
            }
            array->set_element(static_cast<uint32_t>(start) + (i - 2), new_val);
            new_length++;
        }
        
        array->set_property("length", Value(static_cast<double>(new_length)));
        
        std::string array_data = "ARRAY:[";
        uint32_t result_length = result_array->get_length();
        for (uint32_t i = 0; i < result_length; i++) {
            if (i > 0) array_data += ",";
            Value element = result_array->get_element(i);
            array_data += element.to_string();
        }
        array_data += "]";
        return Value(array_data);
        
    } else if (method_name == "reverse") {
        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length / 2; ++i) {
            Value temp = array->get_element(i);
            array->set_element(i, array->get_element(length - 1 - i));
            array->set_element(length - 1 - i, temp);
        }
        return Value(array);
        
    } else if (method_name == "sort") {
        uint32_t length = array->get_length();
        if (length <= 1) return Value(array);
        
        Function* compareFn = nullptr;
        if (arguments_.size() > 0) {
            Value compare_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
            if (compare_val.is_function()) {
                compareFn = compare_val.as_function();
            } else {
            }
        } else {
        }
        
        for (uint32_t i = 0; i < length - 1; ++i) {
            for (uint32_t j = 0; j < length - i - 1; ++j) {
                Value a = array->get_element(j);
                Value b = array->get_element(j + 1);
                
                bool should_swap = false;
                if (compareFn) {
                    std::vector<Value> args = {a, b};
                    Value result = compareFn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    should_swap = result.to_number() > 0;
                } else {
                    should_swap = a.to_string() > b.to_string();
                }
                
                if (should_swap) {
                    array->set_element(j, b);
                    array->set_element(j + 1, a);
                }
            }
        }
        return Value(array);
        
    } else if (method_name == "find") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return element;
                    }
                }
                return Value();
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.find requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "findIndex") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return Value(static_cast<double>(i));
                    }
                }
                return Value(-1.0);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.findIndex requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "some") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (result.to_boolean()) {
                        return Value(true);
                    }
                }
                return Value(false);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.some requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "every") {
        if (arguments_.size() > 0) {
            Value callback = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (callback.is_function()) {
                Function* callback_fn = callback.as_function();
                uint32_t length = array->get_length();
                
                for (uint32_t i = 0; i < length; ++i) {
                    Value element = array->get_element(i);
                    std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(array)};
                    
                    Value result = callback_fn->call(ctx, args);
                    if (ctx.has_exception()) return Value();
                    
                    if (!result.to_boolean()) {
                        return Value(false);
                    }
                }
                return Value(true);
            } else {
                ctx.throw_exception(Value(std::string("Callback is not a function")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("Array.every requires a callback function")));
            return Value();
        }
        
    } else if (method_name == "includes") {
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            int64_t from_index = 0;
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                if (start_val.is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }

                from_index = static_cast<int64_t>(start_val.to_number());
            }

            uint32_t length = array->get_length();

            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }

            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; ++i) {
                Value element = array->get_element(i);

                if (search_value.is_number() && element.is_number()) {
                    double search_num = search_value.to_number();
                    double element_num = element.to_number();

                    if (std::isnan(search_num) && std::isnan(element_num)) {
                        return Value(true);
                    }

                    if (search_num == element_num) {
                        return Value(true);
                    }
                } else if (element.strict_equals(search_value)) {
                    return Value(true);
                }
            }
        }
        return Value(false);
        
    } else {
        return Value();
    }
}

Value CallExpression::handle_string_method_call(const std::string& str, const std::string& method_name, Context& ctx) {
    if (method_name == "charAt") {
        int index = 0;
        if (arguments_.size() > 0) {
            Value index_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            index = static_cast<int>(index_val.to_number());
        }
        
        if (index < 0 || index >= static_cast<int>(str.length())) {
            return Value(std::string(""));
        }
        
        return Value(std::string(1, str[index]));
        
    } else if (method_name == "substring") {
        int start = 0;
        int end = static_cast<int>(str.length());
        
        if (arguments_.size() > 0) {
            Value start_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            start = static_cast<int>(start_val.to_number());
            if (start < 0) start = 0;
            if (start > static_cast<int>(str.length())) start = str.length();
        }
        
        if (arguments_.size() > 1) {
            Value end_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            end = static_cast<int>(end_val.to_number());
            if (end < 0) end = 0;
            if (end > static_cast<int>(str.length())) end = str.length();
        }
        
        if (start > end) {
            std::swap(start, end);
        }
        
        return Value(str.substr(start, end - start));
        
    } else if (method_name == "indexOf") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string search_str = search_val.to_string();
            
            int start_pos = 0;
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                start_pos = static_cast<int>(start_val.to_number());
                if (start_pos < 0) start_pos = 0;
                if (start_pos >= static_cast<int>(str.length())) return Value(-1.0);
            }
            
            size_t pos = str.find(search_str, start_pos);
            if (pos == std::string::npos) {
                return Value(-1.0);
            }
            return Value(static_cast<double>(pos));
        }
        return Value(-1.0);
        
    } else if (method_name == "lastIndexOf") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string search_str = search_val.to_string();
            
            size_t start_pos = str.length();
            if (arguments_.size() > 1) {
                Value start_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                int start_int = static_cast<int>(start_val.to_number());
                if (start_int < 0) return Value(-1.0);
                start_pos = std::min(static_cast<size_t>(start_int), str.length());
            }
            
            size_t pos = str.rfind(search_str, start_pos);
            if (pos == std::string::npos) {
                return Value(-1.0);
            }
            return Value(static_cast<double>(pos));
        }
        return Value(-1.0);
        
    } else if (method_name == "substr") {
        int size = static_cast<int>(str.length());

        int start = 0;
        if (arguments_.size() > 0) {
            Value start_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            double start_num = start_val.to_number();

            // ToIntegerOrInfinity
            if (std::isnan(start_num)) {
                start = 0;
            } else if (std::isinf(start_num)) {
                start = (start_num < 0) ? 0 : size;
            } else {
                start = static_cast<int>(std::trunc(start_num));
            }
        }

        if (start < 0) {
            start = std::max(0, size + start);
        }
        start = std::min(start, size);

        int length;
        if (arguments_.size() > 1) {
            Value length_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            double length_num = length_val.to_number();

            // ToIntegerOrInfinity
            if (std::isnan(length_num)) {
                length = 0;
            } else if (std::isinf(length_num)) {
                length = (length_num < 0) ? 0 : size;
            } else {
                length = static_cast<int>(std::trunc(length_num));
            }
        } else {
            length = size;
        }

        length = std::min(std::max(length, 0), size);

        int end = std::min(start + length, size);

        if (end <= start) {
            return Value(std::string(""));
        }

        return Value(str.substr(start, end - start));
        
    } else if (method_name == "slice") {
        int start = 0;
        int end = static_cast<int>(str.length());
        
        if (arguments_.size() > 0) {
            Value start_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            start = static_cast<int>(start_val.to_number());
            
            if (start < 0) {
                start = std::max(0, static_cast<int>(str.length()) + start);
            }
            if (start >= static_cast<int>(str.length())) {
                return Value(std::string(""));
            }
        }
        
        if (arguments_.size() > 1) {
            Value end_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            end = static_cast<int>(end_val.to_number());
            
            if (end < 0) {
                end = std::max(0, static_cast<int>(str.length()) + end);
            }
            if (end > static_cast<int>(str.length())) {
                end = str.length();
            }
        }
        
        if (start >= end) {
            return Value(std::string(""));
        }
        
        return Value(str.substr(start, end - start));
        
    } else if (method_name == "split") {
        auto result_array = ObjectFactory::create_array(0);

        if (arguments_.size() == 0) {
            result_array->set_element(0, Value(str));
            return Value(result_array.release());
        }

        Value separator_val = arguments_[0]->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // ES1: If separator is undefined, return array with entire string
        if (separator_val.is_undefined()) {
            result_array->set_element(0, Value(str));
            return Value(result_array.release());
        }

        std::string separator = separator_val.to_string();
        
        if (separator.empty()) {
            for (size_t i = 0; i < str.length(); ++i) {
                result_array->set_element(i, Value(std::string(1, str[i])));
            }
        } else {
            size_t start = 0;
            size_t end = 0;
            uint32_t index = 0;
            
            while ((end = str.find(separator, start)) != std::string::npos) {
                result_array->set_element(index++, Value(str.substr(start, end - start)));
                start = end + separator.length();
            }
            result_array->set_element(index, Value(str.substr(start)));
        }
        
        std::string array_data = "ARRAY:[";
        uint32_t result_length = result_array->get_length();
        for (uint32_t i = 0; i < result_length; i++) {
            if (i > 0) array_data += ",";
            Value element = result_array->get_element(i);
            array_data += element.to_string();
        }
        array_data += "]";
        return Value(array_data);
        
    } else if (method_name == "replace") {
        if (arguments_.size() >= 2) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string search_str = search_val.to_string();
            
            Value replace_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string replace_str = replace_val.to_string();
            
            std::string result = str;
            size_t pos = result.find(search_str);
            if (pos != std::string::npos) {
                result.replace(pos, search_str.length(), replace_str);
            }
            return Value(result);
        }
        return Value(str);
        
    } else if (method_name == "toLowerCase") {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return Value(result);
        
    } else if (method_name == "toUpperCase") {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return Value(result);
        
    } else if (method_name == "trim") {
        std::string result = str;
        result.erase(result.begin(), std::find_if(result.begin(), result.end(), [](int ch) {
            return !std::isspace(ch);
        }));
        result.erase(std::find_if(result.rbegin(), result.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), result.end());
        return Value(result);
        
    } else if (method_name == "length") {
        return Value(static_cast<double>(str.length()));
        
    } else if (method_name == "repeat") {
        if (arguments_.size() > 0) {
            Value count_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            int count = static_cast<int>(count_val.to_number());
            if (count < 0) {
                ctx.throw_range_error("Invalid count value");
                return Value();
            }
            if (count == 0) {
                return Value(std::string(""));
            }

            std::string result;
            result.reserve(str.length() * count);
            for (int i = 0; i < count; i++) {
                result += str;
            }
            return Value(result);
        }
        return Value(std::string(""));
        
    } else if (method_name == "includes") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            std::string search_str = search_val.to_string();
            bool found = str.find(search_str) != std::string::npos;
            return Value(found);
        }
        return Value(false);
        
    } else if (method_name == "indexOf") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            std::string search_str = search_val.to_string();
            size_t pos = str.find(search_str);
            return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
        }
        return Value(-1.0);
        
    } else if (method_name == "charAt") {
        if (arguments_.size() > 0) {
            Value index_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            int index = static_cast<int>(index_val.to_number());
            if (index >= 0 && index < static_cast<int>(str.length())) {
                return Value(std::string(1, str[index]));
            }
        }
        return Value(std::string(""));
        
    } else if (method_name == "charCodeAt") {
        if (arguments_.size() > 0) {
            Value index_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            int index = static_cast<int>(index_val.to_number());
            if (index >= 0 && index < static_cast<int>(str.length())) {
                return Value(static_cast<double>(static_cast<unsigned char>(str[index])));
            }
        }
        return Value(std::numeric_limits<double>::quiet_NaN());
        
    } else if (method_name == "padStart") {
        if (arguments_.size() > 0) {
            Value length_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t target_length = static_cast<uint32_t>(length_val.to_number());
            std::string pad_string = " ";
            
            if (arguments_.size() > 1) {
                Value pad_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                pad_string = pad_val.to_string();
            }
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(padding + str);
        }
        return Value(str);
        
    } else if (method_name == "padEnd") {
        if (arguments_.size() > 0) {
            Value length_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t target_length = static_cast<uint32_t>(length_val.to_number());
            std::string pad_string = " ";
            
            if (arguments_.size() > 1) {
                Value pad_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                pad_string = pad_val.to_string();
            }
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(str + padding);
        }
        return Value(str);
        
    } else if (method_name == "replaceAll") {
        if (arguments_.size() > 1) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            Value replace_val = arguments_[1]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            std::string search_str = search_val.to_string();
            std::string replace_str = replace_val.to_string();
            
            if (search_str.empty()) return Value(str);
            
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(search_str, pos)) != std::string::npos) {
                result.replace(pos, search_str.length(), replace_str);
                pos += replace_str.length();
            }
            
            return Value(result);
        }
        return Value(str);
        
    } else if (method_name == "startsWith") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string search_str = search_val.to_string();
            
            size_t start_pos = 0;
            if (arguments_.size() > 1) {
                Value pos_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                start_pos = static_cast<size_t>(std::max(0.0, pos_val.to_number()));
            }
            
            if (start_pos >= str.length()) return Value(false);
            return Value(str.substr(start_pos).find(search_str) == 0);
        }
        return Value(false);
        
    } else if (method_name == "endsWith") {
        if (arguments_.size() > 0) {
            Value search_val = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            std::string search_str = search_val.to_string();
            
            size_t end_pos = str.length();
            if (arguments_.size() > 1) {
                Value pos_val = arguments_[1]->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                end_pos = static_cast<size_t>(std::max(0.0, std::min((double)str.length(), pos_val.to_number())));
            }
            
            if (search_str.length() > end_pos) return Value(false);
            return Value(str.substr(end_pos - search_str.length(), search_str.length()) == search_str);
        }
        return Value(false);

    } else if (method_name == "concat") {
        std::string result = str;
        for (const auto& arg : arguments_) {
            Value arg_val = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            result += arg_val.to_string();
        }
        return Value(result);

    } else {
        // Fallback: Check String.prototype for the method
        Value string_constructor = ctx.get_binding("String");

        // String constructor can be a function (which is also an object)
        Object* string_ctor = nullptr;
        if (string_constructor.is_function()) {
            string_ctor = string_constructor.as_function();
        } else if (string_constructor.is_object()) {
            string_ctor = string_constructor.as_object();
        }

        if (string_ctor && string_ctor->has_property("prototype")) {
            Value prototype_value = string_ctor->get_property("prototype");
            if (prototype_value.is_object()) {
                Object* string_prototype = prototype_value.as_object();
                if (string_prototype && string_prototype->has_property(method_name)) {
                    Value method_value = string_prototype->get_property(method_name);
                    if (method_value.is_function()) {
                        Function* method = method_value.as_function();

                        // Evaluate arguments
                        std::vector<Value> arg_values;
                        for (const auto& arg : arguments_) {
                            Value val = arg->evaluate(ctx);
                            if (ctx.has_exception()) return Value();
                            arg_values.push_back(val);
                        }

                        // Call method with string as 'this'
                        return method->call(ctx, arg_values, Value(str));
                    }
                }
            }
        }

        return Value();
    }
}

Value CallExpression::handle_bigint_method_call(BigInt* bigint, const std::string& method_name, Context& ctx) {
    if (method_name == "toString") {
        return Value(bigint->to_string());
        
    } else {
        std::cout << "Calling BigInt method: " << method_name << "() -> [Method not fully implemented yet]" << std::endl;
        return Value();
    }
}

Value CallExpression::handle_member_expression_call(Context& ctx) {
    MemberExpression* member = static_cast<MemberExpression*>(callee_.get());

    // ES6: super.method() - call parent prototype method with current this
    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(member->get_object())->get_name() == "super") {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            Function* method = method_value.as_function();
            // this should be the current instance, not the parent constructor
            Object* this_obj = ctx.get_this_binding();
            return method->call(ctx, arg_values, Value(static_cast<Object*>(this_obj)));
        } else {
            ctx.throw_exception(Value(std::string("super." +
                (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER
                    ? static_cast<Identifier*>(member->get_property())->get_name()
                    : std::string("method")) + " is not a function")));
            return Value();
        }
    }

    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {

        Identifier* obj = static_cast<Identifier*>(member->get_object());
        Identifier* prop = static_cast<Identifier*>(member->get_property());

        if (obj->get_name() == "console") {
            std::string method_name = prop->get_name();
            
            if (method_name == "log") {
                std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
                if (ctx.has_exception()) return Value();
                
                for (size_t i = 0; i < arg_values.size(); ++i) {
                    if (i > 0) std::cout << " ";
                    try {
                        std::string str_val = arg_values[i].to_string();
                        std::cout << str_val;
                    } catch (...) {
                        std::cout << "[Error: Cannot convert value to string]";
                    }
                }
                std::cout << std::endl;
                std::cout.flush();

                return Value();
            }
        }
    }
    
    
    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
        
        Identifier* obj = static_cast<Identifier*>(member->get_object());
        Identifier* prop = static_cast<Identifier*>(member->get_property());
        
        if (obj->get_name() == "Math") {
            std::string method_name = prop->get_name();

            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();

            if (method_name == "abs") {
                return Math::abs(ctx, arg_values);
            } else if (method_name == "sqrt") {
                return Math::sqrt(ctx, arg_values);
            } else if (method_name == "max") {
                return Math::max(ctx, arg_values);
            } else if (method_name == "min") {
                return Math::min(ctx, arg_values);
            } else if (method_name == "round") {
                return Math::round(ctx, arg_values);
            } else if (method_name == "floor") {
                return Math::floor(ctx, arg_values);
            } else if (method_name == "ceil") {
                return Math::ceil(ctx, arg_values);
            } else if (method_name == "pow") {
                return Math::pow(ctx, arg_values);
            } else if (method_name == "sin") {
                return Math::sin(ctx, arg_values);
            } else if (method_name == "cos") {
                return Math::cos(ctx, arg_values);
            } else if (method_name == "tan") {
                return Math::tan(ctx, arg_values);
            } else if (method_name == "log") {
                return Math::log(ctx, arg_values);
            } else if (method_name == "exp") {
                return Math::exp(ctx, arg_values);
            } else if (method_name == "random") {
                return Math::random(ctx, arg_values);
            }
        }
    }

    Value object_value = member->get_object()->evaluate(ctx);
    if (ctx.has_exception()) {
        return Value();
    }

    if (object_value.is_null() || object_value.is_undefined()) {
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }

    if (object_value.is_string()) {
        std::string str_value = object_value.to_string();
        
        std::string method_name;
        if (member->is_computed()) {
            Value key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            method_name = key_value.to_property_key();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(member->get_property());
                method_name = prop->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid method name")));
                return Value();
            }
        }
        
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:") {
            auto temp_array = ObjectFactory::create_array(0);
            
            size_t start = str_value.find('[');
            size_t end = str_value.find(']');
            if (start != std::string::npos && end != std::string::npos && start < end) {
                std::string content = str_value.substr(start + 1, end - start - 1);
                if (!content.empty()) {
                    size_t pos = 0;
                    uint32_t index = 0;
                    while (pos < content.length()) {
                        size_t comma_pos = content.find(',', pos);
                        if (comma_pos == std::string::npos) comma_pos = content.length();
                        
                        std::string element = content.substr(pos, comma_pos - pos);
                        if (element == "true") {
                            temp_array->set_element(index++, Value(true));
                        } else if (element == "false") {
                            temp_array->set_element(index++, Value(false));
                        } else if (element == "null") {
                            temp_array->set_element(index++, Value());
                        } else {
                            try {
                                double num = std::stod(element);
                                if (element.find('.') == std::string::npos && 
                                    element.find('e') == std::string::npos && 
                                    element.find('E') == std::string::npos) {
                                    temp_array->set_element(index++, Value(num));
                                } else {
                                    temp_array->set_element(index++, Value(num));
                                }
                            } catch (...) {
                                temp_array->set_element(index++, Value(element));
                            }
                        }
                        
                        pos = comma_pos + 1;
                    }
                }
            }
            
            Value result = handle_array_method_call(temp_array.get(), method_name, ctx);
            
            if (method_name == "push" || method_name == "unshift" || method_name == "reverse" || 
                method_name == "sort" || method_name == "splice") {
                std::string new_array_data = "ARRAY:[";
                uint32_t length = temp_array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    if (i > 0) new_array_data += ",";
                    Value element = temp_array->get_element(i);
                    new_array_data += element.to_string();
                }
                new_array_data += "]";
                
                if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* var_id = static_cast<Identifier*>(member->get_object());
                    std::string var_name = var_id->get_name();
                    
                    ctx.set_binding(var_name, Value(new_array_data));
                }
            }
            
            return result;
        }
        
        if (str_value.length() >= 7 && str_value.substr(0, 7) == "OBJECT:") {
            std::string search = method_name + "=";
            size_t start = str_value.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = str_value.find(",", start);
                if (end == std::string::npos) {
                    end = str_value.find("}", start);
                }
                
                if (end != std::string::npos) {
                    std::string method_value = str_value.substr(start, end - start);
                    
                    if (method_value.substr(0, 9) == "FUNCTION:") {
                        std::string func_id = method_value.substr(9);
                        Value func_value = ctx.get_binding(func_id);
                        
                        if (func_value.is_undefined()) {
                            auto it = g_object_function_map.find(func_id);
                            if (it != g_object_function_map.end()) {
                                    func_value = it->second;
                            }
                        }
                        
                        if (func_value.is_function()) {
                            std::vector<Value> arg_values;
                            for (const auto& arg : arguments_) {
                                Value val = arg->evaluate(ctx);
                                if (ctx.has_exception()) return Value();
                                arg_values.push_back(val);
                            }
                            
                            std::string original_object_str = object_value.to_string();
                            
                            std::string obj_var_name;
                            if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                                Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                                obj_var_name = obj_id->get_name();
                                
                            }
                            
                            Function* method = func_value.as_function();
                            Value result = method->call(ctx, arg_values, object_value);
                            if (ctx.has_exception()) {
                            }
                            
                            if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                                Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                                std::string obj_var_name = obj_id->get_name();
                                
                                
                                Value current_obj = ctx.get_binding(obj_var_name);
                                if (!current_obj.is_undefined() && current_obj.to_string() != original_object_str) {
                                }
                            }
                            
                            return result;
                        }
                    }
                }
            }
            
            ctx.throw_exception(Value(std::string("Method not found or not a function")));
            return Value();
        }

        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        if (method_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();

            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        }

        return handle_string_method_call(str_value, method_name, ctx);
        
    } else if (object_value.is_bigint()) {
        BigInt* bigint_value = object_value.as_bigint();
        
        std::string method_name;
        if (member->is_computed()) {
            Value key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            method_name = key_value.to_property_key();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(member->get_property());
                method_name = prop->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid method name")));
                return Value();
            }
        }
        
        return handle_bigint_method_call(bigint_value, method_name, ctx);
        
    } else if (object_value.is_number()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_boolean()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_symbol()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();
        if (method_value.is_function()) {
            return method_value.as_function()->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_type_error(member->to_string() + " is not a function");
            return Value();
        }

    } else if (object_value.is_object() || object_value.is_function()) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();

        std::string method_name;
        if (member->is_computed()) {
            Value key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            method_name = key_value.to_property_key();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(member->get_property());
                method_name = prop->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid method name")));
                return Value();
            }
        }

        Object* private_method_owner = nullptr;
        if (!method_name.empty() && method_name[0] == '#') {
            if (!private_brand_check(ctx, obj, method_name)) {
                ctx.throw_type_error("Cannot read private member " + method_name + " from an object whose class did not declare it");
                return Value();
            }
            std::string qualified = resolve_private_storage_key(method_name, obj);
            if (obj->has_private_slot(qualified)) {
                method_name = qualified;
            } else {
                // Private method: lives on the declaring class's own prototype.
                private_method_owner = resolve_private_accessor_owner(method_name);
            }
        }

        Value method_value = private_method_owner ? private_method_owner->get_property(method_name) : obj->get_property(method_name);
        if (ctx.has_exception()) return Value();

        std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
        if (ctx.has_exception()) return Value();

        if (method_value.is_function()) {
            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else if (method_value.is_object() &&
                   method_value.as_object()->get_type() == Object::ObjectType::Proxy) {
            Proxy* proxy = static_cast<Proxy*>(method_value.as_object());
            return proxy->apply_trap(arg_values, object_value);
        } else {
            ctx.throw_type_error(method_name.empty() ? "is not a function" : method_name + " is not a function");
            return Value();
        }
    }

    ctx.throw_exception(Value(std::string("Unsupported method call")));
    return Value();
}


} // namespace Quanta
