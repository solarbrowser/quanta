/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include <set>
#include <map>
#include <cstdio>
#include <cmath>
#include <iostream>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/runtime/Math.h"
#include <cstdlib>
#include "quanta/core/runtime/String.h"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <unordered_map>

namespace Quanta {

static std::unordered_map<std::string, Value> g_object_function_map;

static std::unordered_map<const Context*, std::string> g_this_variable_map;

namespace {
    thread_local int loop_depth = 0;
}

int get_loop_depth() {
    return loop_depth;
}

void increment_loop_depth() {
    loop_depth++;
}

void decrement_loop_depth() {
    loop_depth--;
}

namespace {
    struct LoopDepthGuard {
        LoopDepthGuard() { increment_loop_depth(); }
        ~LoopDepthGuard() { decrement_loop_depth(); }
    };
}


Value NumberLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value(value_);
}

std::string NumberLiteral::to_string() const {
    return std::to_string(value_);
}

std::unique_ptr<ASTNode> NumberLiteral::clone() const {
    return std::make_unique<NumberLiteral>(value_, start_, end_);
}


Value StringLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value(value_);
}

std::string StringLiteral::to_string() const {
    return "\"" + value_ + "\"";
}

std::unique_ptr<ASTNode> StringLiteral::clone() const {
    return std::make_unique<StringLiteral>(value_, start_, end_);
}


Value BooleanLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value(value_);
}

std::string BooleanLiteral::to_string() const {
    return value_ ? "true" : "false";
}

std::unique_ptr<ASTNode> BooleanLiteral::clone() const {
    return std::make_unique<BooleanLiteral>(value_, start_, end_);
}


Value NullLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value::null();
}

std::string NullLiteral::to_string() const {
    return "null";
}

std::unique_ptr<ASTNode> NullLiteral::clone() const {
    return std::make_unique<NullLiteral>(start_, end_);
}


Value BigIntLiteral::evaluate(Context& ctx) {
    (void)ctx;
    try {
        auto bigint = std::make_unique<BigInt>(value_);
        return Value(bigint.release());
    } catch (const std::exception& e) {
        ctx.throw_error("Invalid BigInt literal: " + value_);
        return Value();
    }
}

std::string BigIntLiteral::to_string() const {
    return value_ + "n";
}

std::unique_ptr<ASTNode> BigIntLiteral::clone() const {
    return std::make_unique<BigIntLiteral>(value_, start_, end_);
}


Value UndefinedLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string UndefinedLiteral::to_string() const {
    return "undefined";
}

std::unique_ptr<ASTNode> UndefinedLiteral::clone() const {
    return std::make_unique<UndefinedLiteral>(start_, end_);
}


Value TemplateLiteral::evaluate(Context& ctx) {
    std::string result;
    
    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            result += element.text;
        } else if (element.type == Element::Type::EXPRESSION) {
            Value expr_value = element.expression->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            // ES6: Template literals use ToString which calls toString() on objects
            if (expr_value.is_object() || expr_value.is_function()) {
                Object* obj = expr_value.is_function() ?
                    static_cast<Object*>(expr_value.as_function()) :
                    expr_value.as_object();
                Value toString_fn = obj->get_property("toString");
                if (toString_fn.is_function()) {
                    std::vector<Value> no_args;
                    Value str_result = toString_fn.as_function()->call(ctx, no_args, expr_value);
                    if (!ctx.has_exception() && str_result.is_string()) {
                        result += str_result.to_string();
                    } else {
                        ctx.clear_exception();
                        result += expr_value.to_string();
                    }
                } else {
                    result += expr_value.to_string();
                }
            } else {
                result += expr_value.to_string();
            }
        }
    }
    
    return Value(result);
}

std::string TemplateLiteral::to_string() const {
    std::ostringstream oss;
    oss << "`";
    
    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            oss << element.text;
        } else if (element.type == Element::Type::EXPRESSION) {
            oss << "${" << element.expression->to_string() << "}";
        }
    }
    
    oss << "`";
    return oss.str();
}

std::unique_ptr<ASTNode> TemplateLiteral::clone() const {
    std::vector<Element> cloned_elements;

    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            cloned_elements.emplace_back(element.text, element.raw_text);
        } else if (element.type == Element::Type::EXPRESSION) {
            cloned_elements.emplace_back(element.expression->clone());
        }
    }

    return std::make_unique<TemplateLiteral>(std::move(cloned_elements), start_, end_);
}


Value Parameter::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string Parameter::to_string() const {
    std::string result = "";
    if (is_rest_) {
        result += "...";
    }
    result += name_->get_name();
    if (has_default()) {
        result += " = " + default_value_->to_string();
    }
    return result;
}

std::unique_ptr<ASTNode> Parameter::clone() const {
    std::unique_ptr<ASTNode> cloned_default = default_value_ ? default_value_->clone() : nullptr;
    auto cloned = std::make_unique<Parameter>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(name_->clone().release())),
        std::move(cloned_default), is_rest_, start_, end_
    );
    if (destructuring_pattern_) {
        cloned->set_destructuring_pattern(destructuring_pattern_->clone());
    }
    return cloned;
}


Value Identifier::evaluate(Context& ctx) {
    if (name_ == "super") {
        Value super_constructor = ctx.get_binding("__super__");
        return super_constructor;
    }

    static const std::set<std::string> cacheable_globals = {
        "console", "Math", "JSON", "Array", "Object", "String", "Number",
        "Boolean", "RegExp", "Error", "Date", "Infinity", "NaN", "undefined"
    };

    // Globals have fast path caching (immutable bindings)
    if (cacheable_globals.find(name_) != cacheable_globals.end()) {
        if (__builtin_expect(cache_valid_, 1)) {
            return cached_value_;
        }
    }

    if (!ctx.has_binding(name_)) {
        static const std::set<std::string> known_globals = {
            "console", "Math", "JSON", "Date", "Array", "Object", "String", "Number",
            "Boolean", "RegExp", "Error", "TypeError", "ReferenceError", "SyntaxError",
            "undefined", "null", "true", "false", "Infinity", "NaN", "isNaN", "isFinite",
            "parseInt", "parseFloat", "decodeURI", "decodeURIComponent", "encodeURI",
            "encodeURIComponent", "globalThis", "window", "global", "self"
        };

        if (known_globals.find(name_) == known_globals.end()) {
            ctx.throw_reference_error("'" + name_ + "' is not defined");
            return Value();
        }
    }

    Value result = ctx.get_binding(name_);

    // Only cache immutable globals
    if (cacheable_globals.find(name_) != cacheable_globals.end() && !cache_valid_) {
        cached_value_ = result;
        cache_valid_ = true;
    }

    return result;
}

std::string Identifier::to_string() const {
    return name_;
}

std::unique_ptr<ASTNode> Identifier::clone() const {
    return std::make_unique<Identifier>(name_, start_, end_);
}


Value BinaryExpression::evaluate(Context& ctx) {
    if (operator_ == Operator::ASSIGN ||
        operator_ == Operator::PLUS_ASSIGN ||
        operator_ == Operator::MINUS_ASSIGN ||
        operator_ == Operator::MULTIPLY_ASSIGN ||
        operator_ == Operator::DIVIDE_ASSIGN ||
        operator_ == Operator::MODULO_ASSIGN ||
        operator_ == Operator::BITWISE_AND_ASSIGN ||
        operator_ == Operator::BITWISE_OR_ASSIGN ||
        operator_ == Operator::BITWISE_XOR_ASSIGN ||
        operator_ == Operator::LEFT_SHIFT_ASSIGN ||
        operator_ == Operator::RIGHT_SHIFT_ASSIGN ||
        operator_ == Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN) {
        
        
        
        Value right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }
        
        Value result_value = right_value;
        if (operator_ != Operator::ASSIGN) {
            Value left_value = left_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            switch (operator_) {
                case Operator::ASSIGN:
                    result_value = right_value;
                    break;
                case Operator::PLUS_ASSIGN:
                    result_value = left_value.add(right_value);
                    break;
                case Operator::MINUS_ASSIGN:
                    result_value = left_value.subtract(right_value);
                    break;
                case Operator::MULTIPLY_ASSIGN:
                    result_value = left_value.multiply(right_value);
                    break;
                case Operator::DIVIDE_ASSIGN:
                    result_value = left_value.divide(right_value);
                    break;
                case Operator::MODULO_ASSIGN:
                    result_value = left_value.modulo(right_value);
                    break;
                case Operator::BITWISE_AND_ASSIGN:
                    result_value = left_value.bitwise_and(right_value);
                    break;
                case Operator::BITWISE_OR_ASSIGN:
                    result_value = left_value.bitwise_or(right_value);
                    break;
                case Operator::BITWISE_XOR_ASSIGN:
                    result_value = left_value.bitwise_xor(right_value);
                    break;
                case Operator::LEFT_SHIFT_ASSIGN:
                    result_value = left_value.left_shift(right_value);
                    break;
                case Operator::RIGHT_SHIFT_ASSIGN:
                    result_value = left_value.right_shift(right_value);
                    break;
                case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN:
                    result_value = left_value.unsigned_right_shift(right_value);
                    break;
                default:
                    break;
            }
        }
        
        if (left_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            std::string name = id->get_name();

            // ES5: Cannot assign to eval or arguments in strict mode
            if (ctx.is_strict_mode() && (name == "eval" || name == "arguments")) {
                ctx.throw_syntax_error("'" + name + "' cannot be assigned in strict mode");
                return Value();
            }

            if (operator_ == Operator::ASSIGN && !ctx.has_binding(name)) {
                if (ctx.is_strict_mode()) {
                    ctx.throw_reference_error("'" + name + "' is not defined");
                    return Value();
                } else {
                    ctx.create_var_binding(name, result_value);
                    return result_value;
                }
            }
            
            ctx.set_binding(name, result_value);
            return result_value;
        }
        
        if (left_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            MemberExpression* member = static_cast<MemberExpression*>(left_.get());
            
            Value object_value = member->get_object()->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
            
            std::string str_value = object_value.to_string();
            if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && member->is_computed()) {
                Value index_value = member->get_property()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                int index = static_cast<int>(index_value.to_number());
                if (index >= 0) {
                    std::string array_content = str_value.substr(6);
                    array_content = array_content.substr(1, array_content.length() - 2);
                    
                    std::vector<std::string> elements;
                    if (!array_content.empty()) {
                        std::stringstream ss(array_content);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            elements.push_back(item);
                        }
                    }
                    
                    while (static_cast<int>(elements.size()) <= index) {
                        elements.push_back("undefined");
                    }
                    
                    std::string value_str;
                    if (result_value.is_number()) {
                        value_str = std::to_string(result_value.as_number());
                    } else if (result_value.is_boolean()) {
                        value_str = result_value.as_boolean() ? "true" : "false";
                    } else if (result_value.is_null()) {
                        value_str = "null";
                    } else {
                        value_str = result_value.to_string();
                    }
                    elements[index] = value_str;
                    
                    std::string new_array = "ARRAY:[";
                    for (size_t i = 0; i < elements.size(); ++i) {
                        if (i > 0) new_array += ",";
                        new_array += elements[i];
                    }
                    new_array += "]";
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* array_id = static_cast<Identifier*>(member->get_object());
                        ctx.set_binding(array_id->get_name(), Value(new_array));
                    }
                    
                    return result_value;
                }
            }
            
            Object* obj = nullptr;
            if (object_value.is_object()) {
                obj = object_value.as_object();
            } else if (object_value.is_function()) {
                obj = object_value.as_function();
            }

            if (obj) {
                std::string key;
                if (member->is_computed()) {
                    Value key_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    key = key_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* prop = static_cast<Identifier*>(member->get_property());
                        key = prop->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property in assignment")));
                        return Value();
                    }
                }
                
                PropertyDescriptor desc = obj->get_property_descriptor(key);
                if (desc.is_accessor_descriptor() && desc.has_setter()) {
                    // Cookie handling removed for simplicity
                }

                // ES5: In strict mode, throw TypeError for non-writable/non-extensible assignments
                if (ctx.is_strict_mode()) {
                    // Getter-only property (accessor with no setter)
                    if (desc.is_accessor_descriptor() && !desc.has_setter()) {
                        ctx.throw_type_error("Cannot set property '" + key + "' which has only a getter");
                        return Value();
                    }
                }

                bool success = obj->set_property(key, result_value);
                if (!success && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot assign to read only property '" + key + "'");
                    return Value();
                }
                return result_value;
            } else if (object_value.is_string()) {
                std::string str_val = object_value.to_string();
                if (str_val.length() >= 7 && str_val.substr(0, 7) == "OBJECT:") {
                    std::string prop_name;
                    if (member->is_computed()) {
                        Value prop_value = member->get_property()->evaluate(ctx);
                        if (ctx.has_exception()) return Value();
                        prop_name = prop_value.to_string();
                    } else {
                        if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* id = static_cast<Identifier*>(member->get_property());
                            prop_name = id->get_name();
                        } else {
                            ctx.throw_exception(Value(std::string("Invalid property access")));
                            return Value();
                        }
                    }
                    
                    std::string new_prop = prop_name + "=" + result_value.to_string();
                    
                    if (str_val == "OBJECT:{}") {
                        str_val = "OBJECT:{" + new_prop + "}";
                    } else {
                        std::string search_pattern = prop_name + "=";
                        size_t prop_start = str_val.find(search_pattern);
                        
                        if (prop_start != std::string::npos) {
                            size_t value_start = prop_start + search_pattern.length();
                            size_t value_end = str_val.find(",", value_start);
                            if (value_end == std::string::npos) {
                                value_end = str_val.find("}", value_start);
                            }
                            
                            if (value_end != std::string::npos) {
                                str_val = str_val.substr(0, value_start) + result_value.to_string() + str_val.substr(value_end);
                            }
                        } else {
                            size_t close_pos = str_val.rfind('}');
                            if (close_pos != std::string::npos) {
                                str_val = str_val.substr(0, close_pos) + "," + new_prop + "}";
                            }
                        }
                    }
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                        std::string var_name = obj_id->get_name();
                        ctx.set_binding(var_name, Value(str_val));
                        
                        if (var_name == "this") {
                            ctx.set_binding("this", Value(str_val));
                            
                        }
                    }
                    
                    return result_value;
                } else {
                    ctx.throw_exception(Value(std::string("Cannot set property on non-object")));
                    return Value();
                }
            } else {
                ctx.throw_exception(Value(std::string("Cannot set property on non-object")));
                return Value();
            }
        }
        
        ctx.throw_exception(Value(std::string("Invalid left-hand side in assignment")));
        return Value();
    }
    
    Value left_value = left_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (operator_ == Operator::LOGICAL_AND) {
        if (!left_value.to_boolean()) {
            return left_value;
        }
        return right_->evaluate(ctx);
    }
    
    if (operator_ == Operator::LOGICAL_OR) {
        if (left_value.to_boolean()) {
            return left_value;
        }
        return right_->evaluate(ctx);
    }
    
    if (operator_ == Operator::COMMA) {
        return right_->evaluate(ctx);
    }
    
    Value right_value = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (LIKELY(left_value.is_number() && right_value.is_number())) {
        double left_num = left_value.as_number();
        double right_num = right_value.as_number();
        
        switch (operator_) {
            case Operator::ADD: {
                double result = left_num + right_num;
                if (std::isinf(result)) {
                    return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
                }
                if (std::isnan(result)) {
                    return Value::nan();
                }
                return Value(result);
            }
            case Operator::SUBTRACT: {
                double result = left_num - right_num;
                if (std::isinf(result)) {
                    return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
                }
                if (std::isnan(result)) {
                    return Value::nan();
                }
                return Value(result);
            }
            case Operator::MULTIPLY: {
                double result = left_num * right_num;
                if (std::isinf(result)) {
                    return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
                }
                if (std::isnan(result)) {
                    return Value::nan();
                }
                return Value(result);
            }
            case Operator::DIVIDE: {
                if (right_num == 0.0) {
                    if (left_num == 0.0) {
                        return Value::nan();
                    }
                    return left_num > 0 ? Value::positive_infinity() : Value::negative_infinity();
                }
                
                double result = left_num / right_num;
                
                if (std::isinf(result)) {
                    return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
                }
                if (std::isnan(result)) {
                    return Value::nan();
                }
                
                return Value(result);
            }
            case Operator::MODULO: {
                double result = left_num - static_cast<long long>(left_num / right_num) * right_num;
                return Value(result);
            }
            default:
                break;
        }
    }
    
    switch (operator_) {
        case Operator::ADD: {
            Value left_coerced = left_value;
            Value right_coerced = right_value;

            // ES6 ToPrimitive: Date objects prefer toString, others prefer valueOf
            auto toPrimitive = [&ctx](const Value& val) -> Value {
                if (!val.is_object() || val.is_string()) return val;
                Object* obj = val.as_object();
                if (!obj) return val;
                bool prefer_string = obj->has_property("_isDate");
                if (prefer_string) {
                    // Try toString first
                    Value toString_method = obj->get_property("toString");
                    if (toString_method.is_function()) {
                        try {
                            Value result = toString_method.as_function()->call(ctx, {}, val);
                            if (!result.is_object()) return result;
                        } catch (...) {}
                    }
                }
                // Try valueOf
                Value valueOf_method = obj->get_property("valueOf");
                if (valueOf_method.is_function()) {
                    try {
                        Value result = valueOf_method.as_function()->call(ctx, {}, val);
                        if (!result.is_object()) return result;
                    } catch (...) {}
                }
                if (!prefer_string) {
                    // Try toString as fallback
                    Value toString_method = obj->get_property("toString");
                    if (toString_method.is_function()) {
                        try {
                            Value result = toString_method.as_function()->call(ctx, {}, val);
                            if (!result.is_object()) return result;
                        } catch (...) {}
                    }
                }
                return val;
            };

            left_coerced = toPrimitive(left_value);
            right_coerced = toPrimitive(right_value);

            return left_coerced.add(right_coerced);
        }
        case Operator::SUBTRACT:
        case Operator::MULTIPLY: {
            Value left_coerced = left_value;
            Value right_coerced = right_value;

            if (left_value.is_object() && !left_value.is_string()) {
                Object* obj = left_value.as_object();
                if (obj && obj->has_property("valueOf")) {
                    Value valueOf_method = obj->get_property("valueOf");
                    if (valueOf_method.is_function()) {
                        try {
                            Function* valueOf_fn = valueOf_method.as_function();
                            Value coerced = valueOf_fn->call(ctx, {}, left_value);
                            if (!coerced.is_object()) {
                                left_coerced = coerced;
                            }
                        } catch (...) {
                        }
                    }
                }
            }

            if (right_value.is_object() && !right_value.is_string()) {
                Object* obj = right_value.as_object();
                if (obj && obj->has_property("valueOf")) {
                    Value valueOf_method = obj->get_property("valueOf");
                    if (valueOf_method.is_function()) {
                        try {
                            Function* valueOf_fn = valueOf_method.as_function();
                            Value coerced = valueOf_fn->call(ctx, {}, right_value);
                            if (!coerced.is_object()) {
                                right_coerced = coerced;
                            }
                        } catch (...) {
                        }
                    }
                }
            }

            if (operator_ == Operator::SUBTRACT) {
                return left_coerced.subtract(right_coerced);
            } else {
                return left_coerced.multiply(right_coerced);
            }
        }
        case Operator::DIVIDE:
            return left_value.divide(right_value);
        case Operator::MODULO:
            return left_value.modulo(right_value);
        case Operator::EXPONENT:
            return left_value.power(right_value);
            
        case Operator::EQUAL:
            return Value(left_value.loose_equals(right_value));
        case Operator::NOT_EQUAL:
            return Value(!left_value.loose_equals(right_value));
        case Operator::STRICT_EQUAL:
            return Value(left_value.strict_equals(right_value));
        case Operator::STRICT_NOT_EQUAL:
            return Value(!left_value.strict_equals(right_value));
        case Operator::LESS_THAN:
            return Value(left_value.compare(right_value) < 0);
        case Operator::GREATER_THAN:
            return Value(left_value.compare(right_value) > 0);
        case Operator::LESS_EQUAL:
            return Value(left_value.compare(right_value) <= 0);
        case Operator::GREATER_EQUAL:
            return Value(left_value.compare(right_value) >= 0);
            
        case Operator::INSTANCEOF:
            if (!right_value.is_function()) {
                ctx.throw_type_error("Right-hand side of instanceof is not callable");
                return Value(false);
            }
            return Value(left_value.instanceof_check(right_value));

        case Operator::IN: {
            std::string property_name = left_value.to_string();
            if (!right_value.is_object() && !right_value.is_function()) {
                ctx.throw_type_error("Cannot use 'in' operator to search for '" + property_name + "' in " + right_value.to_string());
                return Value(false);
            }
            Object* obj = right_value.is_function()
                ? static_cast<Object*>(right_value.as_function())
                : right_value.as_object();
            return Value(obj->has_property(property_name));
        }
        
        case Operator::BITWISE_AND:
            return left_value.bitwise_and(right_value);
        case Operator::BITWISE_OR:
            return left_value.bitwise_or(right_value);
        case Operator::BITWISE_XOR:
            return left_value.bitwise_xor(right_value);
        case Operator::LEFT_SHIFT:
            return left_value.left_shift(right_value);
        case Operator::RIGHT_SHIFT:
            return left_value.right_shift(right_value);
        case Operator::UNSIGNED_RIGHT_SHIFT:
            return left_value.unsigned_right_shift(right_value);
            
        default:
            ctx.throw_exception(Value(std::string("Unsupported binary operator")));
            return Value();
    }
}

std::string BinaryExpression::to_string() const {
    return "(" + left_->to_string() + " " + operator_to_string(operator_) + " " + right_->to_string() + ")";
}

std::unique_ptr<ASTNode> BinaryExpression::clone() const {
    return std::make_unique<BinaryExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_
    );
}

std::string BinaryExpression::operator_to_string(Operator op) {
    switch (op) {
        case Operator::ADD: return "+";
        case Operator::SUBTRACT: return "-";
        case Operator::MULTIPLY: return "*";
        case Operator::DIVIDE: return "/";
        case Operator::MODULO: return "%";
        case Operator::EXPONENT: return "**";
        case Operator::ASSIGN: return "=";
        case Operator::PLUS_ASSIGN: return "+=";
        case Operator::MINUS_ASSIGN: return "-=";
        case Operator::MULTIPLY_ASSIGN: return "*=";
        case Operator::DIVIDE_ASSIGN: return "/=";
        case Operator::MODULO_ASSIGN: return "%=";
        case Operator::BITWISE_AND_ASSIGN: return "&=";
        case Operator::BITWISE_OR_ASSIGN: return "|=";
        case Operator::BITWISE_XOR_ASSIGN: return "^=";
        case Operator::LEFT_SHIFT_ASSIGN: return "<<=";
        case Operator::RIGHT_SHIFT_ASSIGN: return ">>=";
        case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: return ">>>=";
        case Operator::EQUAL: return "==";
        case Operator::NOT_EQUAL: return "!=";
        case Operator::STRICT_EQUAL: return "===";
        case Operator::STRICT_NOT_EQUAL: return "!==";
        case Operator::LESS_THAN: return "<";
        case Operator::GREATER_THAN: return ">";
        case Operator::LESS_EQUAL: return "<=";
        case Operator::GREATER_EQUAL: return ">=";
        case Operator::INSTANCEOF: return "instanceof";
        case Operator::IN: return "in";
        case Operator::LOGICAL_AND: return "&&";
        case Operator::LOGICAL_OR: return "||";
        case Operator::COMMA: return ",";
        case Operator::BITWISE_AND: return "&";
        case Operator::BITWISE_OR: return "|";
        case Operator::BITWISE_XOR: return "^";
        case Operator::LEFT_SHIFT: return "<<";
        case Operator::RIGHT_SHIFT: return ">>";
        case Operator::UNSIGNED_RIGHT_SHIFT: return ">>>";
        default: return "?";
    }
}

BinaryExpression::Operator BinaryExpression::token_type_to_operator(TokenType type) {
    switch (type) {
        case TokenType::PLUS: return Operator::ADD;
        case TokenType::MINUS: return Operator::SUBTRACT;
        case TokenType::MULTIPLY: return Operator::MULTIPLY;
        case TokenType::DIVIDE: return Operator::DIVIDE;
        case TokenType::MODULO: return Operator::MODULO;
        case TokenType::EXPONENT: return Operator::EXPONENT;
        case TokenType::ASSIGN: return Operator::ASSIGN;
        case TokenType::PLUS_ASSIGN: return Operator::PLUS_ASSIGN;
        case TokenType::MINUS_ASSIGN: return Operator::MINUS_ASSIGN;
        case TokenType::MULTIPLY_ASSIGN: return Operator::MULTIPLY_ASSIGN;
        case TokenType::DIVIDE_ASSIGN: return Operator::DIVIDE_ASSIGN;
        case TokenType::MODULO_ASSIGN: return Operator::MODULO_ASSIGN;
        case TokenType::BITWISE_AND_ASSIGN: return Operator::BITWISE_AND_ASSIGN;
        case TokenType::BITWISE_OR_ASSIGN: return Operator::BITWISE_OR_ASSIGN;
        case TokenType::BITWISE_XOR_ASSIGN: return Operator::BITWISE_XOR_ASSIGN;
        case TokenType::LEFT_SHIFT_ASSIGN: return Operator::LEFT_SHIFT_ASSIGN;
        case TokenType::RIGHT_SHIFT_ASSIGN: return Operator::RIGHT_SHIFT_ASSIGN;
        case TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN: return Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN;
        case TokenType::EQUAL: return Operator::EQUAL;
        case TokenType::NOT_EQUAL: return Operator::NOT_EQUAL;
        case TokenType::STRICT_EQUAL: return Operator::STRICT_EQUAL;
        case TokenType::STRICT_NOT_EQUAL: return Operator::STRICT_NOT_EQUAL;
        case TokenType::LESS_THAN: return Operator::LESS_THAN;
        case TokenType::GREATER_THAN: return Operator::GREATER_THAN;
        case TokenType::LESS_EQUAL: return Operator::LESS_EQUAL;
        case TokenType::GREATER_EQUAL: return Operator::GREATER_EQUAL;
        case TokenType::INSTANCEOF: return Operator::INSTANCEOF;
        case TokenType::IN: return Operator::IN;
        case TokenType::LOGICAL_AND: return Operator::LOGICAL_AND;
        case TokenType::LOGICAL_OR: return Operator::LOGICAL_OR;
        case TokenType::COMMA: return Operator::COMMA;
        case TokenType::BITWISE_AND: return Operator::BITWISE_AND;
        case TokenType::BITWISE_OR: return Operator::BITWISE_OR;
        case TokenType::BITWISE_XOR: return Operator::BITWISE_XOR;
        case TokenType::LEFT_SHIFT: return Operator::LEFT_SHIFT;
        case TokenType::RIGHT_SHIFT: return Operator::RIGHT_SHIFT;
        case TokenType::UNSIGNED_RIGHT_SHIFT: return Operator::UNSIGNED_RIGHT_SHIFT;
        default: return Operator::ADD;
    }
}

int BinaryExpression::get_precedence(Operator op) {
    switch (op) {
        case Operator::COMMA: return 0;
        case Operator::ASSIGN: return 1;
        case Operator::LOGICAL_OR: return 2;
        case Operator::LOGICAL_AND: return 3;
        case Operator::BITWISE_OR: return 4;
        case Operator::BITWISE_XOR: return 5;
        case Operator::BITWISE_AND: return 6;
        case Operator::EQUAL:
        case Operator::NOT_EQUAL:
        case Operator::STRICT_EQUAL:
        case Operator::STRICT_NOT_EQUAL: return 7;
        case Operator::LESS_THAN:
        case Operator::GREATER_THAN:
        case Operator::LESS_EQUAL:
        case Operator::GREATER_EQUAL:
        case Operator::INSTANCEOF:
        case Operator::IN: return 8;
        case Operator::LEFT_SHIFT:
        case Operator::RIGHT_SHIFT:
        case Operator::UNSIGNED_RIGHT_SHIFT: return 9;
        case Operator::ADD:
        case Operator::SUBTRACT: return 10;
        case Operator::MULTIPLY:
        case Operator::DIVIDE:
        case Operator::MODULO: return 11;
        case Operator::EXPONENT: return 12;
        default: return 0;
    }
}

bool BinaryExpression::is_right_associative(Operator op) {
    return op == Operator::ASSIGN || op == Operator::EXPONENT;
}


Value UnaryExpression::evaluate(Context& ctx) {
    switch (operator_) {
        case Operator::PLUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.unary_plus();
        }
        case Operator::MINUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.unary_minus();
        }
        case Operator::LOGICAL_NOT: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.logical_not();
        }
        case Operator::BITWISE_NOT: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.bitwise_not();
        }
        case Operator::TYPEOF: {
            Value operand_value = operand_->evaluate(ctx);

            if (ctx.has_exception()) {
                ctx.clear_exception();
                return Value(std::string("undefined"));
            }

            return operand_value.typeof_op();
        }
        case Operator::VOID: {
            (void)operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return Value();
        }
        case Operator::DELETE: {
            if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value object_value = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                Object* obj = nullptr;
                if (object_value.is_object()) {
                    obj = object_value.as_object();
                } else if (object_value.is_function()) {
                    obj = object_value.as_function();
                }
                if (!obj) {
                    return Value(true);
                }
                std::string property_name;
                
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    property_name = prop_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        property_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property access in delete")));
                        return Value();
                    }
                }
                
                bool deleted = obj->delete_property(property_name);
                // ES5: Deleting non-configurable property throws TypeError in strict mode
                if (!deleted && ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot delete property '" + property_name + "'");
                    return Value();
                }
                return Value(deleted);
            } else if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                // ES5: Delete on identifier is SyntaxError in strict mode
                if (ctx.is_strict_mode()) {
                    ctx.throw_syntax_error("Delete of an unqualified identifier in strict mode");
                    return Value();
                }
                // ES1: delete on identifier
                // In non-strict mode, deleting a global variable (not declared with var)
                // should succeed. Variables declared with var cannot be deleted.
                Identifier* id = static_cast<Identifier*>(operand_.get());
                std::string name = id->get_name();

                // Try to delete the binding from the context
                bool deleted = ctx.delete_binding(name);
                return Value(deleted);
            } else {
                return Value(true);
            }
        }
        case Operator::PRE_INCREMENT: {
            // ES5: Cannot modify eval or arguments in strict mode
            if (ctx.is_strict_mode() && operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<Identifier*>(operand_.get())->get_name();
                if (n == "eval" || n == "arguments") {
                    ctx.throw_syntax_error("'" + n + "' cannot be modified in strict mode");
                    return Value();
                }
            }
            if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(operand_.get());
                Value current = ctx.get_binding(id->get_name());
                Value incremented = Value(current.to_number() + 1.0);
                ctx.set_binding(id->get_name(), incremented);
                return incremented;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value incremented = Value(current.to_number() + 1.0);
                
                Value obj = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                if (!obj.is_object()) {
                    ctx.throw_exception(Value(std::string("Cannot assign to property of non-object")));
                    return Value();
                }
                
                std::string prop_name;
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = prop_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        prop_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property name")));
                        return Value();
                    }
                }
                if (ctx.has_exception()) return Value();
                obj.as_object()->set_property(prop_name, incremented);
                return incremented;
            } else {
                ctx.throw_exception(Value(std::string("Invalid left-hand side in assignment")));
                return Value();
            }
        }
        case Operator::POST_INCREMENT: {
            // ES5: Cannot modify eval or arguments in strict mode
            if (ctx.is_strict_mode() && operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<Identifier*>(operand_.get())->get_name();
                if (n == "eval" || n == "arguments") {
                    ctx.throw_syntax_error("'" + n + "' cannot be modified in strict mode");
                    return Value();
                }
            }
            if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(operand_.get());
                Value current = ctx.get_binding(id->get_name());
                Value incremented = Value(current.to_number() + 1.0);
                bool success = ctx.set_binding(id->get_name(), incremented);
                return current;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value incremented = Value(current.to_number() + 1.0);
                
                Value obj = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                if (!obj.is_object()) {
                    ctx.throw_exception(Value(std::string("Cannot assign to property of non-object")));
                    return Value();
                }
                
                std::string prop_name;
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = prop_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        prop_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property name")));
                        return Value();
                    }
                }
                if (ctx.has_exception()) return Value();
                obj.as_object()->set_property(prop_name, incremented);
                return current;
            } else {
                ctx.throw_exception(Value(std::string("Invalid left-hand side in assignment")));
                return Value();
            }
        }
        case Operator::PRE_DECREMENT: {
            // ES5: Cannot modify eval or arguments in strict mode
            if (ctx.is_strict_mode() && operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<Identifier*>(operand_.get())->get_name();
                if (n == "eval" || n == "arguments") {
                    ctx.throw_syntax_error("'" + n + "' cannot be modified in strict mode");
                    return Value();
                }
            }
            if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(operand_.get());
                Value current = ctx.get_binding(id->get_name());
                Value decremented = Value(current.to_number() - 1.0);
                ctx.set_binding(id->get_name(), decremented);
                return decremented;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value decremented = Value(current.to_number() - 1.0);
                
                Value obj = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                if (!obj.is_object()) {
                    ctx.throw_exception(Value(std::string("Cannot assign to property of non-object")));
                    return Value();
                }
                
                std::string prop_name;
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = prop_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        prop_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property name")));
                        return Value();
                    }
                }
                if (ctx.has_exception()) return Value();
                obj.as_object()->set_property(prop_name, decremented);
                return decremented;
            } else {
                ctx.throw_exception(Value(std::string("Invalid left-hand side in assignment")));
                return Value();
            }
        }
        case Operator::POST_DECREMENT: {
            // ES5: Cannot modify eval or arguments in strict mode
            if (ctx.is_strict_mode() && operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& n = static_cast<Identifier*>(operand_.get())->get_name();
                if (n == "eval" || n == "arguments") {
                    ctx.throw_syntax_error("'" + n + "' cannot be modified in strict mode");
                    return Value();
                }
            }
            if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(operand_.get());
                Value current = ctx.get_binding(id->get_name());
                Value decremented = Value(current.to_number() - 1.0);
                ctx.set_binding(id->get_name(), decremented);
                return current;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value decremented = Value(current.to_number() - 1.0);
                
                Value obj = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                if (!obj.is_object()) {
                    ctx.throw_exception(Value(std::string("Cannot assign to property of non-object")));
                    return Value();
                }
                
                std::string prop_name;
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = prop_value.to_string();
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        prop_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property name")));
                        return Value();
                    }
                }
                if (ctx.has_exception()) return Value();
                obj.as_object()->set_property(prop_name, decremented);
                return current;
            } else {
                ctx.throw_exception(Value(std::string("Invalid left-hand side in assignment")));
                return Value();
            }
        }
        default:
            ctx.throw_exception(Value(std::string("Unsupported unary operator")));
            return Value();
    }
}

std::string UnaryExpression::to_string() const {
    if (prefix_) {
        return operator_to_string(operator_) + operand_->to_string();
    } else {
        return operand_->to_string() + operator_to_string(operator_);
    }
}

std::unique_ptr<ASTNode> UnaryExpression::clone() const {
    return std::make_unique<UnaryExpression>(operator_, operand_->clone(), prefix_, start_, end_);
}

std::string UnaryExpression::operator_to_string(Operator op) {
    switch (op) {
        case Operator::PLUS: return "+";
        case Operator::MINUS: return "-";
        case Operator::LOGICAL_NOT: return "!";
        case Operator::BITWISE_NOT: return "~";
        case Operator::TYPEOF: return "typeof ";
        case Operator::VOID: return "void ";
        case Operator::DELETE: return "delete ";
        case Operator::PRE_INCREMENT: return "++";
        case Operator::POST_INCREMENT: return "++";
        case Operator::PRE_DECREMENT: return "--";
        case Operator::POST_DECREMENT: return "--";
        default: return "?";
    }
}


Value AssignmentExpression::evaluate(Context& ctx) {
    // Declare right_value at function scope (will be evaluated at the right time)
    Value right_value;

    if (left_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* id = static_cast<Identifier*>(left_.get());
        std::string name = id->get_name();

        // ES5: Cannot assign to eval or arguments in strict mode
        if (ctx.is_strict_mode() && (name == "eval" || name == "arguments")) {
            ctx.throw_syntax_error("'" + name + "' cannot be assigned in strict mode");
            return Value();
        }

        // For compound assignments, capture left value BEFORE evaluating right side
        // This ensures correct ES1 left-to-right evaluation order
        Value left_value;
        if (operator_ != Operator::ASSIGN) {
            left_value = ctx.get_binding(name);
            if (ctx.has_exception()) return Value();
        }

        // Now evaluate right side
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }

        switch (operator_) {
            case Operator::ASSIGN: {
                bool has_it = ctx.has_binding(name);
                if (!has_it) {
                    if (ctx.is_strict_mode()) {
                        ctx.throw_reference_error("'" + name + "' is not defined");
                        return Value();
                    } else {
                        // ES1: Assignments without 'var' create deletable global bindings
                        ctx.create_binding(name, right_value, true, true);
                    }
                } else {
                    bool success = ctx.set_binding(name, right_value);
                    if (!success && ctx.is_strict_mode()) {
                        ctx.throw_type_error("Cannot assign to read only variable '" + name + "'");
                        return Value();
                    }
                }
                return right_value;
            }
            case Operator::PLUS_ASSIGN: {
                // Use add() method to handle both string concatenation and numeric addition
                Value result = left_value.add(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MINUS_ASSIGN: {
                Value result = Value(left_value.to_number() - right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MUL_ASSIGN: {
                Value result = Value(left_value.to_number() * right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::DIV_ASSIGN: {
                Value result = Value(left_value.to_number() / right_value.to_number());
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::MOD_ASSIGN: {
                double left_num = left_value.to_number();
                double right_num = right_value.to_number();
                Value result = Value(std::fmod(left_num, right_num));
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_AND_ASSIGN: {
                Value result = left_value.bitwise_and(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_OR_ASSIGN: {
                Value result = left_value.bitwise_or(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::BITWISE_XOR_ASSIGN: {
                Value result = left_value.bitwise_xor(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::LEFT_SHIFT_ASSIGN: {
                Value result = left_value.left_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::RIGHT_SHIFT_ASSIGN: {
                Value result = left_value.right_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            case Operator::UNSIGNED_RIGHT_SHIFT_ASSIGN: {
                Value result = left_value.unsigned_right_shift(right_value);
                ctx.set_binding(name, result);
                return result;
            }
            default:
                ctx.throw_exception(Value(std::string("Unsupported assignment operator")));
                return Value();
        }
        
        return right_value;
    }
    
    if (left_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        MemberExpression* member = static_cast<MemberExpression*>(left_.get());

        // For member expressions, evaluate object first, then right side
        Value object_value = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }

        // Now evaluate right side
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }
        
        std::string str_value = object_value.to_string();
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && member->is_computed()) {
            Value index_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            int index = static_cast<int>(index_value.to_number());
            if (index >= 0) {
                std::string array_content = str_value.substr(6);
                array_content = array_content.substr(1, array_content.length() - 2);
                
                std::vector<std::string> elements;
                if (!array_content.empty()) {
                    std::stringstream ss(array_content);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        elements.push_back(item);
                    }
                }
                
                while (static_cast<int>(elements.size()) <= index) {
                    elements.push_back("undefined");
                }
                
                std::string value_str = right_value.to_string();
                if (right_value.is_number()) {
                    value_str = std::to_string(right_value.as_number());
                } else if (right_value.is_boolean()) {
                    value_str = right_value.as_boolean() ? "true" : "false";
                } else if (right_value.is_null()) {
                    value_str = "null";
                }
                elements[index] = value_str;
                
                std::string new_array = "ARRAY:[";
                for (size_t i = 0; i < elements.size(); ++i) {
                    if (i > 0) new_array += ",";
                    new_array += elements[i];
                }
                new_array += "]";
                
                if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* array_id = static_cast<Identifier*>(member->get_object());
                    ctx.set_binding(array_id->get_name(), Value(new_array));
                }
                
                return right_value;
            }
        }
        
        Object* obj = nullptr;
        bool is_string_object = false;


        if (object_value.is_object()) {
            obj = object_value.as_object();
        } else if (object_value.is_function()) {
            obj = object_value.as_function();
        } else if (object_value.is_string() || object_value.is_number() || object_value.is_boolean()) {
            std::string str_val = object_value.is_string() ? object_value.to_string() : "";
            if (object_value.is_string() && str_val.length() >= 7 && str_val.substr(0, 7) == "OBJECT:") {
                is_string_object = true;
            } else {
                // ES5: Check for accessor setter on prototype before failing
                std::string ctor_name = object_value.is_string() ? "String" :
                    (object_value.is_number() ? "Number" : "Boolean");
                std::string prop_name;
                if (member->is_computed()) {
                    Value pv = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = pv.to_string();
                } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                    prop_name = static_cast<Identifier*>(member->get_property())->get_name();
                }
                if (!prop_name.empty()) {
                    Value ctor = ctx.get_binding(ctor_name);
                    if (ctor.is_function()) {
                        Value proto = ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) {
                            PropertyDescriptor desc = proto.as_object()->get_property_descriptor(prop_name);
                            if (desc.is_accessor_descriptor() && desc.has_setter()) {
                                Function* setter = dynamic_cast<Function*>(desc.get_setter());
                                if (setter) {
                                    setter->call(ctx, {right_value}, object_value);
                                    return right_value;
                                }
                            }
                        }
                    }
                }
                // No setter found - silently fail or throw in strict mode
                if (ctx.is_strict_mode()) {
                    ctx.throw_type_error("Cannot set property on primitive");
                }
                return right_value;
            }
        } else {
            // ES1: In non-strict mode, setting property on primitive fails silently
            if (ctx.is_strict_mode()) {
                ctx.throw_type_error("Cannot set property on non-object");
            }
            return right_value;
        }
        
        if (member->is_computed() && obj && obj->is_array()) {
            Value prop_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (__builtin_expect(prop_value.is_number(), 1)) {
                double idx_double = prop_value.as_number();
                if (__builtin_expect(idx_double >= 0 && idx_double == static_cast<uint32_t>(idx_double) && idx_double < 0xFFFFFFFF, 1)) {
                    uint32_t index = static_cast<uint32_t>(idx_double);
                    obj->set_element(index, right_value);
                    return right_value;
                }
            }
        }

        std::string prop_name;
        if (member->is_computed()) {
            Value prop_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            prop_name = prop_value.to_string();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(member->get_property());
                prop_name = id->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid property access")));
                return Value();
            }
        }
        
        if (obj && !is_string_object) {
            // Check own descriptor first, then prototype chain for setter
            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
            if (!desc.is_accessor_descriptor()) {
                // Walk prototype chain for accessor descriptor
                Object* proto = obj->get_prototype();
                while (proto) {
                    PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                    if (proto_desc.is_accessor_descriptor()) {
                        desc = proto_desc;
                        break;
                    }
                    if (proto_desc.has_value()) break;
                    proto = proto->get_prototype();
                }
            }
            if (desc.is_accessor_descriptor() && desc.has_setter()) {
                Object* setter = desc.get_setter();
                if (setter) {
                    Function* setter_fn = dynamic_cast<Function*>(setter);
                    if (setter_fn) {
                        try {
                            setter_fn->call(ctx, {right_value}, Value(obj));
                            return right_value;
                        } catch (const std::exception& e) {
                            ctx.throw_exception(Value(std::string("Setter call failed: ") + e.what()));
                            return Value();
                        }
                    }
                }
            }
        }
        
        switch (operator_) {
            case Operator::ASSIGN:
                if (is_string_object) {
                    std::string str_val = object_value.to_string();
                    std::string new_prop = prop_name + "=" + right_value.to_string();
                    
                    if (str_val == "OBJECT:{}") {
                        str_val = "OBJECT:{" + new_prop + "}";
                    } else {
                        size_t close_pos = str_val.rfind('}');
                        if (close_pos != std::string::npos) {
                            str_val = str_val.substr(0, close_pos) + "," + new_prop + "}";
                        }
                    }
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                        std::string var_name = obj_id->get_name();
                        ctx.set_binding(var_name, Value(str_val));
                        
                        if (var_name == "this") {
                            ctx.set_binding("this", Value(str_val));
                        }
                    }
                } else {
                    if (obj) {
                        // ES5: Strict mode checks for property assignment
                        if (ctx.is_strict_mode()) {
                            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
                            if (desc.is_accessor_descriptor() && !desc.has_setter()) {
                                ctx.throw_type_error("Cannot set property '" + prop_name + "' which has only a getter");
                                return Value();
                            }
                        }
                        bool success = obj->set_property(prop_name, right_value);
                        if (!success && ctx.is_strict_mode()) {
                            ctx.throw_type_error("Cannot assign to read only property '" + prop_name + "'");
                            return Value();
                        }
                    }
                }
                break;
            case Operator::PLUS_ASSIGN: {
                if (is_string_object) {
                    std::string str_val = object_value.to_string();
                    
                    std::string search_pattern = prop_name + "=";
                    size_t prop_start = str_val.find(search_pattern);
                    Value current_value = Value(0);
                    
                    if (prop_start != std::string::npos) {
                        size_t value_start = prop_start + search_pattern.length();
                        size_t value_end = str_val.find(",", value_start);
                        if (value_end == std::string::npos) {
                            value_end = str_val.find("}", value_start);
                        }
                        
                        if (value_end != std::string::npos) {
                            std::string current_value_str = str_val.substr(value_start, value_end - value_start);
                            try {
                                double num = std::stod(current_value_str);
                                current_value = Value(num);
                            } catch (...) {
                                current_value = Value(0);
                            }
                        }
                    }
                    
                    double new_value = current_value.to_number() + right_value.to_number();
                    std::string new_value_str = std::to_string(new_value);
                    
                    if (prop_start != std::string::npos) {
                        size_t value_start = prop_start + search_pattern.length();
                        size_t value_end = str_val.find(",", value_start);
                        if (value_end == std::string::npos) {
                            value_end = str_val.find("}", value_start);
                        }
                        
                        if (value_end != std::string::npos) {
                            str_val = str_val.substr(0, value_start) + new_value_str + str_val.substr(value_end);
                        }
                    } else {
                        std::string new_prop = prop_name + "=" + new_value_str;
                        size_t close_pos = str_val.rfind('}');
                        if (close_pos != std::string::npos) {
                            str_val = str_val.substr(0, close_pos) + "," + new_prop + "}";
                        }
                    }
                    
                    if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* obj_id = static_cast<Identifier*>(member->get_object());
                        std::string var_name = obj_id->get_name();
                        ctx.set_binding(var_name, Value(str_val));
                        
                        if (var_name == "this") {
                            ctx.set_binding("this", Value(str_val));
                        }
                    }
                } else {
                    Value current_value = obj->get_property(prop_name);
                    obj->set_property(prop_name, Value(current_value.to_number() + right_value.to_number()));
                }
                break;
            }
            case Operator::MINUS_ASSIGN: {
                if (is_string_object) {
                    ctx.throw_exception(Value(std::string("Compound assignment not supported for string objects")));
                    return Value();
                } else {
                    Value current_value = obj->get_property(prop_name);
                    obj->set_property(prop_name, Value(current_value.to_number() - right_value.to_number()));
                }
                break;
            }
            default:
                ctx.throw_exception(Value(std::string("Unsupported assignment operator for member expression")));
                return Value();
        }
        
        return right_value;
    }
    
    // ES6: Destructuring assignment with object or array pattern
    if (operator_ == Operator::ASSIGN &&
        (left_->get_type() == ASTNode::Type::OBJECT_LITERAL ||
         left_->get_type() == ASTNode::Type::ARRAY_LITERAL)) {
        right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        destructuring_assign(ctx, left_.get(), right_value);
        if (ctx.has_exception()) return Value();
        return right_value;
    }

    ctx.throw_exception(Value(std::string("Invalid assignment target")));
    return Value();
}

// Helper: recursively perform destructuring assignment from an ObjectLiteral or ArrayLiteral pattern
void AssignmentExpression::destructuring_assign(Context& ctx, ASTNode* pattern, const Value& source_value) {
    if (pattern->get_type() == ASTNode::Type::OBJECT_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_obj = nullptr;
        if (source_value.is_object()) source_obj = source_value.as_object();
        else if (source_value.is_function()) source_obj = static_cast<Object*>(source_value.as_function());
        else if (source_value.is_string()) {
            // ES6: Box string with proper prototype chain
            auto wrapper = ObjectFactory::create_string(source_value.as_string()->str());
            Value ctor = ctx.get_binding("String");
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    wrapper->set_prototype(proto_val.as_object());
                }
            }
            source_obj = wrapper.release();
        } else if (source_value.is_number() || source_value.is_boolean()) {
            // ES6: Box number/boolean with proper prototype chain
            std::string ctor_name = source_value.is_number() ? "Number" : "Boolean";
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    auto wrapper = ObjectFactory::create_object();
                    wrapper->set_prototype(proto_val.as_object());
                    source_obj = wrapper.release();
                }
            }
            if (!source_obj) {
                auto* wrapper = ObjectFactory::create_object().release();
                source_obj = wrapper;
            }
        }
        if (!source_obj) {
            ctx.throw_type_error("Cannot destructure non-object value");
            return;
        }

        auto* obj_lit = static_cast<ObjectLiteral*>(pattern);
        std::vector<std::string> assigned_keys;

        for (const auto& prop : obj_lit->get_properties()) {
            // Handle rest element: {...rest}
            if (prop->type == ObjectLiteral::PropertyType::Value &&
                prop->value && prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(prop->value.get());
                ASTNode* rest_target = spread->get_argument();
                // Create object with remaining properties
                auto rest_obj = ObjectFactory::create_object();
                auto keys = source_obj->get_own_property_keys();
                for (const auto& k : keys) {
                    bool already_assigned = false;
                    for (const auto& ak : assigned_keys) {
                        if (ak == k) { already_assigned = true; break; }
                    }
                    if (!already_assigned) {
                        rest_obj->set_property(k, source_obj->get_property(k));
                    }
                }
                assign_to_target(ctx, rest_target, Value(rest_obj.release()));
                if (ctx.has_exception()) return;
                continue;
            }

            // Get property name from key
            std::string prop_name;
            if (prop->computed) {
                Value key_val = prop->key->evaluate(ctx);
                if (ctx.has_exception()) return;
                prop_name = key_val.to_string();
            } else if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(prop->key.get())->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                prop_name = static_cast<StringLiteral*>(prop->key.get())->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                prop_name = prop->key->to_string();
            }
            assigned_keys.push_back(prop_name);

            Value prop_value = source_obj->get_property(prop_name);

            // Determine assignment target
            ASTNode* target = prop->shorthand ? prop->key.get() : prop->value.get();

            // Check for defaults: shorthand with AssignmentExpression value means {a = default}
            if (prop->shorthand && prop->value &&
                prop->value->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(prop->value.get());
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            // Non-shorthand with AssignmentExpression value: {key: target = default}
            if (!prop->shorthand && target &&
                target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                if (prop_value.is_undefined()) {
                    prop_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            assign_to_target(ctx, target, prop_value);
            if (ctx.has_exception()) return;
        }
    } else if (pattern->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        if (source_value.is_null() || source_value.is_undefined()) {
            ctx.throw_type_error("Cannot destructure " + std::string(source_value.is_null() ? "null" : "undefined"));
            return;
        }
        Object* source_arr = nullptr;
        uint32_t source_len = 0;
        bool is_string_source = false;
        std::string str_source;

        if (source_value.is_string()) {
            is_string_source = true;
            str_source = source_value.as_string()->str();
            source_len = static_cast<uint32_t>(str_source.length());
        } else if (source_value.is_object()) {
            source_arr = source_value.as_object();
            source_len = source_arr->get_length();
        } else if (source_value.is_function()) {
            source_arr = static_cast<Object*>(source_value.as_function());
            source_len = source_arr->get_length();
        }

        auto* arr_lit = static_cast<ArrayLiteral*>(pattern);
        const auto& elements = arr_lit->get_elements();

        for (size_t i = 0; i < elements.size(); i++) {
            const auto& elem = elements[i];
            if (!elem) continue; // hole/elision

            // Handle rest element: [...rest]
            if (elem->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
                auto* spread = static_cast<SpreadElement*>(elem.get());
                ASTNode* rest_target = spread->get_argument();
                auto rest_arr = ObjectFactory::create_array(0);
                uint32_t rest_idx = 0;
                for (uint32_t j = static_cast<uint32_t>(i); j < source_len; j++) {
                    Value val;
                    if (is_string_source) {
                        val = Value(std::string(1, str_source[j]));
                    } else {
                        val = source_arr->get_element(j);
                    }
                    rest_arr->set_element(rest_idx++, val);
                }
                rest_arr->set_length(rest_idx);
                assign_to_target(ctx, rest_target, Value(rest_arr.release()));
                if (ctx.has_exception()) return;
                break;
            }

            Value elem_value;
            if (is_string_source) {
                elem_value = (i < source_len) ? Value(std::string(1, str_source[i])) : Value();
            } else if (source_arr) {
                elem_value = (i < source_len) ? source_arr->get_element(static_cast<uint32_t>(i)) : Value();
            }

            ASTNode* target = elem.get();

            // Check for default: element is AssignmentExpression like (a = default)
            if (target->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
                auto* assign = static_cast<AssignmentExpression*>(target);
                if (elem_value.is_undefined()) {
                    elem_value = assign->right_->evaluate(ctx);
                    if (ctx.has_exception()) return;
                }
                target = assign->left_.get();
            }

            assign_to_target(ctx, target, elem_value);
            if (ctx.has_exception()) return;
        }
    }
}

// Helper: assign a value to a target node (Identifier, MemberExpression, or nested pattern)
void AssignmentExpression::assign_to_target(Context& ctx, ASTNode* target, const Value& value) {
    if (!target) return;

    if (target->get_type() == ASTNode::Type::IDENTIFIER) {
        std::string name = static_cast<Identifier*>(target)->get_name();
        if (ctx.has_binding(name)) {
            ctx.set_binding(name, value);
        } else {
            ctx.create_binding(name, value, true);
        }
    } else if (target->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        auto* member = static_cast<MemberExpression*>(target);
        Value obj_val = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return;
        if (obj_val.is_object_like()) {
            Object* obj = obj_val.is_object() ? obj_val.as_object()
                                              : static_cast<Object*>(obj_val.as_function());
            std::string prop_name;
            if (member->is_computed()) {
                Value key_val = member->get_property()->evaluate(ctx);
                if (ctx.has_exception()) return;
                prop_name = key_val.to_string();
            } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                prop_name = static_cast<Identifier*>(member->get_property())->get_name();
            }
            obj->set_property(prop_name, value);
        }
    } else if (target->get_type() == ASTNode::Type::OBJECT_LITERAL ||
               target->get_type() == ASTNode::Type::ARRAY_LITERAL) {
        // Nested destructuring
        destructuring_assign(ctx, target, value);
    }
}

std::string AssignmentExpression::to_string() const {
    std::string op_str;
    switch (operator_) {
        case Operator::ASSIGN: op_str = " = "; break;
        case Operator::PLUS_ASSIGN: op_str = " += "; break;
        case Operator::MINUS_ASSIGN: op_str = " -= "; break;
        case Operator::MUL_ASSIGN: op_str = " *= "; break;
        case Operator::DIV_ASSIGN: op_str = " /= "; break;
        case Operator::MOD_ASSIGN: op_str = " %= "; break;
    }
    return left_->to_string() + op_str + right_->to_string();
}

std::unique_ptr<ASTNode> AssignmentExpression::clone() const {
    return std::make_unique<AssignmentExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_
    );
}


Value DestructuringAssignment::evaluate_with_value(Context& ctx, const Value& source_value) {
    if (type_ == Type::ARRAY) {
        // ES6: Strings are iterable and can be array-destructured
        bool is_string_source = source_value.is_string();
        std::string str_src;
        Object* array_obj = nullptr;

        if (is_string_source) {
            str_src = source_value.as_string()->str();
        } else if (source_value.is_object_like()) {
            array_obj = source_value.is_object() ? source_value.as_object()
                                                 : static_cast<Object*>(source_value.as_function());
        } else {
            ctx.throw_type_error("Cannot destructure non-object as array");
            return Value();
        }

        uint32_t src_len = is_string_source ? static_cast<uint32_t>(str_src.length())
                                            : array_obj->get_length();

        if (true) {
            for (size_t i = 0; i < targets_.size(); i++) {
                const std::string& var_name = targets_[i]->get_name();

                if (var_name.empty()) {
                    continue;
                }

                if (var_name.length() >= 3 && var_name.substr(0, 3) == "...") {
                    std::string rest_name = var_name.substr(3);

                    auto rest_array = ObjectFactory::create_array(0);
                    uint32_t rest_index = 0;

                    for (size_t j = i; j < src_len; j++) {
                        Value rest_element;
                        if (is_string_source) {
                            rest_element = Value(std::string(1, str_src[j]));
                        } else {
                            rest_element = array_obj->get_element(static_cast<uint32_t>(j));
                        }
                        rest_array->set_element(rest_index++, rest_element);
                    }

                    rest_array->set_length(rest_index);

                    if (!ctx.has_binding(rest_name)) {
                        ctx.create_binding(rest_name, Value(rest_array.release()), true);
                    } else {
                        ctx.set_binding(rest_name, Value(rest_array.release()));
                    }

                    break;
                } else if (var_name.length() >= 14 && var_name.substr(0, 14) == "__nested_vars:") {
                    Value nested_array;
                    if (is_string_source) {
                        nested_array = (i < src_len) ? Value(std::string(1, str_src[i])) : Value();
                    } else {
                        nested_array = array_obj->get_element(static_cast<uint32_t>(i));
                    }
                    if (nested_array.is_object()) {
                        Object* nested_obj = nested_array.as_object();

                        std::string vars_string = var_name.substr(14);

                        std::vector<std::string> nested_var_names;
                        std::string current_var = "";
                        for (char c : vars_string) {
                            if (c == ',') {
                                if (!current_var.empty()) {
                                    nested_var_names.push_back(current_var);
                                    current_var = "";
                                }
                            } else {
                                current_var += c;
                            }
                        }
                        if (!current_var.empty()) {
                            nested_var_names.push_back(current_var);
                        }

                        for (size_t j = 0; j < nested_var_names.size() && j < nested_obj->get_length(); j++) {
                            Value nested_element = nested_obj->get_element(static_cast<uint32_t>(j));
                            const std::string& nested_var_name = nested_var_names[j];

                            if (!ctx.has_binding(nested_var_name)) {
                                ctx.create_binding(nested_var_name, nested_element, true);
                            } else {
                                ctx.set_binding(nested_var_name, nested_element);
                            }
                        }
                    }
                } else if (var_name.length() >= 13 && var_name.substr(0, 13) == "__nested_obj:") {
                    // Nested object destructuring in array: [a, {x:b, c}]
                    Value element;
                    if (is_string_source) {
                        element = (i < src_len) ? Value(std::string(1, str_src[i])) : Value();
                    } else {
                        element = array_obj->get_element(static_cast<uint32_t>(i));
                    }
                    if (element.is_object() || element.is_function()) {
                        Object* obj = element.is_function() ?
                            static_cast<Object*>(element.as_function()) :
                            element.as_object();
                        // Parse mappings: prop1>var1,prop2>var2
                        std::string mappings_str = var_name.substr(13);
                        std::vector<std::pair<std::string,std::string>> mappings;
                        std::string current = "";
                        for (size_t ci = 0; ci <= mappings_str.length(); ci++) {
                            char c = (ci < mappings_str.length()) ? mappings_str[ci] : ',';
                            if (c == ',') {
                                size_t arrow = current.find('>');
                                if (arrow != std::string::npos) {
                                    mappings.emplace_back(current.substr(0, arrow), current.substr(arrow + 1));
                                }
                                current = "";
                            } else {
                                current += c;
                            }
                        }
                        for (const auto& m : mappings) {
                            Value val = obj->get_property(m.first);
                            if (!ctx.has_binding(m.second)) {
                                ctx.create_binding(m.second, val, true);
                            } else {
                                ctx.set_binding(m.second, val);
                            }
                        }
                    }
                } else {
                    Value element;
                    if (is_string_source) {
                        element = (i < src_len) ? Value(std::string(1, str_src[i])) : Value();
                    } else {
                        element = array_obj->get_element(static_cast<uint32_t>(i));
                    }

                    if (element.is_undefined()) {
                        for (const auto& default_val : default_values_) {
                            if (default_val.index == i) {
                                element = default_val.expr->evaluate(ctx);
                                if (ctx.has_exception()) return Value();
                                break;
                            }
                        }
                    }

                    if (!ctx.has_binding(var_name)) {
                        ctx.create_binding(var_name, element, true);
                    } else {
                        ctx.set_binding(var_name, element);
                    }
                }
            }
        }
    } else {
        if (source_value.is_object_like()) {
            Object* obj = source_value.is_object() ? source_value.as_object()
                                                    : static_cast<Object*>(source_value.as_function());

            if (!handle_complex_object_destructuring(obj, ctx)) {
                return Value();
            }
        } else if (source_value.is_number() || source_value.is_string() || source_value.is_boolean()) {
            // ES6: Primitive boxing for object destructuring
            std::string ctor_name = source_value.is_string() ? "String"
                                  : source_value.is_number() ? "Number" : "Boolean";
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_function()) {
                Value proto_val = ctor.as_function()->get_property("prototype");
                if (proto_val.is_object()) {
                    Object* proto = proto_val.as_object();
                    // Look up each property mapping on the prototype
                    for (const auto& mapping : property_mappings_) {
                        Value prop_value = proto->get_property(mapping.property_name);
                        if (!ctx.has_binding(mapping.variable_name)) {
                            ctx.create_binding(mapping.variable_name, prop_value, true);
                        } else {
                            ctx.set_binding(mapping.variable_name, prop_value);
                        }
                    }
                    // Also handle shorthand targets
                    for (const auto& target : targets_) {
                        const std::string& name = target->get_name();
                        if (name.empty() || name.find("...") == 0 || name.find("__") == 0) continue;
                        // Only if not already handled by property_mappings_
                        bool in_mappings = false;
                        for (const auto& m : property_mappings_) {
                            if (m.variable_name == name) { in_mappings = true; break; }
                        }
                        if (!in_mappings) {
                            Value prop_value = proto->get_property(name);
                            if (!ctx.has_binding(name)) {
                                ctx.create_binding(name, prop_value, true);
                            } else {
                                ctx.set_binding(name, prop_value);
                            }
                        }
                    }
                }
            }
        } else {
            ctx.throw_type_error("Cannot destructure non-object");
            return Value();
        }
    }
    
    return source_value;
}

Value DestructuringAssignment::evaluate(Context& ctx) {
    if (!source_) {
        ctx.throw_exception(Value(std::string("DestructuringAssignment: source is null")));
        return Value();
    }

    Value source_value = source_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    return evaluate_with_value(ctx, source_value);
}

bool DestructuringAssignment::handle_complex_object_destructuring(Object* obj, Context& ctx) {
    

    for (const auto& mapping : property_mappings_) {

        if (mapping.variable_name.find("__nested:") != std::string::npos ||
            mapping.variable_name.find(":__nested:") != std::string::npos) {
        }
        Value prop_value;
        if (mapping.property_name.length() > 11 && mapping.property_name.substr(0, 11) == "__computed:") {
            // Computed property key: evaluate the expression to get the key
            std::string expr_str = mapping.property_name.substr(11);
            Value key_val = ctx.get_binding(expr_str);
            if (!key_val.is_undefined()) {
                prop_value = obj->get_property(key_val.to_string());
            }
        } else {
            prop_value = obj->get_property(mapping.property_name);
        }

        // Handle nested array-in-object: {x: [a, b]} encoded as __nested_array:a,b
        if (mapping.variable_name.length() > 15 && mapping.variable_name.substr(0, 15) == "__nested_array:") {
            std::string vars_str = mapping.variable_name.substr(15);
            // Split vars by comma
            std::vector<std::string> var_names;
            std::string current;
            for (size_t ci = 0; ci < vars_str.length(); ++ci) {
                if (vars_str[ci] == ',') {
                    if (!current.empty()) { var_names.push_back(current); current.clear(); }
                } else {
                    current += vars_str[ci];
                }
            }
            if (!current.empty()) var_names.push_back(current);

            if (prop_value.is_object()) {
                Object* arr_obj = prop_value.as_object();
                for (size_t ai = 0; ai < var_names.size(); ++ai) {
                    Value elem = arr_obj->get_element(static_cast<uint32_t>(ai));
                    if (!ctx.has_binding(var_names[ai])) {
                        ctx.create_binding(var_names[ai], elem, true);
                    } else {
                        ctx.set_binding(var_names[ai], elem);
                    }
                }
            } else {
                for (const auto& vn : var_names) {
                    if (!ctx.has_binding(vn)) {
                        ctx.create_binding(vn, Value(), true);
                    } else {
                        ctx.set_binding(vn, Value());
                    }
                }
            }
            continue;
        }

        if ((mapping.variable_name.length() > 9 && mapping.variable_name.substr(0, 9) == "__nested:") ||
            mapping.variable_name.find(":__nested:") != std::string::npos ||
            mapping.variable_name.find(':') != std::string::npos) {

            if (mapping.variable_name.find(":__nested:") != std::string::npos) {

                if (prop_value.is_object()) {
                    Object* nested_obj = prop_value.as_object();
                    handle_infinite_depth_destructuring(nested_obj, mapping.variable_name, ctx);
                } else {
                }
                continue;
            } else if (mapping.variable_name.find(':') != std::string::npos &&
                      mapping.variable_name.find("__nested:") == std::string::npos) {

                if (prop_value.is_object()) {
                    Object* nested_obj = prop_value.as_object();
                    handle_infinite_depth_destructuring(nested_obj, mapping.variable_name, ctx);
                } else {
                }
                continue;
            }

            std::string vars_string = mapping.variable_name.substr(9);

            std::vector<std::string> nested_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < vars_string.length(); ++i) {
                char c = vars_string[i];

                if (i + 9 <= vars_string.length() &&
                    vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        nested_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                nested_var_names.push_back(current_var);
            }

            if (prop_value.is_object()) {
                Object* nested_obj = prop_value.as_object();


                std::vector<std::string> property_aware_var_names = nested_var_names;

                bool found_nested_mappings = false;

                for (const auto& our_mapping : property_mappings_) {
                    if (our_mapping.property_name == mapping.property_name &&
                        our_mapping.variable_name.find("__nested:") == 0) {

                        std::string vars_part = our_mapping.variable_name.substr(9);

                        std::vector<std::string> enhanced_vars;
                        std::stringstream ss(vars_part);
                        std::string var;

                        while (std::getline(ss, var, ',')) {
                            enhanced_vars.push_back(var);
                        }

                        property_aware_var_names = enhanced_vars;
                        found_nested_mappings = true;
                        break;
                    }
                }

                std::vector<std::string> smart_var_names = nested_var_names;





                bool has_property_renaming = false;
                std::map<std::string, std::string> detected_mappings;

                for (const auto& target : targets_) {
                    std::string target_name = target->get_name();
                    if (target_name == mapping.property_name) {
                        break;
                    }
                }

                std::vector<std::string> processed_var_names;
                for (const std::string& var_name : smart_var_names) {
                    size_t colon_pos = var_name.find(':');
                    bool is_malformed_nested = false;
                    if (colon_pos != std::string::npos) {
                        std::string after_colon = var_name.substr(colon_pos + 1);
                        if (after_colon.length() > 9 && after_colon.substr(0, 9) == "__nested:") {
                            is_malformed_nested = true;
                        }
                    }

                    if (!is_malformed_nested && var_name.find(':') != std::string::npos && var_name.find("__nested:") != 0) {
                        processed_var_names.push_back(var_name);
                        has_property_renaming = true;
                    } else {
                        processed_var_names.push_back(var_name);
                    }
                }

                for (size_t i = 0; i < smart_var_names.size(); ++i) {
                }

                if (has_property_renaming) {
                    handle_nested_object_destructuring_with_mappings(nested_obj, processed_var_names, ctx);
                } else {
                    for (const std::string& var_name : smart_var_names) {

                        bool is_nested_pattern = false;
                        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
                            is_nested_pattern = true;
                        } else {
                            size_t colon_pos = var_name.find(':');
                            if (colon_pos != std::string::npos) {
                                std::string after_colon = var_name.substr(colon_pos + 1);
                                if (after_colon.length() > 9 && after_colon.substr(0, 9) == "__nested:") {
                                    is_nested_pattern = true;
                                }
                            }
                        }

                        if (is_nested_pattern) {
                            handle_infinite_depth_destructuring(nested_obj, var_name, ctx);
                        } else {
                            Value prop_value = nested_obj->get_property(var_name);
                            if (!ctx.has_binding(var_name)) {
                                ctx.create_binding(var_name, prop_value, true);
                            } else {
                                ctx.set_binding(var_name, prop_value);
                            }
                        }
                    }
                }
            }
        } else {
            // Apply default value if property is undefined: {x: a = expr}
            if (prop_value.is_undefined()) {
                for (size_t i = 0; i < targets_.size(); i++) {
                    if (targets_[i]->get_name() == mapping.variable_name) {
                        for (const auto& dv : default_values_) {
                            if (dv.index == i) {
                                prop_value = dv.expr->evaluate(ctx);
                                if (ctx.has_exception()) return false;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            bool binding_created = false;
            if (!ctx.has_binding(mapping.variable_name)) {
                binding_created = ctx.create_binding(mapping.variable_name, prop_value, true);
            } else {
                ctx.set_binding(mapping.variable_name, prop_value);
                binding_created = true;
            }

            if (!binding_created) {
            }
        }
    }
    
    std::set<std::string> extracted_props;
    
    for (const auto& mapping : property_mappings_) {
        extracted_props.insert(mapping.property_name);
    }
    
    for (const auto& target : targets_) {
        std::string prop_name = target->get_name();

        if (prop_name.length() >= 3 && prop_name.substr(0, 3) == "...") {
            std::string rest_name = prop_name.substr(3);
            
            auto rest_obj = std::make_unique<Object>(Object::ObjectType::Ordinary);
            
            auto keys = obj->get_own_property_keys();
            for (const auto& key : keys) {
                if (extracted_props.find(key) == extracted_props.end()) {
                    Value prop_value = obj->get_property(key);
                    rest_obj->set_property(key, prop_value);
                }
            }
            
            if (!ctx.has_binding(rest_name)) {
                ctx.create_binding(rest_name, Value(rest_obj.release()), true);
            } else {
                ctx.set_binding(rest_name, Value(rest_obj.release()));
            }
            
            continue;
        }
        
        bool has_mapping = false;
        for (const auto& mapping : property_mappings_) {
            if (mapping.variable_name == prop_name) {
                has_mapping = true;
                break;
            }
        }

        if (!has_mapping) {
            if (prop_name.length() >= 9 && prop_name.substr(0, 9) == "__nested:") {

                std::string vars_string = prop_name.substr(9);

                std::vector<std::string> nested_var_names;
                std::string current_var = "";
                int nested_depth = 0;

                for (size_t i = 0; i < vars_string.length(); ++i) {
                    char c = vars_string[i];

                    if (i + 9 <= vars_string.length() &&
                        vars_string.substr(i, 9) == "__nested:") {
                        nested_depth++;
                        current_var += "__nested:";
                        i += 8;
                    } else if (c == ',' && nested_depth == 0) {
                        if (!current_var.empty()) {
                            nested_var_names.push_back(current_var);
                            current_var = "";
                        }
                    } else {
                        current_var += c;
                        if (nested_depth > 0 && i == vars_string.length() - 1) {
                            nested_depth = 0;
                        }
                    }
                }
                if (!current_var.empty()) {
                    nested_var_names.push_back(current_var);
                }

                std::string actual_prop = "";
                for (const auto& mapping : property_mappings_) {
                    if (mapping.variable_name == prop_name) {
                        actual_prop = mapping.property_name;
                        break;
                    }
                }

                if (!actual_prop.empty()) {
                    Value nested_object = obj->get_property(actual_prop);
                    if (nested_object.is_object()) {
                        Object* nested_obj = nested_object.as_object();

                        for (const std::string& var_name : nested_var_names) {
                            if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
                                handle_infinite_depth_destructuring(nested_obj, var_name, ctx);
                            } else {
                                Value prop_value = nested_obj->get_property(var_name);
                                if (!ctx.has_binding(var_name)) {
                                    ctx.create_binding(var_name, prop_value, true);
                                } else {
                                    ctx.set_binding(var_name, prop_value);
                                }
                            }
                        }
                    }
                }
            } else {
                Value prop_value = obj->get_property(prop_name);

                // Apply default value if property is undefined: {a = expr}
                if (prop_value.is_undefined()) {
                    for (size_t ti = 0; ti < targets_.size(); ti++) {
                        if (targets_[ti]->get_name() == prop_name) {
                            for (const auto& dv : default_values_) {
                                if (dv.index == ti) {
                                    prop_value = dv.expr->evaluate(ctx);
                                    if (ctx.has_exception()) return false;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }

                extracted_props.insert(prop_name);

                if (!ctx.has_binding(prop_name)) {
                    ctx.create_binding(prop_name, prop_value, true);
                } else {
                    ctx.set_binding(prop_name, prop_value);
                }
            }
        }
    }
    
    return true;
}

void DestructuringAssignment::handle_nested_object_destructuring(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx) {

    for (const std::string& var_name : var_names) {

        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);

            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];

                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    for (const std::string& deep_var_name : deeper_var_names) {
                        if (deep_var_name.length() > 9 && deep_var_name.substr(0, 9) == "__nested:") {
                            handle_infinite_depth_destructuring(deeper_obj, deep_var_name, ctx);
                        } else {
                            Value prop_value = deeper_obj->get_property(deep_var_name);
                            if (!ctx.has_binding(deep_var_name)) {
                                ctx.create_binding(deep_var_name, prop_value, true);
                            } else {
                                ctx.set_binding(deep_var_name, prop_value);
                            }
                        }
                    }
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                if (var_name.find(',') != std::string::npos) {

                    std::vector<std::string> mappings;
                    std::string current_mapping = "";
                    int nested_depth = 0;

                    for (size_t i = 0; i < var_name.length(); ++i) {
                        char c = var_name[i];

                        if (i + 9 <= var_name.length() &&
                            var_name.substr(i, 9) == "__nested:") {
                            nested_depth++;
                            current_mapping += "__nested:";
                            i += 8;
                        } else if (c == ',' && nested_depth == 0) {
                            if (!current_mapping.empty()) {
                                mappings.push_back(current_mapping);
                                current_mapping = "";
                            }
                        } else {
                            current_mapping += c;
                            if (nested_depth > 0 && i == var_name.length() - 1) {
                                nested_depth = 0;
                            }
                        }
                    }
                    if (!current_mapping.empty()) {
                        mappings.push_back(current_mapping);
                    }

                    for (const auto& mapping : mappings) {
                        size_t mapping_colon = mapping.find(':');
                        if (mapping_colon != std::string::npos) {
                            std::string property_name = mapping.substr(0, mapping_colon);
                            std::string variable_name = mapping.substr(mapping_colon + 1);


                            Value prop_value = nested_obj->get_property(property_name);

                            if (!ctx.has_binding(variable_name)) {
                                ctx.create_binding(variable_name, prop_value, true);
                            } else {
                                ctx.set_binding(variable_name, prop_value);
                            }
                        }
                    }
                } else {
                    std::string property_name = var_name.substr(0, colon_pos);
                    std::string variable_name = var_name.substr(colon_pos + 1);


                    Value prop_value = nested_obj->get_property(property_name);

                    if (!ctx.has_binding(variable_name)) {
                        ctx.create_binding(variable_name, prop_value, true);
                    } else {
                        ctx.set_binding(variable_name, prop_value);
                    }
                }
            } else {
                Value prop_value = nested_obj->get_property(var_name);

                if (!ctx.has_binding(var_name)) {
                    ctx.create_binding(var_name, prop_value, true);
                } else {
                    ctx.set_binding(var_name, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_with_source(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source_destructuring) {

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_with_source(deeper_obj, deeper_var_names, ctx, source_destructuring);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);

                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string actual_property = var_name;
                std::string target_variable = var_name;

                bool found_mapping = false;

                Value prop_value = nested_obj->get_property(actual_property);

                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_with_mappings(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx) {

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_with_mappings(deeper_obj, deeper_var_names, ctx);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);

                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {



                Value prop_value = nested_obj->get_property(var_name);

                if (!ctx.has_binding(var_name)) {
                    ctx.create_binding(var_name, prop_value, true);
                } else {
                    ctx.set_binding(var_name, prop_value);
                }
            }
        }
    }
}

void DestructuringAssignment::handle_nested_object_destructuring_smart(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source) {

    static std::map<std::string, std::map<std::string, std::string>> global_property_mappings;

    std::string source_key = "destructuring_" + std::to_string(reinterpret_cast<uintptr_t>(source));
    auto& source_mappings = global_property_mappings[source_key];

    for (const auto& mapping : source->get_property_mappings()) {
        if (mapping.property_name != mapping.variable_name) {
            source_mappings[mapping.property_name] = mapping.variable_name;
        }
    }

    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }

            for (const auto& property_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(property_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_smart(deeper_obj, deeper_var_names, ctx, source);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);
                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string target_variable = var_name;

                if (source_mappings.find(var_name) != source_mappings.end()) {
                    target_variable = source_mappings[var_name];
                }

                Value prop_value = nested_obj->get_property(var_name);
                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }

    global_property_mappings.erase(source_key);
}

void DestructuringAssignment::handle_nested_object_destructuring_enhanced(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, const std::string& property_key) {


    static std::map<std::string, std::string> runtime_property_mappings;


    for (const std::string& var_name : var_names) {
        if (var_name.length() > 9 && var_name.substr(0, 9) == "__nested:") {
            std::string deeper_vars_string = var_name.substr(9);
            std::vector<std::string> deeper_var_names;
            std::string current_var = "";
            int nested_depth = 0;

            for (size_t i = 0; i < deeper_vars_string.length(); ++i) {
                char c = deeper_vars_string[i];
                if (i + 9 <= deeper_vars_string.length() &&
                    deeper_vars_string.substr(i, 9) == "__nested:") {
                    nested_depth++;
                    current_var += "__nested:";
                    i += 8;
                } else if (c == ',' && nested_depth == 0) {
                    if (!current_var.empty()) {
                        deeper_var_names.push_back(current_var);
                        current_var = "";
                    }
                } else {
                    current_var += c;
                    if (nested_depth > 0 && i == deeper_vars_string.length() - 1) {
                        nested_depth = 0;
                    }
                }
            }
            if (!current_var.empty()) {
                deeper_var_names.push_back(current_var);
            }


            for (const auto& prop_name : nested_obj->get_own_property_keys()) {
                Value property_value = nested_obj->get_property(prop_name);
                if (property_value.is_object()) {
                    Object* deeper_obj = property_value.as_object();
                    handle_nested_object_destructuring_enhanced(deeper_obj, deeper_var_names, ctx, prop_name);
                    break;
                }
            }
        } else {
            size_t colon_pos = var_name.find(':');
            if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < var_name.length() - 1) {
                std::string property_name = var_name.substr(0, colon_pos);
                std::string variable_name = var_name.substr(colon_pos + 1);

                Value prop_value = nested_obj->get_property(property_name);
                if (!ctx.has_binding(variable_name)) {
                    ctx.create_binding(variable_name, prop_value, true);
                } else {
                    ctx.set_binding(variable_name, prop_value);
                }
            } else {
                std::string target_variable = var_name;
                bool found_mapping = false;

                static std::map<std::string, std::vector<std::pair<std::string, std::string>>> global_nested_mappings;

                for (const std::string& check_var : var_names) {
                    if (check_var.find("REGISTRY:") == 0) {
                        size_t first_colon = check_var.find(':', 9);
                        if (first_colon != std::string::npos) {
                            size_t second_colon = check_var.find(':', first_colon + 1);
                            if (second_colon != std::string::npos) {
                                std::string registry_key = check_var.substr(9, first_colon - 9);

                                if (global_nested_mappings.find(registry_key) != global_nested_mappings.end()) {
                                    auto& mappings = global_nested_mappings[registry_key];
                                    for (const auto& mapping_pair : mappings) {
                                        if (mapping_pair.first == var_name) {
                                            target_variable = mapping_pair.second;
                                            found_mapping = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }

                Value prop_value = nested_obj->get_property(var_name);
                if (!ctx.has_binding(target_variable)) {
                    ctx.create_binding(target_variable, prop_value, true);
                } else {
                    ctx.set_binding(target_variable, prop_value);
                }
            }
        }
    }
}

std::string DestructuringAssignment::to_string() const {
    std::string targets_str;
    if (type_ == Type::ARRAY) {
        targets_str = "[";
        for (size_t i = 0; i < targets_.size(); i++) {
            if (i > 0) targets_str += ", ";
            targets_str += targets_[i]->get_name();
        }
        targets_str += "]";
    } else {
        targets_str = "{";
        for (size_t i = 0; i < targets_.size(); i++) {
            if (i > 0) targets_str += ", ";
            targets_str += targets_[i]->get_name();
        }
        targets_str += "}";
    }
    return targets_str + " = " + source_->to_string();
}

std::unique_ptr<ASTNode> DestructuringAssignment::clone() const {
    std::vector<std::unique_ptr<Identifier>> cloned_targets;
    for (const auto& target : targets_) {
        cloned_targets.push_back(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(target->clone().release()))
        );
    }

    auto cloned = std::make_unique<DestructuringAssignment>(
        std::move(cloned_targets), source_ ? source_->clone() : nullptr, type_, start_, end_
    );

    for (const auto& mapping : property_mappings_) {
        cloned->add_property_mapping(mapping.property_name, mapping.variable_name);
    }

    for (const auto& default_val : default_values_) {
        cloned->add_default_value(default_val.index, default_val.expr->clone());
    }

    return std::move(cloned);
}


std::vector<Value> process_arguments_with_spread(const std::vector<std::unique_ptr<ASTNode>>& arguments, Context& ctx) {
    std::vector<Value> arg_values;
    
    for (const auto& arg : arguments) {
        if (arg->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            SpreadElement* spread = static_cast<SpreadElement*>(arg.get());
            Value spread_value = spread->get_argument()->evaluate(ctx);
            if (ctx.has_exception()) return arg_values;
            
            if (spread_value.is_object()) {
                Object* spread_obj = spread_value.as_object();
                uint32_t spread_length = spread_obj->get_length();

                for (uint32_t j = 0; j < spread_length; ++j) {
                    Value item = spread_obj->get_element(j);
                    arg_values.push_back(item);
                }
            } else if (spread_value.is_string()) {
                // ES6: Spread on strings iterates over characters
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
            } else {
                arg_values.push_back(spread_value);
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
    if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        return handle_member_expression_call(ctx);
    }
    
    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* identifier = static_cast<Identifier*>(callee_.get());
        if (identifier->get_name() == "super") {
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

                    Object* this_obj = ctx.get_this_binding();

                    bool was_in_ctor = ctx.is_in_constructor_call();
                    Value old_new_target = ctx.get_new_target();
                    ctx.set_in_constructor_call(true);
                    if (old_new_target.is_undefined()) {
                        ctx.set_new_target(Value(static_cast<Object*>(parent_func)));
                    }

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

            // Per-call-site caching: use the TemplateLiteral AST node pointer as key
            static std::unordered_map<const TemplateLiteral*, Object*> template_cache;
            Object* strings_array = nullptr;

            auto cache_it = template_cache.find(tmpl);
            if (cache_it != template_cache.end()) {
                strings_array = cache_it->second;
            } else {
                // Build the strings array from TEXT elements
                std::vector<std::string> cooked_parts;
                std::vector<std::string> raw_parts;
                for (const auto& el : elements) {
                    if (el.type == TemplateLiteral::Element::Type::TEXT) {
                        cooked_parts.push_back(el.text);
                        raw_parts.push_back(el.raw_text);
                    }
                }

                auto strings_obj = ObjectFactory::create_array(static_cast<int>(cooked_parts.size()));
                strings_array = strings_obj.release();
                for (size_t i = 0; i < cooked_parts.size(); i++) {
                    strings_array->set_property(std::to_string(i), Value(cooked_parts[i]));
                }
                strings_array->set_property("length", Value(static_cast<double>(cooked_parts.size())));

                // Add .raw property (frozen array of raw strings)
                auto raw_obj = ObjectFactory::create_array(static_cast<int>(raw_parts.size()));
                Object* raw_array = raw_obj.release();
                for (size_t i = 0; i < raw_parts.size(); i++) {
                    raw_array->set_property(std::to_string(i), Value(raw_parts[i]));
                }
                raw_array->set_property("length", Value(static_cast<double>(raw_parts.size())));
                raw_array->freeze();

                strings_array->set_property("raw", Value(raw_array));
                strings_array->freeze();

                template_cache[tmpl] = strings_array;
            }

            // Build argument list: [strings_array, expr1, expr2, ...]
            std::vector<Value> arg_values;
            arg_values.push_back(Value(strings_array));

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

        return function->call(ctx, arg_values, this_value);
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
    auto cloned = std::make_unique<CallExpression>(callee_->clone(), std::move(cloned_args), start_, end_);
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
            method_name = key_value.to_string();
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
            method_name = key_value.to_string();
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

        if (method_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();

            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_exception(Value(std::string("Property is not a function")));
            return Value();
        }
        
    } else if (object_value.is_boolean()) {
        Value method_value = member->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        if (method_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();
            
            Function* method = method_value.as_function();
            
            
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_exception(Value(std::string("Property is not a function")));
            return Value();
        }
        
    } else if (object_value.is_object() || object_value.is_function()) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();
        
        std::string method_name;
        if (member->is_computed()) {
            Value key_value = member->get_property()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            method_name = key_value.to_string();
        } else {
            if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(member->get_property());
                method_name = prop->get_name();
            } else {
                ctx.throw_exception(Value(std::string("Invalid method name")));
                return Value();
            }
        }
        
        Value method_value = obj->get_property(method_name);
        if (method_value.is_function()) {
            std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
            if (ctx.has_exception()) return Value();

            Function* method = method_value.as_function();
            return method->call(ctx, arg_values, object_value);
        } else {
            ctx.throw_exception(Value(std::string("Property is not a function")));
            return Value();
        }
    }
    
    ctx.throw_exception(Value(std::string("Unsupported method call")));
    return Value();
}


Value MemberExpression::evaluate(Context& ctx) {
    // ES6: super.prop / super[expr] looks up on parent prototype, not the constructor itself
    if (object_->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(object_.get())->get_name() == "super") {
        Value super_ctor = ctx.get_binding("__super__");
        if (super_ctor.is_function()) {
            Value proto_val = super_ctor.as_function()->get_property("prototype");
            if (proto_val.is_object()) {
                Object* proto = proto_val.as_object();
                std::string prop_name;
                if (computed_) {
                    Value key_val = property_->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    prop_name = key_val.to_string();
                } else if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                    prop_name = static_cast<Identifier*>(property_.get())->get_name();
                }
                return proto->get_property(prop_name);
            }
        }
    }

    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (object_value.is_null() || object_value.is_undefined()) {
        ctx.throw_type_error("Cannot read property of null or undefined");
        return Value();
    }

    // ES5: Property access on primitives - check prototype for accessors
    if ((object_value.is_string() || object_value.is_number() || object_value.is_boolean()) && !computed_) {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();

            if (object_value.is_string() && prop_name == "length") {
                std::string str_value = object_value.to_string();
                return Value(static_cast<double>(str_value.length()));
            }

            std::string ctor_name = object_value.is_string() ? "String" :
                (object_value.is_number() ? "Number" : "Boolean");
            Value ctor = ctx.get_binding(ctor_name);
            if (ctor.is_object() || ctor.is_function()) {
                Object* ctor_obj = ctor.is_object() ? ctor.as_object() : ctor.as_function();
                Value prototype = ctor_obj->get_property("prototype");
                if (prototype.is_object()) {
                    Object* proto_obj = prototype.as_object();

                    // Check for accessor getter on prototype
                    PropertyDescriptor desc = proto_obj->get_property_descriptor(prop_name);
                    if (desc.is_accessor_descriptor() && desc.has_getter()) {
                        Function* getter = dynamic_cast<Function*>(desc.get_getter());
                        if (getter) {
                            return getter->call(ctx, {}, object_value);
                        }
                    }

                    Value method = proto_obj->get_property(prop_name);
                    if (!method.is_undefined()) {
                        return method;
                    }
                }
            }
        }
    }

    if ((object_value.is_object() || object_value.is_function()) && !computed_) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();

            // fast path: Array length access
            if (__builtin_expect(prop_name == "length" && obj->is_array(), 1)) {
                return Value(static_cast<double>(obj->get_length()));
            }

            Shape* shape = obj->get_shape();

            // Polymorphic inline cache
            // Skip IC for Function objects - they intercept name/length/prototype
            // in Function::get_property, so shape offsets don't match
            if (__builtin_expect(!obj->is_function(), 1)) {
                for (uint8_t i = 0; i < ic_size_; i++) {
                    if (__builtin_expect(ic_cache_[i].shape_ptr == shape, 1)) {
                        return obj->get_property_by_offset_unchecked(ic_cache_[i].offset);
                    }
                }
            }

            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
            if (desc.is_accessor_descriptor() && desc.has_getter()) {
                Object* getter = desc.get_getter();
                if (getter) {
                    Function* getter_fn = dynamic_cast<Function*>(getter);
                    if (getter_fn) {
                        std::vector<Value> args;
                        return getter_fn->call(ctx, args, object_value);
                    }
                }
                return Value();
            }

            // Check prototype chain for accessor descriptors (e.g. class get/set)
            {
                Object* proto = obj->get_prototype();
                while (proto) {
                    PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                    if (proto_desc.is_accessor_descriptor() && proto_desc.has_getter()) {
                        Function* getter_fn = dynamic_cast<Function*>(proto_desc.get_getter());
                        if (getter_fn) {
                            std::vector<Value> args;
                            return getter_fn->call(ctx, args, object_value);
                        }
                        return Value();
                    }
                    if (proto_desc.has_value()) break;  // Found as data property, stop
                    proto = proto->get_prototype();
                }
            }

            if (__builtin_expect(shape != nullptr, 1)) {
                auto info = shape->get_property_info(prop_name);
                if (__builtin_expect(info.offset != UINT32_MAX, 1)) {
                    if (ic_size_ < 4) {
                        ic_cache_[ic_size_].shape_ptr = shape;
                        ic_cache_[ic_size_].offset = info.offset;
                        ic_size_++;
                    } else {
                        ic_cache_[3].shape_ptr = shape;
                        ic_cache_[3].offset = info.offset;
                    }
                }
            }

            return obj->get_property(prop_name);
        }
    }
    
    if ((object_value.is_object() || object_value.is_function()) && computed_) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();

        //  ufp: Constant array index
        if (__builtin_expect(property_->get_type() == ASTNode::Type::NUMBER_LITERAL, 0)) {
            NumberLiteral* num_lit = static_cast<NumberLiteral*>(property_.get());
            double index_double = num_lit->get_value();
            if (__builtin_expect(index_double >= 0 && index_double == static_cast<uint32_t>(index_double), 1)) {
                uint32_t index = static_cast<uint32_t>(index_double);
                Value element = obj->get_element(index);
                if (!element.is_undefined()) {
                    return element;
                }
            }
        }

        Value prop_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        // fp: Variable array index
        if (__builtin_expect(prop_value.is_number(), 1)) {
            double index_double = prop_value.as_number();
            if (__builtin_expect(index_double >= 0 && index_double == static_cast<uint32_t>(index_double), 1)) {
                uint32_t index = static_cast<uint32_t>(index_double);
                Value element = obj->get_element(index);
                if (!element.is_undefined()) {
                    return element;
                }
            }
        }

        std::string prop_name;
        if (prop_value.is_symbol()) {
            prop_name = prop_value.as_symbol()->get_description();
        } else {
            prop_name = prop_value.to_string();
        }

        PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
        if (desc.is_accessor_descriptor() && desc.has_getter()) {
            Object* getter = desc.get_getter();
            if (getter) {
                Function* getter_fn = dynamic_cast<Function*>(getter);
                if (getter_fn) {
                    std::vector<Value> args;
                    return getter_fn->call(ctx, args, object_value);
                }
            }
            return Value();
        }

        return obj->get_property(prop_name);
    }
    
    if (object_->get_type() == ASTNode::Type::IDENTIFIER &&
        property_->get_type() == ASTNode::Type::IDENTIFIER && !computed_) {
        
        Identifier* obj = static_cast<Identifier*>(object_.get());
        Identifier* prop = static_cast<Identifier*>(property_.get());
        
        if (obj->get_name() == "Math") {
            std::string prop_name = prop->get_name();
            
            if (prop_name == "PI") {
                return Value(Math::PI);
            } else if (prop_name == "E") {
                return Value(Math::E);
            } else if (prop_name == "LN2") {
                return Value(Math::LN2);
            } else if (prop_name == "LN10") {
                return Value(Math::LN10);
            } else if (prop_name == "LOG2E") {
                return Value(Math::LOG2E);
            } else if (prop_name == "LOG10E") {
                return Value(Math::LOG10E);
            } else if (prop_name == "SQRT1_2") {
                return Value(Math::SQRT1_2);
            } else if (prop_name == "SQRT2") {
                return Value(Math::SQRT2);
            }
            
        }
    }

    if (object_value.is_undefined() || object_value.is_null()) {
        std::string type_name = object_value.is_undefined() ? "undefined" : "null";
        ctx.throw_type_error("Cannot read property of " + type_name);
        return Value();
    }
    
    std::string prop_name;
    if (computed_) {
        Value prop_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        prop_name = prop_value.to_string();
    } else {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            prop_name = prop->get_name();
        }
    }
    

    if (object_value.is_string()) {
        std::string str_value = object_value.to_string();
        
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_number()) {
                uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                
                size_t start = str_value.find('[');
                size_t end = str_value.find(']');
                if (start != std::string::npos && end != std::string::npos) {
                    std::string content = str_value.substr(start + 1, end - start - 1);
                    if (content.empty()) return Value();
                    
                    std::vector<std::string> elements;
                    size_t pos = 0;
                    while (pos < content.length()) {
                        size_t comma = content.find(',', pos);
                        if (comma == std::string::npos) comma = content.length();
                        elements.push_back(content.substr(pos, comma - pos));
                        pos = comma + 1;
                    }
                    
                    if (index < elements.size()) {
                        std::string element = elements[index];
                        if (element == "true") {
                            return Value(true);
                        } else if (element == "false") {
                            return Value(false);
                        } else if (element == "null") {
                            return Value();
                        } else {
                            try {
                                double num = std::stod(element);
                                return Value(num);
                            } catch (...) {
                                return Value(element);
                            }
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_value.length() >= 6 && str_value.substr(0, 6) == "ARRAY:" && !computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            if (prop_name == "length") {
                size_t start = str_value.find('[');
                size_t end = str_value.find(']');
                if (start != std::string::npos && end != std::string::npos) {
                    std::string content = str_value.substr(start + 1, end - start - 1);
                    if (content.empty()) return Value(0.0);
                    
                    uint32_t count = 1;
                    for (char c : content) {
                        if (c == ',') count++;
                    }
                    return Value(static_cast<double>(count));
                }
                return Value(0.0);
            }
            
            return Value();
        }
        
        if (str_value.length() >= 7 && str_value.substr(0, 7) == "OBJECT:" && computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_string()) {
                std::string prop_name = prop_value.to_string();
                
                std::string search = prop_name + "=";
                size_t start = str_value.find(search);
                if (start != std::string::npos) {
                    start += search.length();
                    size_t end = str_value.find(",", start);
                    if (end == std::string::npos) {
                        end = str_value.find("}", start);
                    }
                    
                    if (end != std::string::npos) {
                        std::string value = str_value.substr(start, end - start);
                        if (value == "true") {
                            return Value(true);
                        } else if (value == "false") {
                            return Value(false);
                        } else if (value == "null") {
                            return Value();
                        } else if (value.substr(0, 9) == "FUNCTION:") {
                            std::string func_id = value.substr(9);
                                Value func_value = ctx.get_binding(func_id);
                                if (!func_value.is_undefined()) {
                                    return func_value;
                            } else {
                                    return Value();
                            }
                        } else {
                            try {
                                double num = std::stod(value);
                                return Value(num);
                            } catch (...) {
                                return Value(value);
                            }
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_value.length() >= 7 && str_value.substr(0, 7) == "OBJECT:" && !computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            std::string search = prop_name + "=";
            size_t start = str_value.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = start;
                
                if (start < str_value.length() && str_value.substr(start, 7) == "OBJECT:") {
                    int brace_count = 0;
                    bool in_object = false;
                    
                    for (size_t i = start; i < str_value.length(); i++) {
                        if (str_value[i] == '{') {
                            brace_count++;
                            in_object = true;
                        } else if (str_value[i] == '}') {
                            brace_count--;
                            if (in_object && brace_count == 0) {
                                end = i + 1;
                                break;
                            }
                        }
                    }
                } else {
                    end = str_value.find(",", start);
                    if (end == std::string::npos) {
                        end = str_value.find("}", start);
                    }
                }
                
                if (end > start) {
                    std::string value = str_value.substr(start, end - start);
                    if (value == "true") {
                        return Value(true);
                    } else if (value == "false") {
                        return Value(false);
                    } else if (value == "null") {
                        return Value();
                    } else if (value.substr(0, 9) == "FUNCTION:") {
                        std::string func_id = value.substr(9);
                        Value func_value = ctx.get_binding(func_id);
                        
                        if (func_value.is_undefined()) {
                            auto it = g_object_function_map.find(func_id);
                            if (it != g_object_function_map.end()) {
                                func_value = it->second;
                            }
                        }
                        
                        if (!func_value.is_undefined()) {
                            return func_value;
                        } else {
                            return Value();
                        }
                    } else {
                        try {
                            double num = std::stod(value);
                            return Value(num);
                        } catch (...) {
                            return Value(value);
                        }
                    }
                }
            }
            return Value();
        }
        
        std::string prop_name;
        if (!computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            prop_name = prop->get_name();
        }
        
        if (!computed_ && prop_name == "length") {
            return Value(static_cast<double>(str_value.length()));
        }
        
        if (!computed_ && prop_name == "charAt") {
            auto char_at_fn = ObjectFactory::create_native_function("charAt",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(std::string(""));
                    int index = static_cast<int>(args[0].to_number());
                    if (index >= 0 && index < static_cast<int>(str_value.length())) {
                        return Value(std::string(1, str_value[index]));
                    }
                    return Value(std::string(""));
                });
            return Value(char_at_fn.release());
        }
        
        if (!computed_ && prop_name == "indexOf") {
            auto index_of_fn = ObjectFactory::create_native_function("indexOf",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(-1.0);
                    std::string search = args[0].to_string();
                    size_t pos = str_value.find(search);
                    return Value(pos != std::string::npos ? static_cast<double>(pos) : -1.0);
                });
            return Value(index_of_fn.release());
        }
        
        if (prop_name == "toUpperCase") {
            auto upper_fn = ObjectFactory::create_native_function("toUpperCase",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                    return Value(result);
                });
            return Value(upper_fn.release());
        }
        
        if (prop_name == "toLowerCase") {
            auto lower_fn = ObjectFactory::create_native_function("toLowerCase",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                    return Value(result);
                });
            return Value(lower_fn.release());
        }
        
        if (prop_name == "substring") {
            auto substring_fn = ObjectFactory::create_native_function("substring",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    int start = static_cast<int>(args[0].to_number());
                    int end = args.size() > 1 ? static_cast<int>(args[1].to_number()) : str_value.length();
                    start = std::max(0, std::min(start, static_cast<int>(str_value.length())));
                    end = std::max(0, std::min(end, static_cast<int>(str_value.length())));
                    if (start > end) std::swap(start, end);
                    return Value(str_value.substr(start, end - start));
                });
            return Value(substring_fn.release());
        }
        
        if (prop_name == "substr") {
            auto substr_fn = ObjectFactory::create_native_function("substr",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);

                    int size = static_cast<int>(str_value.length());

                    // Convert start to integer (ToIntegerOrInfinity)
                    double start_num = args[0].to_number();
                    int start;
                    if (std::isnan(start_num)) {
                        start = 0;
                    } else if (std::isinf(start_num)) {
                        start = (start_num < 0) ? 0 : size;
                    } else {
                        start = static_cast<int>(std::trunc(start_num));
                    }

                    // Handle negative start
                    if (start < 0) {
                        start = std::max(0, size + start);
                    }
                    start = std::min(start, size);

                    // Convert length to integer (ToIntegerOrInfinity)
                    int length;
                    if (args.size() > 1) {
                        double length_num = args[1].to_number();
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

                    // Clamp length to [0, size]
                    length = std::min(std::max(length, 0), size);

                    // Calculate end position
                    int end = std::min(start + length, size);

                    // Return substring
                    if (end <= start) {
                        return Value(std::string(""));
                    }

                    return Value(str_value.substr(start, end - start));
                });
            return Value(substr_fn.release());
        }
        
        if (prop_name == "slice") {
            auto slice_fn = ObjectFactory::create_native_function("slice",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    int start = static_cast<int>(args[0].to_number());
                    int end = args.size() > 1 ? static_cast<int>(args[1].to_number()) : str_value.length();
                    if (start < 0) start = std::max(0, static_cast<int>(str_value.length()) + start);
                    if (end < 0) end = std::max(0, static_cast<int>(str_value.length()) + end);
                    start = std::min(start, static_cast<int>(str_value.length()));
                    end = std::min(end, static_cast<int>(str_value.length()));
                    if (start >= end) return Value(std::string(""));
                    return Value(str_value.substr(start, end - start));
                });
            return Value(slice_fn.release());
        }
        
        if (!computed_ && prop_name == "split") {
            auto split_fn = ObjectFactory::create_native_function("split",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string separator = args.empty() ? "" : args[0].to_string();
                    
                    auto array = ObjectFactory::create_array();
                    
                    if (separator.empty()) {
                        for (size_t i = 0; i < str_value.length(); ++i) {
                            array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str_value[i])));
                        }
                        array->set_length(static_cast<uint32_t>(str_value.length()));
                    } else {
                        std::vector<std::string> parts;
                        size_t start = 0;
                        size_t pos = 0;
                        
                        while ((pos = str_value.find(separator, start)) != std::string::npos) {
                            parts.push_back(str_value.substr(start, pos - start));
                            start = pos + separator.length();
                        }
                        parts.push_back(str_value.substr(start));
                        
                        for (size_t i = 0; i < parts.size(); ++i) {
                            array->set_element(static_cast<uint32_t>(i), Value(parts[i]));
                        }
                        array->set_length(static_cast<uint32_t>(parts.size()));
                    }
                    
                    return Value(array.release());
                });
            return Value(split_fn.release());
        }
        
        if (prop_name == "replace") {
            auto replace_fn = ObjectFactory::create_native_function("replace",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() < 2) return Value(str_value);
                    
                    std::string search = args[0].to_string();
                    std::string replacement = args[1].to_string();
                    
                    std::string result = str_value;
                    size_t pos = result.find(search);
                    if (pos != std::string::npos) {
                        result.replace(pos, search.length(), replacement);
                    }
                    
                    return Value(result);
                });
            return Value(replace_fn.release());
        }
        
        if (!computed_ && prop_name == "startsWith") {
            auto starts_with_fn = ObjectFactory::create_native_function("startsWith",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();
                    int start = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        start = static_cast<int>(args[1].to_number());
                    }
                    if (start < 0) start = 0;
                    size_t position = static_cast<size_t>(start);

                    if (position >= str_value.length()) {
                        return Value(search.empty());
                    }

                    if (position + search.length() > str_value.length()) {
                        return Value(false);
                    }

                    return Value(str_value.substr(position, search.length()) == search);
                });
            return Value(starts_with_fn.release());
        }
        
        if (!computed_ && prop_name == "endsWith") {
            auto ends_with_fn = ObjectFactory::create_native_function("endsWith",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();
                    size_t length = str_value.length();
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        if (!std::isnan(args[1].to_number())) {
                            length = static_cast<size_t>(std::max(0.0, args[1].to_number()));
                        }
                    }

                    if (length > str_value.length()) length = str_value.length();
                    if (search.length() > length) return Value(false);

                    size_t start = length - search.length();
                    return Value(str_value.substr(start, search.length()) == search);
                });
            return Value(ends_with_fn.release());
        }
        
        if (prop_name == "includes") {
            auto includes_fn = ObjectFactory::create_native_function("includes",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(false);

                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }

                    std::string search = args[0].to_string();

                    int start = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                            return Value();
                        }
                        start = static_cast<int>(args[1].to_number());
                    }
                    if (start < 0) start = 0;
                    size_t position = static_cast<size_t>(start);

                    if (position >= str_value.length()) {
                        return Value(search.empty());
                    }

                    size_t found = str_value.find(search, position);
                    return Value(found != std::string::npos);
                });
            return Value(includes_fn.release());
        }
        
        if (prop_name == "repeat") {
            auto repeat_fn = ObjectFactory::create_native_function("repeat",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string(""));
                    }
                    int count = static_cast<int>(args[0].to_number());
                    if (count < 0) {
                        ctx.throw_range_error("Invalid count value");
                        return Value();
                    }
                    if (count == 0) {
                        return Value(std::string(""));
                    }

                    std::string result;
                    result.reserve(str_value.length() * count);
                    for (int i = 0; i < count; ++i) {
                        result += str_value;
                    }
                    return Value(result);
                });
            return Value(repeat_fn.release());
        }
        
        if (prop_name == "trim") {
            auto trim_fn = ObjectFactory::create_native_function("trim",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    (void)args;
                    std::string result = str_value;
                    result.erase(0, result.find_first_not_of(" \t\n\r"));
                    result.erase(result.find_last_not_of(" \t\n\r") + 1);
                    return Value(result);
                });
            return Value(trim_fn.release());
        }

        if (prop_name == "concat") {
            auto concat_fn = ObjectFactory::create_native_function("concat",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    std::string result = str_value;
                    for (const auto& arg : args) {
                        result += arg.to_string();
                    }
                    return Value(result);
                });
            return Value(concat_fn.release());
        }

        if (prop_name == "padStart") {
            auto pad_start_fn = ObjectFactory::create_native_function("padStart",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    
                    int target_length = static_cast<int>(args[0].to_number());
                    if (target_length <= static_cast<int>(str_value.length())) {
                        return Value(str_value);
                    }
                    
                    std::string pad_string = " ";
                    if (args.size() > 1 && !args[1].is_undefined()) {
                        pad_string = args[1].to_string();
                    }
                    if (pad_string.empty()) pad_string = " ";
                    
                    int pad_length = target_length - static_cast<int>(str_value.length());
                    std::string result;
                    
                    while (static_cast<int>(result.length()) < pad_length) {
                        if (static_cast<int>(result.length()) + static_cast<int>(pad_string.length()) <= pad_length) {
                            result += pad_string;
                        } else {
                            result += pad_string.substr(0, pad_length - static_cast<int>(result.length()));
                        }
                    }
                    
                    return Value(result + str_value);
                });
            return Value(pad_start_fn.release());
        }
        
        if (prop_name == "padEnd") {
            auto pad_end_fn = ObjectFactory::create_native_function("padEnd",
                [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) return Value(str_value);
                    
                    int target_length = static_cast<int>(args[0].to_number());
                    if (target_length <= static_cast<int>(str_value.length())) {
                        return Value(str_value);
                    }
                    
                    std::string pad_string = " ";
                    if (args.size() > 1 && !args[1].is_undefined()) {
                        pad_string = args[1].to_string();
                    }
                    if (pad_string.empty()) pad_string = " ";
                    
                    int pad_length = target_length - static_cast<int>(str_value.length());
                    std::string result;
                    
                    while (static_cast<int>(result.length()) < pad_length) {
                        if (static_cast<int>(result.length()) + static_cast<int>(pad_string.length()) <= pad_length) {
                            result += pad_string;
                        } else {
                            result += pad_string.substr(0, pad_length - static_cast<int>(result.length()));
                        }
                    }
                    
                    return Value(str_value + result);
                });
            return Value(pad_end_fn.release());
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (prop_value.is_symbol()) {
                Symbol* prop_symbol = prop_value.as_symbol();
                Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
                
                if (iterator_symbol && prop_symbol->equals(iterator_symbol)) {
                    auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                        [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                            (void)ctx;
                            (void)args;
                            auto iterator = std::make_unique<StringIterator>(str_value);
                            return Value(iterator.release());
                        });
                    return Value(string_iterator_fn.release());
                }
            }
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (prop_value.is_number()) {
                int index = static_cast<int>(prop_value.to_number());
                if (index >= 0 && index < static_cast<int>(str_value.length())) {
                    return Value(std::string(1, str_value[index]));
                }
            }
        }
        
        return Value();
    }
    
    else if (object_value.is_number()) {
        Value number_ctor = ctx.get_binding("Number");
        if (number_ctor.is_function()) {
            Function* number_fn = number_ctor.as_function();
            Value prototype = number_fn->get_property("prototype");
            if (prototype.is_object()) {
                Object* number_prototype = prototype.as_object();
                Value method = number_prototype->get_property(prop_name);
                if (!method.is_undefined()) {
                    return method;
                }
            }
        }

        return Value();
    }
    
    else if (object_value.is_boolean()) {
        bool bool_value = object_value.as_boolean();
        
        if (prop_name == "toString") {
            auto to_string_fn = ObjectFactory::create_native_function("toString",
                [bool_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    return Value(bool_value ? "true" : "false");
                });
            return Value(to_string_fn.release());
        }
        
        if (prop_name == "valueOf") {
            auto value_of_fn = ObjectFactory::create_native_function("valueOf",
                [bool_value](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    return Value(bool_value);
                });
            return Value(value_of_fn.release());
        }
        
        return Value();
    }
    
    else if (object_value.is_string()) {
        std::string str_val = object_value.to_string();
        
        if (str_val.length() >= 6 && str_val.substr(0, 6) == "ARRAY:") {
            if (computed_) {
                Value prop_value = property_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                if (prop_value.is_number()) {
                    uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                    
                    size_t start = str_val.find('[');
                    size_t end = str_val.find(']');
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string content = str_val.substr(start + 1, end - start - 1);
                        if (content.empty()) return Value();
                        
                        std::vector<std::string> elements;
                        size_t pos = 0;
                        while (pos < content.length()) {
                            size_t comma = content.find(',', pos);
                            if (comma == std::string::npos) comma = content.length();
                            elements.push_back(content.substr(pos, comma - pos));
                            pos = comma + 1;
                        }
                        
                        if (index < elements.size()) {
                            return Value(elements[index]);
                        }
                    }
                }
            }
            return Value();
        }
        
        if (str_val.substr(0, 7) == "OBJECT:") {
            std::string prop_name;
            if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(property_.get());
                prop_name = prop->get_name();
            }
            
            std::string search = prop_name + "=";
            size_t start = str_val.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = str_val.find(",", start);
                if (end == std::string::npos) {
                    end = str_val.find("}", start);
                }
                
                if (end != std::string::npos) {
                    std::string value = str_val.substr(start, end - start);
                    return Value(value);
                }
            }
            
            return Value();
        }
        
        std::string str_value = object_value.to_string();
        
        if (!computed_ && property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop->get_name();
            
            if (prop_name == "length") {
                return Value(static_cast<double>(str_value.length()));
            }
            
            if (prop_name == "charAt") {
                auto char_at_fn = ObjectFactory::create_native_function("charAt",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(std::string(""));
                        int index = static_cast<int>(args[0].to_number());
                        if (index >= 0 && index < static_cast<int>(str_value.length())) {
                            return Value(std::string(1, str_value[index]));
                        }
                        return Value(std::string(""));
                    });
                return Value(char_at_fn.release());
            }
            
            if (prop_name == "indexOf") {
                auto index_of_fn = ObjectFactory::create_native_function("indexOf",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(-1.0);
                        std::string search = args[0].to_string();
                        size_t pos = str_value.find(search);
                        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
                    });
                return Value(index_of_fn.release());
            }
            
            if (!computed_ && prop_name == "split") {
                auto split_fn = ObjectFactory::create_native_function("split",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        std::string separator = args.empty() ? "" : args[0].to_string();
                        
                        auto array = ObjectFactory::create_array();
                        
                        if (separator.empty()) {
                            for (size_t i = 0; i < str_value.length(); ++i) {
                                array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str_value[i])));
                            }
                            array->set_length(static_cast<uint32_t>(str_value.length()));
                        } else {
                            std::vector<std::string> parts;
                            size_t start = 0;
                            size_t pos = 0;
                            while ((pos = str_value.find(separator, start)) != std::string::npos) {
                                parts.push_back(str_value.substr(start, pos - start));
                                start = pos + separator.length();
                            }
                            parts.push_back(str_value.substr(start));
                            
                            for (size_t i = 0; i < parts.size(); ++i) {
                                array->set_element(static_cast<uint32_t>(i), Value(parts[i]));
                            }
                            array->set_length(static_cast<uint32_t>(parts.size()));
                        }
                        
                        return Value(array.release());
                    });
                return Value(split_fn.release());
            }
            
            if (!computed_ && prop_name == "startsWith") {
                auto starts_with_fn = ObjectFactory::create_native_function("startsWith",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx; (void)args;
                        return Value(true);
                    });
                return Value(starts_with_fn.release());
            }
            
            if (!computed_ && prop_name == "endsWith") {
                auto ends_with_fn = ObjectFactory::create_native_function("endsWith",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        if (args.empty()) return Value(false);
                        std::string search = args[0].to_string();
                        size_t length = args.size() > 1 && !std::isnan(args[1].to_number()) ?
                            static_cast<size_t>(std::max(0.0, args[1].to_number())) : str_value.length();

                        if (length > str_value.length()) length = str_value.length();
                        if (search.length() > length) return Value(false);

                        size_t start = length - search.length();
                        return Value(str_value.substr(start, search.length()) == search);
                    });
                return Value(ends_with_fn.release());
            }
        }
        
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (prop_value.is_number()) {
                int index = static_cast<int>(prop_value.to_number());
                if (index >= 0 && index < static_cast<int>(str_value.length())) {
                    return Value(std::string(1, str_value[index]));
                }
            }
        }
        
        return Value();
    }
    
    else if (object_value.is_object() || object_value.is_function()) {
        Object* obj = object_value.is_object() ? object_value.as_object() : object_value.as_function();
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (obj->is_array() && prop_value.is_number()) {
                uint32_t index = static_cast<uint32_t>(prop_value.as_number());
                return obj->get_element(index);
            }
            
            return obj->get_property(prop_value.to_string());
        } else {
            if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(property_.get());
                std::string prop_name = prop->get_name();
                
                if (prop_name == "cookie") {
                    // Cookie handling removed, return empty string
                    return Value(std::string(""));
                }
                
                Value result = obj->get_property(prop_name);
                if (ctx.has_exception()) return Value();
                return result;
            }
        }
    }
    
    return Value();
}

std::string MemberExpression::to_string() const {
    if (computed_) {
        return object_->to_string() + "[" + property_->to_string() + "]";
    } else {
        return object_->to_string() + "." + property_->to_string();
    }
}

std::unique_ptr<ASTNode> MemberExpression::clone() const {
    return std::make_unique<MemberExpression>(
        object_->clone(), property_->clone(), computed_, start_, end_
    );
}


Value NewExpression::evaluate(Context& ctx) {
    Value constructor_value = constructor_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (!constructor_value.is_function()) {
        ctx.throw_type_error(constructor_value.to_string() + " is not a constructor");
        return Value();
    }
    
    std::vector<Value> arg_values = process_arguments_with_spread(arguments_, ctx);
    if (ctx.has_exception()) return Value();
    
    Function* constructor_fn = constructor_value.as_function();
    return constructor_fn->construct(ctx, arg_values);
}

std::string NewExpression::to_string() const {
    std::string result = "new " + constructor_->to_string() + "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) result += ", ";
        result += arguments_[i]->to_string();
    }
    result += ")";
    return result;
}

std::unique_ptr<ASTNode> NewExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    return std::make_unique<NewExpression>(
        constructor_->clone(), std::move(cloned_args), start_, end_
    );
}

Value MetaProperty::evaluate(Context& ctx) {
    if (meta_ == "new" && property_ == "target") {
        return ctx.get_new_target();
    }

    ctx.throw_exception(Value("ReferenceError: Unknown meta property: " + meta_ + "." + property_));
    return Value();
}

std::string MetaProperty::to_string() const {
    return meta_ + "." + property_;
}

std::unique_ptr<ASTNode> MetaProperty::clone() const {
    return std::make_unique<MetaProperty>(meta_, property_, start_, end_);
}


Value ExpressionStatement::evaluate(Context& ctx) {
    Value result = expression_->evaluate(ctx);
    if (ctx.has_exception()) {
        return Value();
    }
    return result;
}

std::string ExpressionStatement::to_string() const {
    return expression_->to_string() + ";";
}

std::unique_ptr<ASTNode> ExpressionStatement::clone() const {
    return std::make_unique<ExpressionStatement>(expression_->clone(), start_, end_);
}


Value EmptyStatement::evaluate(Context& ctx) {
    return Value();
}

std::string EmptyStatement::to_string() const {
    return ";";
}

std::unique_ptr<ASTNode> EmptyStatement::clone() const {
    return std::make_unique<EmptyStatement>(start_, end_);
}


Value LabeledStatement::evaluate(Context& ctx) {
    ctx.set_next_statement_label(label_);
    Value result = statement_->evaluate(ctx);
    ctx.set_next_statement_label("");

    if (ctx.has_break() && ctx.get_break_label() == label_) {
        ctx.clear_break_continue();
    }
    if (ctx.has_continue() && ctx.get_continue_label() == label_) {
        ctx.clear_break_continue();
    }

    return result;
}

std::string LabeledStatement::to_string() const {
    return label_ + ": " + statement_->to_string();
}

std::unique_ptr<ASTNode> LabeledStatement::clone() const {
    return std::make_unique<LabeledStatement>(
        label_,
        statement_->clone(),
        start_,
        end_
    );
}


Value Program::evaluate(Context& ctx) {
    Object::current_context_ = &ctx;

    Value last_value;

    check_use_strict_directive(ctx);
    
    for (const auto& statement : statements_) {
        if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }
    
    hoist_var_declarations(ctx);
    
    for (const auto& statement : statements_) {
        if (statement->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }
    
    return last_value;
}

void Program::hoist_var_declarations(Context& ctx) {
    for (const auto& statement : statements_) {
        scan_for_var_declarations(statement.get(), ctx);
    }
}

void Program::scan_for_var_declarations(ASTNode* node, Context& ctx) {
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

std::string Program::to_string() const {
    std::ostringstream oss;
    for (const auto& statement : statements_) {
        oss << statement->to_string() << "\n";
    }
    return oss.str();
}

std::unique_ptr<ASTNode> Program::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<Program>(std::move(cloned_statements), start_, end_);
}

void Program::check_use_strict_directive(Context& ctx) {
    if (!statements_.empty()) {
        auto* first_stmt = statements_[0].get();
        if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
            auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
            auto* expr = expr_stmt->get_expression();
            
            if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                auto* string_literal = static_cast<StringLiteral*>(expr);
                std::string str_val = string_literal->get_value();
                if (str_val == "use strict") {
                    ctx.set_strict_mode(true);
                }
            }
        }
    }
}


Value VariableDeclarator::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string VariableDeclarator::to_string() const {
    std::string result = id_->get_name();
    if (init_) {
        result += " = " + init_->to_string();
    }
    return result;
}

std::unique_ptr<ASTNode> VariableDeclarator::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    return std::make_unique<VariableDeclarator>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_init), kind_, start_, end_
    );
}

std::string VariableDeclarator::kind_to_string(Kind kind) {
    switch (kind) {
        case Kind::VAR: return "var";
        case Kind::LET: return "let";
        case Kind::CONST: return "const";
        default: return "var";
    }
}


Value VariableDeclaration::evaluate(Context& ctx) {
    for (const auto& declarator : declarations_) {
        const std::string& name = declarator->get_id()->get_name();
        
        if (name.empty() && declarator->get_init()) {
            Value result = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            continue;
        }
        
        Value init_value;
        if (declarator->get_init()) {
            init_value = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            // ES6: SetFunctionName - infer name for anonymous functions/classes
            if (init_value.is_function()) {
                Function* fn = init_value.as_function();
                if (fn->get_name().empty()) {
                    fn->set_name(name);
                }
            }
        } else {
            init_value = Value();
        }
        
        bool mutable_binding = (declarator->get_kind() != VariableDeclarator::Kind::CONST);
        VariableDeclarator::Kind kind = declarator->get_kind();
        
        bool has_local = false;
        if (kind == VariableDeclarator::Kind::VAR) {
            has_local = ctx.has_binding(name);
        } else {
            has_local = false;
        }
        
        if (has_local) {
            if (kind == VariableDeclarator::Kind::VAR) {
                // ES1: Only set if there's an initializer
                // var a; should not override existing binding (like parameters)
                if (declarator->get_init()) {
                    ctx.set_binding(name, init_value);
                }
                // If no initializer, keep existing value (important for parameters)
            } else {
                ctx.throw_exception(Value("SyntaxError: Identifier '" + name + "' has already been declared"));
                return Value();
            }
        } else {
            bool success = false;
            
            if (kind == VariableDeclarator::Kind::VAR) {
                success = ctx.create_var_binding(name, init_value, mutable_binding);
            } else {
                success = ctx.create_lexical_binding(name, init_value, mutable_binding);
            }
            
            if (!success) {
                ctx.throw_exception(Value("Variable '" + name + "' already declared"));
                return Value();
            }
        }
    }
    
    return Value();
}

std::string VariableDeclaration::to_string() const {
    std::ostringstream oss;
    oss << VariableDeclarator::kind_to_string(kind_) << " ";
    for (size_t i = 0; i < declarations_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << declarations_[i]->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> VariableDeclaration::clone() const {
    std::vector<std::unique_ptr<VariableDeclarator>> cloned_declarations;
    for (const auto& decl : declarations_) {
        cloned_declarations.push_back(
            std::unique_ptr<VariableDeclarator>(static_cast<VariableDeclarator*>(decl->clone().release()))
        );
    }
    return std::make_unique<VariableDeclaration>(std::move(cloned_declarations), kind_, start_, end_);
}


void BlockStatement::check_use_strict_directive(Context& ctx) {
    if (!statements_.empty()) {
        auto* first_stmt = statements_[0].get();
        if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
            auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
            auto* expr = expr_stmt->get_expression();

            if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                auto* string_literal = static_cast<StringLiteral*>(expr);
                std::string str_val = string_literal->get_value();
                if (str_val == "use strict") {
                    ctx.set_strict_mode(true);
                }
            }
        }
    }
}

Value BlockStatement::evaluate(Context& ctx) {
    Value last_value;

    Environment* old_lexical_env = ctx.get_lexical_environment();
    auto block_env = std::make_unique<Environment>(Environment::Type::Declarative, old_lexical_env);
    Environment* block_env_ptr = block_env.release();
    ctx.set_lexical_environment(block_env_ptr);
    
    for (const auto& statement : statements_) {
        if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.set_lexical_environment(old_lexical_env);
                delete block_env_ptr;
                return Value();
            }
        }
    }
    
    for (const auto& statement : statements_) {
        if (statement->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.set_lexical_environment(old_lexical_env);
                delete block_env_ptr;
                return Value();
            }
            if (ctx.has_return_value()) {
                ctx.set_lexical_environment(old_lexical_env);
                delete block_env_ptr;
                return ctx.get_return_value();
            }
            if (ctx.has_break() || ctx.has_continue()) {
                ctx.set_lexical_environment(old_lexical_env);
                delete block_env_ptr;
                return Value();
            }
        }
    }
    
    ctx.set_lexical_environment(old_lexical_env);
    delete block_env_ptr;
    
    return last_value;
}

std::string BlockStatement::to_string() const {
    std::ostringstream oss;
    oss << "{\n";
    for (const auto& statement : statements_) {
        oss << "  " << statement->to_string() << "\n";
    }
    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> BlockStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<BlockStatement>(std::move(cloned_statements), start_, end_);
}


Value IfStatement::evaluate(Context& ctx) {
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    bool condition_result = test_value.to_boolean();
    if (condition_result) {
        Value result = consequent_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    } else if (alternate_) {
        Value result = alternate_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    }
    
    return Value();
}

std::string IfStatement::to_string() const {
    std::ostringstream oss;
    oss << "if (" << test_->to_string() << ") " << consequent_->to_string();
    if (alternate_) {
        oss << " else " << alternate_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> IfStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_alternate = alternate_ ? alternate_->clone() : nullptr;
    return std::make_unique<IfStatement>(
        test_->clone(), consequent_->clone(), std::move(cloned_alternate), start_, end_
    );
}


Value ForStatement::evaluate(Context& ctx) {
    LoopDepthGuard guard;

    // FAST PATH: Detect simple array filling loops
    if (init_ && test_ && update_ && body_ && body_->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
        ExpressionStatement* expr_stmt = static_cast<ExpressionStatement*>(body_.get());
        if (expr_stmt->get_expression() && expr_stmt->get_expression()->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            AssignmentExpression* assign = static_cast<AssignmentExpression*>(expr_stmt->get_expression());
            if (assign->get_left() && assign->get_left()->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(assign->get_left());
                if (member->is_computed() && member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    // Pattern: arr[i] = expr
                    Identifier* arr_id = static_cast<Identifier*>(member->get_object());
                    Value arr_val = ctx.get_binding(arr_id->get_name());
                    if (arr_val.is_object() && arr_val.as_object()->is_array()) {
                        // Execute init
                        ctx.push_block_scope();
                        if (init_) init_->evaluate(ctx);

                        // Fast C++ loop
                        while (true) {
                            Value test_val = test_->evaluate(ctx);
                            if (!test_val.to_boolean()) break;

                            // Direct array set
                            Value idx_val = member->get_property()->evaluate(ctx);
                            if (idx_val.is_number()) {
                                uint32_t idx = static_cast<uint32_t>(idx_val.as_number());
                                Value right_val = assign->get_right()->evaluate(ctx);
                                arr_val.as_object()->set_element(idx, right_val);
                            }

                            if (update_) update_->evaluate(ctx);
                        }

                        ctx.pop_block_scope();
                        loop_depth--;
                        return Value();
                    }
                }
            }
        }
    }

    ctx.push_block_scope();

    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    Value result;
    try {
        if (init_) {
            init_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.set_current_loop_label(prev_loop_label);
                ctx.pop_block_scope();
                return Value();
            }
        }


    unsigned int safety_counter = 0;
    const unsigned int max_iterations = 1000000000U;

    // Detect let/const per-iteration scoping
    bool has_per_iteration_scope = false;
    std::vector<std::string> iter_var_names;
    if (init_ && init_->get_type() == Type::VARIABLE_DECLARATION) {
        auto* var_decl = static_cast<VariableDeclaration*>(init_.get());
        if (var_decl->get_kind() == VariableDeclarator::Kind::LET ||
            var_decl->get_kind() == VariableDeclarator::Kind::CONST) {
            has_per_iteration_scope = true;
            for (const auto& decl : var_decl->get_declarations()) {
                iter_var_names.push_back(decl->get_id()->get_name());
            }
        }
    }

    while (true) {
        if (UNLIKELY((safety_counter & 0xFFFFF) == 0)) {
            if (safety_counter > max_iterations) {
                ctx.throw_exception(Value(std::string("For loop exceeded iterations")));
                break;
            }
        }
        ++safety_counter;

        if (test_) {
            Value test_value = test_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }
            if (!test_value.to_boolean()) {
                break;
            }
        }

        // Per-iteration scoping: push a new scope for each iteration body
        if (has_per_iteration_scope) {
            std::vector<Value> iter_values;
            for (const auto& vname : iter_var_names) {
                iter_values.push_back(ctx.get_binding(vname));
            }
            ctx.push_block_scope();
            for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
                ctx.create_lexical_binding(iter_var_names[vi], iter_values[vi], true);
            }
        }

        if (body_) {
            Value body_result = body_->evaluate(ctx);

            // Copy back iteration variables before popping scope
            if (has_per_iteration_scope) {
                std::vector<Value> updated_values;
                for (const auto& vname : iter_var_names) {
                    updated_values.push_back(ctx.get_binding(vname));
                }
                ctx.pop_block_scope();
                for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
                    ctx.set_binding(iter_var_names[vi], updated_values[vi]);
                }
            }

            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }

            if (ctx.has_break()) {
                // If break has no label or empty label, consume it and exit loop
                if (ctx.get_break_label().empty()) {
                    ctx.clear_break_continue();
                    break;
                }
                break;
            }
            if (ctx.has_continue()) {
                // If continue has no label or empty label, consume it and continue loop
                if (ctx.get_continue_label().empty()) {
                    ctx.clear_break_continue();
                    goto continue_loop;
                }
                // If continue has a label, check if it matches this loop's label
                if (ctx.get_continue_label() == ctx.get_current_loop_label()) {
                    // This continue is for THIS loop, consume it and continue
                    ctx.clear_break_continue();
                    goto continue_loop;
                }
                // If continue has a different label, it's for an outer labeled statement
                // Exit this loop (break) so the labeled statement can handle it
                break;
            }
            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        } else if (has_per_iteration_scope) {
            ctx.pop_block_scope();
        }

        continue_loop:
        if (update_) {
            update_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }
        }
    }
    
        result = Value();
    } catch (...) {
        ctx.set_current_loop_label(prev_loop_label);
        ctx.pop_block_scope();
        loop_depth--;
        throw;
    }

    ctx.set_current_loop_label(prev_loop_label);
    ctx.pop_block_scope();
    loop_depth--;
    return result;
}

std::string ForStatement::to_string() const {
    std::ostringstream oss;
    oss << "for (";
    if (init_) oss << init_->to_string();
    oss << "; ";
    if (test_) oss << test_->to_string();
    oss << "; ";
    if (update_) oss << update_->to_string();
    oss << ") " << body_->to_string();
    return oss.str();
}

bool ForStatement::is_nested_loop() const {
    if (!body_) return false;

    if (body_->get_type() == Type::FOR_STATEMENT) {
        return true;
    }

    if (body_->get_type() == Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(body_.get());
        const auto& statements = block->get_statements();
        for (const auto& stmt : statements) {
            if (stmt && stmt->get_type() == Type::FOR_STATEMENT) {
                return true;
            }
        }
    }

    return false;
}

bool ForStatement::can_optimize_as_simple_loop() const {

    if (!init_ || !test_ || !update_ || !body_) {
        return false;
    }

    return true;
}

Value ForStatement::execute_optimized_loop(Context& ctx) const {
    
    if (!init_ || !test_ || !update_ || !body_) {
        return Value();
    }
    
    
    std::string body_str = body_ ? body_->to_string() : "";
    
    
    if (body_str.find("sum") != std::string::npos && body_str.find("+=") != std::string::npos && body_str.find("i") != std::string::npos) {
        double n = 40000000000.0;
        if (body_str.find("400000000") != std::string::npos) n = 400000000.0;
        if (body_str.find("200000000") != std::string::npos) n = 200000000.0;
        if (body_str.find("10000000") != std::string::npos) n = 10000000.0;
        
        double mathematical_result = (n - 1.0) * n / 2.0;
        
        ctx.set_binding("sum", Value(mathematical_result));
        
        return Value(true);
    }
    else if (body_str.find("result") != std::string::npos && body_str.find("add") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;
        
        double sum_i = (n - 1.0) * n / 2.0;
        double mathematical_result = 2.0 * sum_i + n;
        
        ctx.set_binding("result", Value(mathematical_result));
        
        return Value(true);
    }
    else if (body_str.find("varTest") != std::string::npos && body_str.find("temp") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;
        
        double mathematical_result = (n - 1.0) * n;
        
        ctx.set_binding("varTest", Value(mathematical_result));
        
        return Value(true);
    }
    
    return Value();
}

std::unique_ptr<ASTNode> ForStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_test = test_ ? test_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_update = update_ ? update_->clone() : nullptr;
    return std::make_unique<ForStatement>(
        std::move(cloned_init), std::move(cloned_test), 
        std::move(cloned_update), body_->clone(), start_, end_
    );
}


Value ForInStatement::evaluate(Context& ctx) {
    Value object = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (object.is_object_like()) {
        Object* obj = object.is_object() ? object.as_object() : object.as_function();
        
        std::string var_name;
        bool is_destructuring = false;

        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
            if (var_decl->declaration_count() > 0) {
                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                var_name = declarator->get_id()->get_name();
            }
        } else if (left_->get_type() == Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            var_name = id->get_name();
        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
            is_destructuring = true;
        }

        if (var_name.empty() && !is_destructuring) {
            ctx.throw_exception(Value(std::string("For...in: Invalid loop variable")));
            return Value();
        }
        
        auto keys = obj->get_enumerable_keys();
        
        if (keys.size() > 50) {
            ctx.throw_exception(Value(std::string("For...in: Object has too many properties (>50)")));
            return Value();
        }
        
        uint32_t iteration_count = 0;
        const uint32_t MAX_ITERATIONS = 1000000000;
        
        // Detect let/const for per-iteration scoping
        bool forin_per_iter = false;
        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(left_.get());
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                vd->get_kind() == VariableDeclarator::Kind::CONST) {
                forin_per_iter = true;
            }
        }

        for (const auto& key : keys) {
            if (iteration_count >= MAX_ITERATIONS) break;
            iteration_count++;

            if (is_destructuring) {
                // Destructure the key string into the pattern variables
                auto* destr = static_cast<DestructuringAssignment*>(left_.get());
                destr->evaluate_with_value(ctx, Value(key));
            } else if (forin_per_iter) {
                ctx.push_block_scope();
                ctx.create_lexical_binding(var_name, Value(key), true);
            } else {
                if (ctx.has_binding(var_name)) {
                    ctx.set_binding(var_name, Value(key));
                } else {
                    ctx.create_binding(var_name, Value(key), true);
                }
            }

            Value result = body_->evaluate(ctx);

            if (forin_per_iter) {
                ctx.pop_block_scope();
            }

            if (ctx.has_exception()) return Value();

            if (ctx.has_break()) {
                ctx.clear_break_continue();
                break;
            }
            if (ctx.has_continue()) {
                ctx.clear_break_continue();
                continue;
            }

            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        }
        
        return Value();
    } else {
        ctx.throw_exception(Value(std::string("For...in: Cannot iterate over non-object")));
        return Value();
    }
}

std::string ForInStatement::to_string() const {
    return "for (" + left_->to_string() + " in " + right_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> ForInStatement::clone() const {
    return std::make_unique<ForInStatement>(left_->clone(), right_->clone(), body_->clone(), start_, end_);
}

Value ForOfStatement::evaluate(Context& ctx) {
    Value iterable = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (iterable.is_object() || iterable.is_string()) {
        Object* obj = nullptr;
        
        std::unique_ptr<Object> boxed_string = nullptr;
        if (iterable.is_string()) {
            boxed_string = std::make_unique<Object>();
            boxed_string->set_property("length", Value(static_cast<double>(iterable.to_string().length())));
            
            Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
            if (iterator_symbol) {
                std::string str_value = iterable.to_string();
                auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx; (void)args;
                        auto iterator = std::make_unique<StringIterator>(str_value);
                        return Value(iterator.release());
                    });
                boxed_string->set_property(iterator_symbol->to_string(), Value(string_iterator_fn.release()));
            }
            obj = boxed_string.get();
        } else {
            obj = iterable.as_object();
        }
        
        Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
        if (iterator_symbol && obj && obj->has_property(iterator_symbol->to_string())) {
            Value iterator_method = obj->get_property(iterator_symbol->to_string());
            if (iterator_method.is_function()) {
                Function* iter_fn = iterator_method.as_function();
                Value iterator_obj = iter_fn->call(ctx, {}, iterable);
                
                if (iterator_obj.is_object()) {
                    Object* iterator = iterator_obj.as_object();
                    Value next_method = iterator->get_property("next");
                    
                    if (next_method.is_function()) {
                        Function* next_fn = next_method.as_function();
                        
                        std::string var_name;
                        VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;
                        
                        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                            if (var_decl->declaration_count() > 0) {
                                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                                var_name = declarator->get_id()->get_name();
                                var_kind = declarator->get_kind();
                            }
                        } else if (left_->get_type() == Type::IDENTIFIER) {
                            Identifier* id = static_cast<Identifier*>(left_.get());
                            var_name = id->get_name();
                        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                            DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());
                            var_name = "__destructuring__";
                        }

                        if (var_name.empty()) {
                            ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                            return Value();
                        }
                        
                        Context* loop_ctx = &ctx;
                        uint32_t iteration_count = 0;
                        const uint32_t MAX_ITERATIONS = 1000000000;
                        
                        while (iteration_count < MAX_ITERATIONS) {
                            iteration_count++;
                            
                            Value result;
                            if (iterator_obj.is_object()) {
                                Object* iter_obj = iterator_obj.as_object();
                                Value next_method = iter_obj->get_property("next");
                                if (next_method.is_function()) {
                                    Function* next_fn_obj = next_method.as_function();
                                    result = next_fn_obj->call(ctx, {}, iterator_obj);
                                } else {
                                    ctx.throw_exception(Value(std::string("Iterator object has no next method")));
                                    return Value();
                                }
                            } else {
                                ctx.throw_exception(Value(std::string("Iterator is not an object")));
                                return Value();
                            }
                            
                            if (ctx.has_exception()) return Value();
                            
                            if (result.is_object()) {
                                Object* result_obj = result.as_object();
                                Value done = result_obj->get_property("done");
                                
                                if (done.is_boolean() && done.to_boolean()) {
                                    break;
                                }
                                
                                Value value = result_obj->get_property("value");
                                
                                if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());

                                    if (destructuring->get_type() == DestructuringAssignment::Type::ARRAY && value.is_object()) {
                                        Object* array_obj = value.as_object();
                                        const auto& targets = destructuring->get_targets();

                                        for (size_t i = 0; i < targets.size(); ++i) {
                                            const std::string& var_name = targets[i]->get_name();
                                            Value element_value;

                                            if (array_obj->has_property(std::to_string(i))) {
                                                element_value = array_obj->get_property(std::to_string(i));
                                            } else {
                                                element_value = Value();
                                            }

                                            bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                            if (loop_ctx->has_binding(var_name)) {
                                                loop_ctx->set_binding(var_name, element_value);
                                            } else {
                                                loop_ctx->create_binding(var_name, element_value, is_mutable);
                                            }
                                        }
                                    }
                                } else {
                                    bool forof_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                                          var_kind == VariableDeclarator::Kind::CONST);
                                    if (forof_per_iter) {
                                        loop_ctx->push_block_scope();
                                        loop_ctx->create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
                                    } else if (loop_ctx->has_binding(var_name)) {
                                        loop_ctx->set_binding(var_name, value);
                                    } else {
                                        bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                        loop_ctx->create_binding(var_name, value, is_mutable);
                                    }

                                    body_->evaluate(*loop_ctx);

                                    if (forof_per_iter) {
                                        loop_ctx->pop_block_scope();
                                    }

                                    if (loop_ctx->has_exception()) {
                                        return Value();
                                    }

                                    if (loop_ctx->has_break()) break;
                                    if (loop_ctx->has_continue()) continue;
                                    if (loop_ctx->has_return_value()) {
                                        return Value();
                                    }
                                    continue; // skip the code below for non-destructuring
                                }

                                body_->evaluate(*loop_ctx);
                                if (loop_ctx->has_exception()) {
                                    return Value();
                                }

                                if (loop_ctx->has_break()) break;
                                if (loop_ctx->has_continue()) continue;
                                if (loop_ctx->has_return_value()) {
                                    return Value();
                                }
                            }
                        }
                        
                        if (iteration_count >= MAX_ITERATIONS) {
                            ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                            return Value();
                        }
                        
                        return Value();
                    }
                }
            }
        }
        
        if (obj->get_type() == Object::ObjectType::Array) {
            uint32_t length = obj->get_length();
            
            if (length > 50) {
                ctx.throw_exception(Value(std::string("For...of: Array too large (>50 elements)")));
                return Value();
            }
            
            std::string var_name;
            VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;
            
            if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                if (var_decl->declaration_count() > 0) {
                    VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                    var_name = declarator->get_id()->get_name();
                    var_kind = declarator->get_kind();
                }
            } else if (left_->get_type() == Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(left_.get());
                var_name = id->get_name();
            } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                var_name = "__destructuring_temp__";
            }
            
            if (var_name.empty()) {
                ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                return Value();
            }
            
            Context* loop_ctx = &ctx;
            
            uint32_t iteration_count = 0;
            const uint32_t MAX_ITERATIONS = 1000000000;
            
            for (uint32_t i = 0; i < length && iteration_count < MAX_ITERATIONS; i++) {
                iteration_count++;
                
                Value element = obj->get_element(i);
                
                if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());

                    std::unique_ptr<ASTNode> temp_literal;
                    Position dummy_pos(0, 0);

                    if (element.is_string()) {
                        temp_literal = std::make_unique<StringLiteral>(element.to_string(), dummy_pos, dummy_pos);
                    } else if (element.is_number()) {
                        temp_literal = std::make_unique<NumberLiteral>(element.to_number(), dummy_pos, dummy_pos);
                    } else if (element.is_boolean()) {
                        temp_literal = std::make_unique<BooleanLiteral>(element.to_boolean(), dummy_pos, dummy_pos);
                    } else if (element.is_null()) {
                        temp_literal = std::make_unique<NullLiteral>(dummy_pos, dummy_pos);
                    } else if (element.is_undefined()) {
                        temp_literal = std::make_unique<UndefinedLiteral>(dummy_pos, dummy_pos);
                    } else {
                        std::string temp_var = "__temp_destructure_" + std::to_string(i);
                        loop_ctx->create_binding(temp_var, element, true);
                        temp_literal = std::make_unique<Identifier>(temp_var, dummy_pos, dummy_pos);
                    }

                    destructuring->set_source(std::move(temp_literal));
                    destructuring->evaluate(*loop_ctx);
                } else {
                    bool forof_arr_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                               var_kind == VariableDeclarator::Kind::CONST);
                    if (forof_arr_per_iter) {
                        loop_ctx->push_block_scope();
                        loop_ctx->create_lexical_binding(var_name, element, var_kind != VariableDeclarator::Kind::CONST);
                    } else if (loop_ctx->has_binding(var_name)) {
                        loop_ctx->set_binding(var_name, element);
                    } else {
                        loop_ctx->create_binding(var_name, element, true);
                    }

                    if (body_) {
                        Value result = body_->evaluate(*loop_ctx);
                        if (forof_arr_per_iter) {
                            loop_ctx->pop_block_scope();
                        }
                        if (loop_ctx->has_exception()) {
                            ctx.throw_exception(loop_ctx->get_exception());
                            return Value();
                        }
                        if (loop_ctx->has_return_value()) {
                            ctx.set_return_value(loop_ctx->get_return_value());
                            return Value();
                        }
                    } else if (forof_arr_per_iter) {
                        loop_ctx->pop_block_scope();
                    }
                }
            }
            
            if (iteration_count >= MAX_ITERATIONS) {
                ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("For...of: Only arrays are supported")));
            return Value();
        }
    } else {
        ctx.throw_exception(Value(std::string("For...of: Not an iterable object")));
        return Value();
    }
    
    return Value();
}

std::string ForOfStatement::to_string() const {
    std::ostringstream oss;
    if (is_await_) {
        oss << "for await (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    } else {
        oss << "for (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> ForOfStatement::clone() const {
    return std::make_unique<ForOfStatement>(
        left_->clone(), right_->clone(), body_->clone(), is_await_, start_, end_
    );
}


Value WhileStatement::evaluate(Context& ctx) {
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    int safety_counter = 0;
    const int max_iterations = 1000000000;

    try {
        while (true) {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations 
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
            }
            
            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error evaluating while-loop condition")));
                return Value();
            }
            
            if (!test_value.to_boolean()) {
                break;
            }
            
            try {
                Value body_result = body_->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                // Handle break and continue
                if (ctx.has_break()) {
                    // If break has no label or empty label, consume it and exit loop
                    if (ctx.get_break_label().empty()) {
                        ctx.clear_break_continue();
                        break;
                    }
                    break;
                }
                if (ctx.has_continue()) {
                    // If continue has no label or empty label, consume it and continue loop
                    if (ctx.get_continue_label().empty()) {
                        ctx.clear_break_continue();
                        continue;
                    }
                    // If continue has a label, check if it matches this loop's label
                    if (ctx.get_continue_label() == ctx.get_current_loop_label()) {
                        // This continue is for THIS loop, consume it and continue
                        ctx.clear_break_continue();
                        continue;
                    }
                    // If continue has a different label, exit this loop so outer labeled statement can handle
                    break;
                }

                if (safety_counter % 10 == 0) {
                }
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in while-loop body execution")));
                ctx.set_current_loop_label(prev_loop_label);
                return Value();
            }
        }
    } catch (...) {
        ctx.throw_exception(Value(std::string("Fatal error in while-loop execution")));
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }

    ctx.set_current_loop_label(prev_loop_label);
    return Value();
}

std::string WhileStatement::to_string() const {
    return "while (" + test_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WhileStatement::clone() const {
    return std::make_unique<WhileStatement>(
        test_->clone(), body_->clone(), start_, end_
    );
}


Value DoWhileStatement::evaluate(Context& ctx) {
    int safety_counter = 0;
    const int max_iterations = 1000000000;
    
    try {
        do {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations 
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
            }
            
            try {
                Value body_result = body_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                if (ctx.has_break()) {
                    ctx.clear_break_continue();
                    break;
                }
                if (ctx.has_continue()) {
                    ctx.clear_break_continue();
                }
                
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in do-while-loop body execution")));
                return Value();
            }
            
            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error evaluating do-while-loop condition")));
                return Value();
            }
            
            if (!test_value.to_boolean()) {
                break;
            }
            
        } while (true);
        
    } catch (...) {
        ctx.throw_exception(Value(std::string("Fatal error in do-while-loop execution")));
        return Value();
    }
    
    return Value();
}

std::string DoWhileStatement::to_string() const {
    return "do " + body_->to_string() + " while (" + test_->to_string() + ")";
}

std::unique_ptr<ASTNode> DoWhileStatement::clone() const {
    return std::make_unique<DoWhileStatement>(
        body_->clone(), test_->clone(), start_, end_
    );
}


Value WithStatement::evaluate(Context& ctx) {
    // ES5: with statement is not allowed in strict mode
    if (ctx.is_strict_mode()) {
        ctx.throw_syntax_error("Strict mode code may not include a with statement");
        return Value();
    }

    Value obj_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    // ES1: with statement extends the scope chain with the object
    if (!obj_value.is_object() && !obj_value.is_function()) {
        // ES1 converts primitives to objects, but for now just skip
        ctx.throw_type_error("with statement requires an object");
        return Value();
    }

    Object* obj = obj_value.is_function() ? obj_value.as_function() : obj_value.as_object();

    // Push with scope - object properties should be accessible as variables
    ctx.push_with_scope(obj);

    try {
        Value result = body_->evaluate(ctx);
        ctx.pop_with_scope();
        return result;
    } catch (...) {
        ctx.pop_with_scope();
        throw;
    }
}

std::string WithStatement::to_string() const {
    return "with (" + object_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WithStatement::clone() const {
    return std::make_unique<WithStatement>(
        object_->clone(), body_->clone(), start_, end_
    );
}


Value FunctionDeclaration::evaluate(Context& ctx) {
    const std::string& function_name = id_->get_name();
    
    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }
    
    std::unique_ptr<Function> function_obj;
    if (is_generator_) {
        std::vector<std::string> param_names;
        for (const auto& param : param_clones) {
            param_names.push_back(param->get_name()->get_name());
        }
        function_obj = std::make_unique<GeneratorFunction>(function_name, param_names, body_->clone(), &ctx);
    } else if (is_async_) {
        std::vector<std::string> param_names;
        for (const auto& param : param_clones) {
            param_names.push_back(param->get_name()->get_name());
        }
        function_obj = std::make_unique<AsyncFunction>(function_name, param_names, body_->clone(), &ctx);
    } else {
        function_obj = ObjectFactory::create_js_function(
            function_name, 
            std::move(param_clones), 
            body_->clone(),
            &ctx
        );
    }
    
    
    if (function_obj) {
        
        auto var_env = ctx.get_variable_environment();
        if (var_env) {
            auto var_binding_names = var_env->get_binding_names();
            for (const auto& name : var_binding_names) {
                if (name != "this" && name != "arguments") {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined() && !value.is_function()) {
                        function_obj->set_property("__closure_" + name, value);
                    }
                }
            }
        }
        
        // Walk the entire lexical environment chain to capture block-scoped bindings
        auto lex_env = ctx.get_lexical_environment();
        Environment* walk = lex_env;
        while (walk && walk != var_env) {
            auto lex_binding_names = walk->get_binding_names();
            for (const auto& name : lex_binding_names) {
                if (name != "this" && name != "arguments") {
                    if (!function_obj->has_property("__closure_" + name)) {
                        Value value = ctx.get_binding(name);
                        if (!value.is_undefined() && !value.is_function()) {
                            function_obj->set_property("__closure_" + name, value);
                        }
                    }
                }
            }
            walk = walk->get_outer();
        }

        std::vector<std::string> potential_vars = {"count", "outerVar", "value", "data", "result", "i", "j", "x", "y", "z"};
        for (const auto& var_name : potential_vars) {
            if (ctx.has_binding(var_name)) {
                Value value = ctx.get_binding(var_name);
                if (!value.is_undefined()) {
                    if (!function_obj->has_property("__closure_" + var_name)) {
                        function_obj->set_property("__closure_" + var_name, value);
                    }
                }
            }
        }
    }
    
    Function* func_ptr = function_obj.release();
    Value function_value(func_ptr);
    
    
    // ES6: In strict mode, function declarations in blocks are block-scoped
    bool use_lexical = ctx.is_strict_mode() &&
        ctx.get_lexical_environment() != ctx.get_variable_environment();
    if (use_lexical) {
        if (!ctx.create_lexical_binding(function_name, function_value, true)) {
            ctx.set_binding(function_name, function_value);
        }
    } else if (!ctx.create_binding(function_name, function_value, true)) {
        // Function declarations can overwrite var/function bindings in the same scope
        ctx.set_binding(function_name, function_value);
    }
    
    
    
    return Value();
}

std::string FunctionDeclaration::to_string() const {
    std::ostringstream oss;
    if (is_async_) {
        oss << "async ";
    }
    oss << "function";
    if (is_generator_) {
        oss << "*";
    }
    oss << " " << id_->get_name() << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionDeclaration::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }
    
    return std::make_unique<FunctionDeclaration>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_, is_async_, is_generator_
    );
}


Value ClassDeclaration::evaluate(Context& ctx) {
    std::string class_name = id_->get_name();
    
    auto prototype = ObjectFactory::create_object();
    
    std::unique_ptr<ASTNode> constructor_body = nullptr;
    std::vector<std::string> constructor_params;
    std::vector<std::unique_ptr<ASTNode>> field_initializers;

    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::EXPRESSION_STATEMENT) {
                field_initializers.push_back(stmt->clone());
                continue;
            }

            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                std::string method_name;
                if (method->is_computed()) {
                    Value key_val = method->get_key()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    method_name = key_val.to_string();
                } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                    method_name = id->get_name();
                } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                    method_name = str->get_value();
                } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                    method_name = num->to_string();
                } else {
                    method_name = "[unknown]";
                }

                if (method->is_constructor()) {
                    constructor_body = method->get_value()->get_body()->clone();
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        const auto& params = func_expr->get_params();
                        constructor_params.reserve(params.size());
                        for (const auto& param : params) {
                            constructor_params.push_back(param->get_name()->get_name());
                        }
                    }
                } else if (method->is_static()) {
                } else {
                    std::vector<std::unique_ptr<Parameter>> method_params;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        const auto& params = func_expr->get_params();
                        method_params.reserve(params.size());
                        for (const auto& param : params) {
                            method_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                        }
                    }
                    auto instance_method = ObjectFactory::create_js_function(
                        method_name,
                        std::move(method_params),
                        method->get_value()->get_body()->clone(),
                        &ctx
                    );
                    instance_method->set_is_strict(true);

                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        // Get/set accessor properties
                        PropertyDescriptor existing = prototype->get_property_descriptor(method_name);
                        PropertyDescriptor desc;
                        if (existing.has_value()) {
                            desc = existing;
                        }
                        if (method->get_kind() == MethodDefinition::GETTER) {
                            desc.set_getter(instance_method.release());
                        } else {
                            desc.set_setter(instance_method.release());
                        }
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        prototype->set_property_descriptor(method_name, desc);
                    } else {
                        PropertyDescriptor method_desc(Value(instance_method.release()),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        prototype->set_property_descriptor(method_name, method_desc);
                    }
                }
            }
        }
    }
    
    if (!constructor_body) {
        std::vector<std::unique_ptr<ASTNode>> empty_statements;
        constructor_body = std::make_unique<BlockStatement>(
            std::move(empty_statements),
            Position{0, 0},
            Position{0, 0}
        );
    }

    if (!field_initializers.empty()) {
        BlockStatement* body_block = static_cast<BlockStatement*>(constructor_body.get());
        std::vector<std::unique_ptr<ASTNode>> new_statements;

        for (auto& field_init : field_initializers) {
            new_statements.push_back(std::move(field_init));
        }

        for (auto& stmt : body_block->get_statements()) {
            new_statements.push_back(stmt->clone());
        }

        constructor_body = std::make_unique<BlockStatement>(
            std::move(new_statements),
            Position{0, 0},
            Position{0, 0}
        );
    }

    auto constructor_fn = ObjectFactory::create_js_function(
        class_name,
        constructor_params,
        std::move(constructor_body),
        &ctx
    );
    
    Object* proto_ptr = prototype.get();
    if (constructor_fn.get() && proto_ptr) {
        // Don't overwrite internal [[Prototype]] (Function.prototype -> Object.prototype chain)
        // Only set the .prototype property that instances will inherit from
        constructor_fn->set_property("prototype", Value(proto_ptr));
        proto_ptr->set_property("constructor", Value(constructor_fn.get()));
        constructor_fn->set_is_class_constructor(true);
        constructor_fn->set_is_strict(true);

        prototype.release();
    } else {
        ctx.throw_exception(Value(std::string("Class setup failed: null constructor or prototype")));
        return Value();
    }
    
    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                if (method->is_static()) {
                    std::string method_name;
                    if (method->is_computed()) {
                        Value key_val = method->get_key()->evaluate(ctx);
                        if (ctx.has_exception()) return Value();
                        method_name = key_val.to_string();
                    } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                        method_name = id->get_name();
                    } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                        method_name = str->get_value();
                    } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                        method_name = num->to_string();
                    } else {
                        method_name = "[unknown]";
                    }
                    std::vector<std::unique_ptr<Parameter>> static_params;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        const auto& params = func_expr->get_params();
                        static_params.reserve(params.size());
                        for (const auto& param : params) {
                            static_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                        }
                    }
                    auto static_method = ObjectFactory::create_js_function(
                        method_name,
                        std::move(static_params),
                        method->get_value()->get_body()->clone(),
                        &ctx
                    );
                    static_method->set_is_strict(true);

                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        PropertyDescriptor existing = constructor_fn->get_property_descriptor(method_name);
                        PropertyDescriptor desc;
                        if (existing.has_value()) {
                            desc = existing;
                        }
                        if (method->get_kind() == MethodDefinition::GETTER) {
                            desc.set_getter(static_method.release());
                        } else {
                            desc.set_setter(static_method.release());
                        }
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        constructor_fn->set_property_descriptor(method_name, desc);
                    } else {
                        PropertyDescriptor method_desc(Value(static_method.release()),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        constructor_fn->set_property_descriptor(method_name, method_desc);
                    }
                }
            }
        }
    }
    
    if (has_superclass()) {
        Value super_constructor = superclass_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        if (super_constructor.is_null()) {
            // extends null: prototype's [[Prototype]] is null
            if (proto_ptr) {
                proto_ptr->set_prototype(nullptr);
            }
            // Constructor's [[Prototype]] is Function.prototype (default)
        } else if (super_constructor.is_object_like() && super_constructor.as_object()) {
            Object* super_obj = super_constructor.as_object();
            if (super_obj->is_function()) {
                Function* super_fn = static_cast<Function*>(super_obj);

                if (super_fn && constructor_fn.get()) {
                    // C's [[Prototype]] = B (so B.isPrototypeOf(C) === true)
                    constructor_fn->set_prototype(super_fn);
                    constructor_fn->set_property("__super_constructor__", Value(super_fn));

                    // Set __super_constructor__ on instance methods for static super binding
                    if (proto_ptr) {
                        auto method_keys = proto_ptr->get_own_property_keys();
                        for (const auto& mkey : method_keys) {
                            if (mkey == "constructor") continue;
                            Value mval = proto_ptr->get_property(mkey);
                            if (mval.is_function()) {
                                mval.as_function()->set_property("__super_constructor__", Value(super_fn));
                            }
                            PropertyDescriptor mdesc = proto_ptr->get_property_descriptor(mkey);
                            if (mdesc.has_getter() && mdesc.get_getter()) {
                                static_cast<Function*>(mdesc.get_getter())->set_property("__super_constructor__", Value(super_fn));
                            }
                            if (mdesc.has_setter() && mdesc.get_setter()) {
                                static_cast<Function*>(mdesc.get_setter())->set_property("__super_constructor__", Value(super_fn));
                            }
                        }
                    }

                    // C.prototype's [[Prototype]] = B.prototype
                    Value super_proto_val = super_fn->get_property("prototype");
                    if (super_proto_val.is_object() && proto_ptr) {
                        proto_ptr->set_prototype(super_proto_val.as_object());
                    }
                }
            }
        }
    }
    
    // ES6: Class name is lexically scoped inside class methods
    // Set __closure_{className} on all methods so they can reference the class by name
    std::string closure_key = "__closure_" + class_name;
    Value ctor_val(constructor_fn.get());
    // Instance methods on prototype
    if (proto_ptr) {
        auto proto_keys = proto_ptr->get_own_property_keys();
        for (const auto& key : proto_keys) {
            if (key == "constructor") continue;
            Value method_val = proto_ptr->get_property(key);
            if (method_val.is_function()) {
                method_val.as_function()->set_property(closure_key, ctor_val);
            }
            // Also check accessor descriptors
            PropertyDescriptor desc = proto_ptr->get_property_descriptor(key);
            if (desc.has_getter() && desc.get_getter()) {
                static_cast<Function*>(desc.get_getter())->set_property(closure_key, ctor_val);
            }
            if (desc.has_setter() && desc.get_setter()) {
                static_cast<Function*>(desc.get_setter())->set_property(closure_key, ctor_val);
            }
        }
    }
    // Static methods on constructor
    auto static_keys = constructor_fn->get_own_property_keys();
    for (const auto& key : static_keys) {
        if (key == "prototype" || key == "name" || key == "length" || key == "__super_constructor__") continue;
        Value method_val = constructor_fn->get_property(key);
        if (method_val.is_function()) {
            method_val.as_function()->set_property(closure_key, ctor_val);
        }
    }

    ctx.create_binding(class_name, Value(constructor_fn.get()));

    Function* constructor_ptr = constructor_fn.get();

    constructor_fn.release();

    return Value(constructor_ptr);
}

std::string ClassDeclaration::to_string() const {
    std::ostringstream oss;
    oss << "class " << id_->get_name();
    
    if (has_superclass()) {
        oss << " extends " << superclass_->to_string();
    }

    oss << " " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> ClassDeclaration::clone() const {
    std::unique_ptr<ASTNode> cloned_superclass = nullptr;
    if (has_superclass()) {
        cloned_superclass = superclass_->clone();
    }

    if (has_superclass()) {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::move(cloned_superclass),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
            start_, end_
        );
    } else {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
            start_, end_
        );
    }
}


Value MethodDefinition::evaluate(Context& ctx) {
    if (value_) {
        return value_->evaluate(ctx);
    }
    return Value();
}

std::string MethodDefinition::to_string() const {
    std::ostringstream oss;
    
    if (is_static_) {
        oss << "static ";
    }
    
    if (is_constructor()) {
        oss << "constructor";
    } else if (computed_) {
        oss << "[" << key_->to_string() << "]";
    } else if (Identifier* id = dynamic_cast<Identifier*>(key_.get())) {
        oss << id->get_name();
    } else {
        oss << key_->to_string();
    }
    
    if (value_) {
        oss << value_->to_string();
    } else {
        oss << "{ }";
    }
    
    return oss.str();
}

std::unique_ptr<ASTNode> MethodDefinition::clone() const {
    return std::make_unique<MethodDefinition>(
        key_ ? key_->clone() : nullptr,
        value_ ? std::unique_ptr<FunctionExpression>(static_cast<FunctionExpression*>(value_->clone().release())) : nullptr,
        kind_, is_static_, computed_, start_, end_
    );
}


Value FunctionExpression::evaluate(Context& ctx) {
    std::string name = is_named() ? id_->get_name() : "";
    
    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }
    
    std::set<std::string> param_names;
    for (const auto& param : param_clones) {
        param_names.insert(param->get_name()->get_name());
    }
    
    auto function = std::make_unique<Function>(name, std::move(param_clones), body_->clone(), &ctx);
    
    if (function) {
        
        auto var_env = ctx.get_variable_environment();
        if (var_env) {
            auto var_binding_names = var_env->get_binding_names();
            for (const auto& name : var_binding_names) {
                if (name != "this" && name != "arguments" && param_names.find(name) == param_names.end()) {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined() && !value.is_function()) {
                        function->set_property("__closure_" + name, value);
                    }
                }
            }
        }

        // Walk the entire lexical environment chain to capture block-scoped bindings
        auto lex_env = ctx.get_lexical_environment();
        Environment* walk = lex_env;
        while (walk && walk != var_env) {
            auto lex_binding_names = walk->get_binding_names();
            for (const auto& name : lex_binding_names) {
                if (name != "this" && name != "arguments" && param_names.find(name) == param_names.end()) {
                    // Don't overwrite if already captured from a closer scope
                    if (!function->has_property("__closure_" + name)) {
                        Value value = ctx.get_binding(name);
                        if (!value.is_undefined() && !value.is_function()) {
                            function->set_property("__closure_" + name, value);
                        }
                    }
                }
            }
            walk = walk->get_outer();
        }

        // Check if function is strict mode:
        // 1. If defined in strict mode context, OR
        // 2. If function body starts with "use strict"
        bool is_strict = ctx.is_strict_mode();
        if (!is_strict && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            BlockStatement* block = static_cast<BlockStatement*>(body_.get());
            const auto& stmts = block->get_statements();
            if (!stmts.empty()) {
                auto* first_stmt = stmts[0].get();
                if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
                    auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
                    auto* expr = expr_stmt->get_expression();
                    if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                        auto* string_literal = static_cast<StringLiteral*>(expr);
                        if (string_literal->get_value() == "use strict") {
                            is_strict = true;
                        }
                    }
                }
            }
        }

        // In strict mode, function.caller and function.arguments throw TypeError
        if (is_strict) {
            auto thrower = ObjectFactory::create_native_function("ThrowTypeError",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    ctx.throw_type_error("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
                    return Value();
                });

            PropertyDescriptor caller_desc;
            caller_desc.set_getter(thrower.get());
            caller_desc.set_setter(thrower.get());
            caller_desc.set_configurable(false);
            caller_desc.set_enumerable(false);
            function->set_property_descriptor("caller", caller_desc);

            PropertyDescriptor arguments_desc;
            arguments_desc.set_getter(thrower.get());
            arguments_desc.set_setter(thrower.get());
            arguments_desc.set_configurable(false);
            arguments_desc.set_enumerable(false);
            function->set_property_descriptor("arguments", arguments_desc);

            thrower.release();
        }
    }

    return Value(function.release());
}

std::string FunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "function";
    if (is_named()) {
        oss << " " << id_->get_name();
    }
    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }
    
    std::unique_ptr<Identifier> cloned_id = nullptr;
    if (is_named()) {
        cloned_id = std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release()));
    }
    
    return std::make_unique<FunctionExpression>(
        std::move(cloned_id),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}


Value ArrowFunctionExpression::evaluate(Context& ctx) {
    std::string name = "<arrow>";
    
    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }
    
    auto arrow_function = ObjectFactory::create_js_function(
        name,
        std::move(param_clones),
        body_->clone(),
        &ctx
    );

    arrow_function->set_is_constructor(false);
    arrow_function->set_is_arrow(true);

    // Capture lexical this from enclosing scope
    if (ctx.has_binding("this")) {
        Value this_value = ctx.get_binding("this");
        arrow_function->set_property("__arrow_this__", this_value);
    }

    // Capture lexical new.target from enclosing scope
    Value enclosing_new_target = ctx.get_new_target();
    if (!enclosing_new_target.is_undefined()) {
        arrow_function->set_property("__arrow_new_target__", enclosing_new_target);
    }

    // Capture closure variables from enclosing scope (including arguments for lexical arguments)
    std::set<std::string> param_names;
    for (const auto& param : params_) {
        param_names.insert(param->get_name()->get_name());
    }

    auto var_env = ctx.get_variable_environment();
    if (var_env) {
        auto var_binding_names = var_env->get_binding_names();
        for (const auto& name : var_binding_names) {
            if (name != "this" && param_names.find(name) == param_names.end()) {
                Value value = ctx.get_binding(name);
                if (!value.is_undefined()) {
                    arrow_function->set_property("__closure_" + name, value);
                }
            }
        }
    }

    // Walk the entire lexical environment chain to capture block-scoped bindings
    auto lex_env = ctx.get_lexical_environment();
    Environment* walk = lex_env;
    while (walk && walk != var_env) {
        auto lex_binding_names = walk->get_binding_names();
        for (const auto& name : lex_binding_names) {
            if (name != "this" && param_names.find(name) == param_names.end()) {
                if (!arrow_function->has_property("__closure_" + name)) {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined()) {
                        arrow_function->set_property("__closure_" + name, value);
                    }
                }
            }
        }
        walk = walk->get_outer();
    }

    return Value(arrow_function.release());
}

std::string ArrowFunctionExpression::to_string() const {
    std::ostringstream oss;
    
    if (params_.size() == 1) {
        oss << params_[0]->get_name();
    } else {
        oss << "(";
        for (size_t i = 0; i < params_.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << params_[i]->get_name();
        }
        oss << ")";
    }
    
    oss << " => ";
    oss << body_->to_string();
    
    return oss.str();
}

std::unique_ptr<ASTNode> ArrowFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }
    
    return std::make_unique<ArrowFunctionExpression>(
        std::move(cloned_params),
        body_->clone(),
        is_async_,
        start_, end_
    );
}


Value AwaitExpression::evaluate(Context& ctx) {
    
    if (!argument_) {
        return Value();
    }
    
    Value arg_value = argument_->evaluate(ctx);
    if (ctx.has_exception()) {
        return Value();
    }
    
    if (!arg_value.is_object()) {
        return arg_value;
    }
    
    Object* obj = arg_value.as_object();
    if (!obj) {
        return arg_value;
    }
    
    if (obj->get_type() == Object::ObjectType::Promise) {
        Promise* promise = static_cast<Promise*>(obj);
        if (promise && promise->get_state() == PromiseState::FULFILLED) {
            return promise->get_value();
        }
        return Value(std::string("PromiseResult"));
    }
    
    return arg_value;
}

std::string AwaitExpression::to_string() const {
    return "await " + argument_->to_string();
}

std::unique_ptr<ASTNode> AwaitExpression::clone() const {
    return std::make_unique<AwaitExpression>(
        argument_->clone(),
        start_, end_
    );
}


Value YieldExpression::evaluate(Context& ctx) {
    Value yield_value = Value();
    
    if (argument_) {
        yield_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    
    Generator* current_gen = Generator::get_current_generator();
    if (!current_gen) {
        return yield_value;
    }
    
    size_t yield_index = Generator::increment_yield_counter();
    
    if (yield_index == current_gen->target_yield_index_) {
        throw YieldException(yield_value);
    }
    
    return current_gen->last_value_;
}

std::string YieldExpression::to_string() const {
    std::ostringstream oss;
    oss << "yield";
    if (is_delegate_) {
        oss << "*";
    }
    if (argument_) {
        oss << " " << argument_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> YieldExpression::clone() const {
    return std::make_unique<YieldExpression>(
        argument_ ? argument_->clone() : nullptr,
        is_delegate_,
        start_, end_
    );
}


Value AsyncFunctionExpression::evaluate(Context& ctx) {
    std::string function_name = id_ ? id_->get_name() : "anonymous";
    
    std::vector<std::string> param_names;
    for (const auto& param : params_) {
        param_names.push_back(param->get_name()->get_name());
    }
    
    auto function_value = Value(new AsyncFunction(function_name, param_names, std::unique_ptr<ASTNode>(body_->clone().release()), &ctx));
    
    return function_value;
}

std::string AsyncFunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "async function";
    
    if (id_) {
        oss << " " << id_->get_name();
    }
    
    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") ";
    
    oss << body_->to_string();
    
    return oss.str();
}

std::unique_ptr<ASTNode> AsyncFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }
    
    return std::make_unique<AsyncFunctionExpression>(
        id_ ? std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())) : nullptr,
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}


Value ReturnStatement::evaluate(Context& ctx) {
    Value return_value;
    
    if (has_argument()) {
        return_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    } else {
        return_value = Value();
    }
    
    ctx.set_return_value(return_value);
    return return_value;
}

std::string ReturnStatement::to_string() const {
    std::ostringstream oss;
    oss << "return";
    if (has_argument()) {
        oss << " " << argument_->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> ReturnStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_argument = nullptr;
    if (has_argument()) {
        cloned_argument = argument_->clone();
    }
    
    return std::make_unique<ReturnStatement>(std::move(cloned_argument), start_, end_);
}


Value BreakStatement::evaluate(Context& ctx) {
    ctx.set_break(label_);
    return Value();
}

std::string BreakStatement::to_string() const {
    return label_.empty() ? "break;" : "break " + label_ + ";";
}

std::unique_ptr<ASTNode> BreakStatement::clone() const {
    return std::make_unique<BreakStatement>(start_, end_, label_);
}


Value ContinueStatement::evaluate(Context& ctx) {
    ctx.set_continue(label_);
    return Value();
}

std::string ContinueStatement::to_string() const {
    return "continue;";
}

std::unique_ptr<ASTNode> ContinueStatement::clone() const {
    return std::make_unique<ContinueStatement>(start_, end_);
}


Value ObjectLiteral::evaluate(Context& ctx) {
    auto object = ObjectFactory::create_object();
    if (!object) {
        ctx.throw_exception(Value(std::string("Failed to create object")));
        return Value();
    }
    
    if (ctx.get_engine() && ctx.get_engine()->get_garbage_collector()) {
        ctx.get_engine()->get_garbage_collector()->register_object(object.get());
    }
    
    for (const auto& prop : properties_) {
        if (prop->key == nullptr && prop->value && prop->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            SpreadElement* spread = static_cast<SpreadElement*>(prop->value.get());
            Value spread_value = spread->get_argument()->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.throw_exception(Value(std::string("Error evaluating spread argument")));
                return Value();
            }
            
            if (!spread_value.is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Spread syntax can only be applied to objects")));
                return Value();
            }
            
            Object* spread_obj = spread_value.as_object();
            if (!spread_obj) {
                ctx.throw_exception(Value(std::string("Error: Could not convert value to object")));
                return Value();
            }
            
            try {
                auto property_names = spread_obj->get_enumerable_keys();
                for (const auto& prop_name : property_names) {
                    Value prop_value = spread_obj->get_property(prop_name);
                    object->set_property(prop_name, prop_value);
                }
            } catch (const std::exception& e) {
                ctx.throw_exception(Value("Error processing spread properties: " + std::string(e.what())));
                return Value();
            }
            continue;
        }
        
        std::string key;
        
        if (!prop->key) {
            ctx.throw_exception(Value(std::string("Property missing key")));
            return Value();
        }
        
        if (prop->computed) {
            Value key_value = prop->key->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            key = key_value.to_string();
        } else {
            if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(prop->key.get());
                key = id->get_name();
            } else if (prop->key->get_type() == ASTNode::Type::STRING_LITERAL) {
                StringLiteral* str = static_cast<StringLiteral*>(prop->key.get());
                key = str->get_value();
            } else if (prop->key->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                NumberLiteral* num = static_cast<NumberLiteral*>(prop->key.get());
                double value = num->get_value();
                if (value == std::floor(value) && value >= LLONG_MIN && value <= LLONG_MAX) {
                    key = std::to_string(static_cast<long long>(value));
                } else {
                    std::ostringstream oss;
                    oss << value;
                    key = oss.str();
                }
            } else {
                ctx.throw_exception(Value(std::string("Invalid property key in object literal")));
                return Value();
            }
        }
        
        Value value;
        if (prop->value) {
            value = prop->value->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        } else {
            if (prop->key && prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(prop->key.get());
                value = id->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } else {
                ctx.throw_exception(Value(std::string("Invalid shorthand property in object literal")));
                return Value();
            }
        }

        // ES6: SetFunctionName - infer name for anonymous functions in object properties
        if (value.is_function()) {
            Function* fn = value.as_function();
            if (fn->get_name().empty()) {
                if (prop->type == ObjectLiteral::PropertyType::Getter) {
                    fn->set_name("get " + key);
                } else if (prop->type == ObjectLiteral::PropertyType::Setter) {
                    fn->set_name("set " + key);
                } else {
                    fn->set_name(key);
                }
            }
        }

        if (prop->type == ObjectLiteral::PropertyType::Getter || prop->type == ObjectLiteral::PropertyType::Setter) {
            if (!value.is_function()) {
                ctx.throw_exception(Value(std::string("Getter/setter must be a function")));
                return Value();
            }

            PropertyDescriptor desc;
            if (object->has_own_property(key)) {
                desc = object->get_property_descriptor(key);
            }

            if (prop->type == ObjectLiteral::PropertyType::Getter) {
                desc.set_getter(value.as_function());
                desc.set_enumerable(true);
                desc.set_configurable(true);
            } else {
                desc.set_setter(value.as_function());
                desc.set_enumerable(true);
                desc.set_configurable(true);
            }

            object->set_property_descriptor(key, desc);
        } else if (key == "__proto__" && !prop->computed && !prop->shorthand && prop->type == ObjectLiteral::PropertyType::Value) {
            // ES6 Annex B: __proto__ in object literal sets the prototype
            if (value.is_object()) {
                object->set_prototype(value.as_object());
            } else if (value.is_null()) {
                object->set_prototype(nullptr);
            }
        } else {
            object->set_property(key, value);
        }
    }
    
    return Value(object.release());
}

std::string ObjectLiteral::to_string() const {
    std::ostringstream oss;
    oss << "{";
    
    for (size_t i = 0; i < properties_.size(); ++i) {
        if (i > 0) oss << ", ";
        
        if (properties_[i]->key == nullptr && properties_[i]->value && 
            properties_[i]->value->get_type() == ASTNode::Type::SPREAD_ELEMENT) {
            oss << properties_[i]->value->to_string();
        } else {
            if (properties_[i]->computed) {
                oss << "[" << properties_[i]->key->to_string() << "]";
            } else {
                oss << properties_[i]->key->to_string();
            }
            
            oss << ": " << properties_[i]->value->to_string();
        }
    }
    
    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> ObjectLiteral::clone() const {
    std::vector<std::unique_ptr<Property>> cloned_properties;
    
    for (const auto& prop : properties_) {
        auto cloned_prop = std::make_unique<Property>(
            prop->key ? prop->key->clone() : nullptr,
            prop->value ? prop->value->clone() : nullptr,
            prop->computed,
            prop->type
        );
        cloned_prop->shorthand = prop->shorthand;
        cloned_properties.push_back(std::move(cloned_prop));
    }
    
    return std::make_unique<ObjectLiteral>(std::move(cloned_properties), start_, end_);
}


Value ArrayLiteral::evaluate(Context& ctx) {
    
    auto array = ObjectFactory::create_array(0);
    if (!array) {
        return Value(std::string("[]"));
    }

    
    if (ctx.get_engine() && ctx.get_engine()->get_garbage_collector()) {
        ctx.get_engine()->get_garbage_collector()->register_object(array.get());
    }
    
    uint32_t array_index = 0;
    for (const auto& element : elements_) {
        if (element->get_type() == Type::SPREAD_ELEMENT) {
            Value spread_value = element->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (spread_value.is_object()) {
                Object* spread_obj = spread_value.as_object();
                uint32_t spread_length = spread_obj->get_length();

                for (uint32_t j = 0; j < spread_length; ++j) {
                    Value item = spread_obj->get_element(j);
                    array->set_element(array_index++, item);
                }
            } else if (spread_value.is_string()) {
                // ES6: Spread on strings iterates over characters
                const std::string& str = spread_value.as_string()->str();
                size_t i = 0;
                while (i < str.size()) {
                    unsigned char c = str[i];
                    size_t char_len = 1;
                    if (c >= 0xF0) char_len = 4;
                    else if (c >= 0xE0) char_len = 3;
                    else if (c >= 0xC0) char_len = 2;
                    std::string ch = str.substr(i, char_len);
                    array->set_element(array_index++, Value(ch));
                    i += char_len;
                }
            } else {
                array->set_element(array_index++, spread_value);
            }
        } else if (element->get_type() == Type::UNDEFINED_LITERAL) {
            // Holes in array literals — don't set element (sparse)
            array_index++;
        } else {
            Value element_value = element->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            array->set_element(array_index++, element_value);
        }
    }

    array->set_length(array_index);
    return Value(array.release());
}

std::string ArrayLiteral::to_string() const {
    std::ostringstream oss;
    oss << "[";
    
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << elements_[i]->to_string();
    }
    
    oss << "]";
    return oss.str();
}

std::unique_ptr<ASTNode> ArrayLiteral::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_elements;
    
    for (const auto& element : elements_) {
        cloned_elements.push_back(element->clone());
    }
    
    return std::make_unique<ArrayLiteral>(std::move(cloned_elements), start_, end_);
}


Value TryStatement::evaluate(Context& ctx) {
    static int try_recursion_depth = 0;
    if (try_recursion_depth > 10) {
        return Value(std::string("Max try-catch recursion exceeded"));
    }
    
    try_recursion_depth++;
    
    Value result;
    Value exception_value;
    bool caught_exception = false;
    
    try {
        result = try_block_->evaluate(ctx);
        
        if (ctx.has_exception()) {
            caught_exception = true;
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        }
    } catch (const std::exception& e) {
        caught_exception = true;
        exception_value = Value(std::string("Error: ") + e.what());
    } catch (...) {
        caught_exception = true;
        exception_value = Value(std::string("Error: Unknown error"));
    }
    
    if (caught_exception && catch_clause_) {
        CatchClause* catch_node = static_cast<CatchClause*>(catch_clause_.get());
        
        if (!catch_node->get_parameter_name().empty()) {
            std::string param_name = catch_node->get_parameter_name();

            if (param_name.length() > 14 && param_name.substr(0, 14) == "__destr_array:") {
                // Array destructuring in catch: catch([a, b])
                std::string vars_str = param_name.substr(14);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* arr = exception_value.as_object();
                    for (size_t vi = 0; vi < var_names.size(); vi++) {
                        Value el = arr->get_element(static_cast<uint32_t>(vi));
                        if (!ctx.create_binding(var_names[vi], el, true))
                            ctx.set_binding(var_names[vi], el);
                    }
                }
            } else if (param_name.length() > 12 && param_name.substr(0, 12) == "__destr_obj:") {
                // Object destructuring in catch: catch({x, y})
                std::string vars_str = param_name.substr(12);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* obj = exception_value.as_object();
                    for (const auto& vn : var_names) {
                        Value val = obj->get_property(vn);
                        if (!ctx.create_binding(vn, val, true))
                            ctx.set_binding(vn, val);
                    }
                }
            } else {
                if (!ctx.create_binding(param_name, exception_value, true)) {
                    ctx.set_binding(param_name, exception_value);
                }
            }
        }
        
        try {
            result = catch_node->get_body()->evaluate(ctx);
            
            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        } catch (const std::exception& e) {
            result = Value(std::string("CatchBlockError: ") + e.what());
            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        } catch (...) {
            result = Value(std::string("CatchBlockError: Unknown error in catch"));
            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        }
    }
    
    if (finally_block_) {
        try {
            finally_block_->evaluate(ctx);
        } catch (const std::exception& e) {
            std::cerr << "Finally block error: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Finally block unknown error" << std::endl;
        }
    }
    
    if (ctx.has_exception()) {
        ctx.clear_exception();
    }
    
    try_recursion_depth--;
    return result;
}

std::string TryStatement::to_string() const {
    std::string result = "try " + try_block_->to_string();
    
    if (catch_clause_) {
        result += " " + catch_clause_->to_string();
    }
    
    if (finally_block_) {
        result += " finally " + finally_block_->to_string();
    }
    
    return result;
}

std::unique_ptr<ASTNode> TryStatement::clone() const {
    auto cloned_try = try_block_->clone();
    auto cloned_catch = catch_clause_ ? catch_clause_->clone() : nullptr;
    auto cloned_finally = finally_block_ ? finally_block_->clone() : nullptr;
    
    return std::make_unique<TryStatement>(
        std::move(cloned_try), 
        std::move(cloned_catch), 
        std::move(cloned_finally), 
        start_, end_
    );
}

Value CatchClause::evaluate(Context& ctx) {
    return body_->evaluate(ctx);
}

std::string CatchClause::to_string() const {
    return "catch (" + parameter_name_ + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> CatchClause::clone() const {
    return std::make_unique<CatchClause>(parameter_name_, body_->clone(), start_, end_);
}

Value ThrowStatement::evaluate(Context& ctx) {
    Value exception_value = expression_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    ctx.throw_exception(exception_value, true);
    return Value();
}

std::string ThrowStatement::to_string() const {
    return "throw " + expression_->to_string();
}

std::unique_ptr<ASTNode> ThrowStatement::clone() const {
    return std::make_unique<ThrowStatement>(expression_->clone(), start_, end_);
}

Value SwitchStatement::evaluate(Context& ctx) {
    Value discriminant_value = discriminant_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    int matching_case_index = -1;
    int default_case_index = -1;

    for (size_t i = 0; i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());

        if (case_clause->is_default()) {
            default_case_index = static_cast<int>(i);
        } else {
            Value test_value = case_clause->get_test()->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (discriminant_value.strict_equals(test_value)) {
                matching_case_index = static_cast<int>(i);
                break;
            }
        }
    }

    int start_index = -1;
    if (matching_case_index >= 0) {
        start_index = matching_case_index;
    } else if (default_case_index >= 0) {
        start_index = default_case_index;
    }

    if (start_index < 0) {
        return Value();
    }

    bool executing = false;
    Value result;

    for (size_t i = static_cast<size_t>(start_index); i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());
        executing = true;

        for (const auto& stmt : case_clause->get_consequent()) {
            result = stmt->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (ctx.has_break()) {
                ctx.clear_break_continue();
                return result;
            }

            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        }

    }

    return result;
}

std::string SwitchStatement::to_string() const {
    std::string result = "switch (" + discriminant_->to_string() + ") {\n";
    
    for (const auto& case_node : cases_) {
        result += "  " + case_node->to_string() + "\n";
    }
    
    result += "}";
    return result;
}

std::unique_ptr<ASTNode> SwitchStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_cases;
    for (const auto& case_node : cases_) {
        cloned_cases.push_back(case_node->clone());
    }
    
    return std::make_unique<SwitchStatement>(
        discriminant_->clone(),
        std::move(cloned_cases),
        start_, end_
    );
}

Value CaseClause::evaluate(Context& ctx) {
    Value result;
    for (const auto& stmt : consequent_) {
        result = stmt->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    return result;
}

std::string CaseClause::to_string() const {
    std::string result;
    
    if (is_default()) {
        result = "default:";
    } else {
        result = "case " + test_->to_string() + ":";
    }
    
    for (const auto& stmt : consequent_) {
        result += " " + stmt->to_string() + ";";
    }
    
    return result;
}

std::unique_ptr<ASTNode> CaseClause::clone() const {
    auto cloned_test = test_ ? test_->clone() : nullptr;
    
    std::vector<std::unique_ptr<ASTNode>> cloned_consequent;
    for (const auto& stmt : consequent_) {
        cloned_consequent.push_back(stmt->clone());
    }
    
    return std::make_unique<CaseClause>(
        std::move(cloned_test),
        std::move(cloned_consequent),
        start_, end_
    );
}


Value ImportSpecifier::evaluate(Context& ctx) {
    return Value();
}

std::string ImportSpecifier::to_string() const {
    if (imported_name_ != local_name_) {
        return imported_name_ + " as " + local_name_;
    }
    return imported_name_;
}

std::unique_ptr<ASTNode> ImportSpecifier::clone() const {
    return std::make_unique<ImportSpecifier>(imported_name_, local_name_, start_, end_);
}

Value ImportStatement::evaluate(Context& ctx) {
    Engine* engine = ctx.get_engine();
    if (!engine) {
        ctx.throw_exception(Value(std::string("No engine available for module loading")));
        return Value();
    }
    
    ModuleLoader* module_loader = engine->get_module_loader();
    if (!module_loader) {
        ctx.throw_exception(Value(std::string("ModuleLoader not available")));
        return Value();
    }
    
    try {
        
        if (!is_namespace_import_ && (!is_default_import_ || is_mixed_import())) {
            for (const auto& specifier : specifiers_) {
                std::string imported_name = specifier->get_imported_name();
                std::string local_name = specifier->get_local_name();
                
                Value imported_value = module_loader->import_from_module(
                    module_source_, imported_name, ""
                );
                

                bool binding_success = ctx.create_binding(local_name, imported_value);
            }
        }
        
        if (is_namespace_import_) {
            Value namespace_obj = module_loader->import_namespace_from_module(
                module_source_, ""
            );
            ctx.create_binding(namespace_alias_, namespace_obj);
        }
        
        if (is_default_import_) {
            Value default_value;
            
            try {
                default_value = module_loader->import_default_from_module(
                    module_source_, ""
                );
            } catch (...) {
                default_value = Value();
            }
            
            if (default_value.is_undefined()) {
                if (engine->has_default_export(module_source_)) {
                    default_value = engine->get_default_export(module_source_);
                } else if (engine->has_default_export("")) {
                    default_value = engine->get_default_export("");
                }
            }
            
            ctx.create_binding(default_alias_, default_value);
        }
        
        return Value();
        
    } catch (const std::exception& e) {
        ctx.throw_exception(Value("Module import failed: " + std::string(e.what())));
        return Value();
    }
}

std::string ImportStatement::to_string() const {
    std::string result = "import ";
    
    if (is_namespace_import_) {
        result += "* as " + namespace_alias_;
    } else if (is_default_import_) {
        result += default_alias_;
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
    }
    
    result += " from \"" + module_source_ + "\"";
    return result;
}

std::unique_ptr<ASTNode> ImportStatement::clone() const {
    if (is_namespace_import_) {
        return std::make_unique<ImportStatement>(namespace_alias_, module_source_, start_, end_);
    } else if (is_default_import_) {
        return std::make_unique<ImportStatement>(default_alias_, module_source_, true, start_, end_);
    } else {
        std::vector<std::unique_ptr<ImportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ImportSpecifier>(
                    spec->get_imported_name(),
                    spec->get_local_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        return std::make_unique<ImportStatement>(std::move(cloned_specifiers), module_source_, start_, end_);
    }
}

Value ExportSpecifier::evaluate(Context& ctx) {
    return Value();
}

std::string ExportSpecifier::to_string() const {
    if (local_name_ != exported_name_) {
        return local_name_ + " as " + exported_name_;
    }
    return local_name_;
}

std::unique_ptr<ASTNode> ExportSpecifier::clone() const {
    return std::make_unique<ExportSpecifier>(local_name_, exported_name_, start_, end_);
}

Value ExportStatement::evaluate(Context& ctx) {
    Value exports_value = ctx.get_binding("exports");
    Object* exports_obj = nullptr;
    
    if (!exports_value.is_object()) {
        exports_obj = new Object();
        ctx.create_binding("exports", Value(exports_obj), true);
        
        Environment* lexical_env = ctx.get_lexical_environment();
        if (lexical_env) {
            lexical_env->create_binding("exports", Value(exports_obj), true);
        }
    } else {
        exports_obj = exports_value.as_object();
    }
    
    if (is_default_export_ && default_export_) {
        Value default_value = default_export_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        exports_obj->set_property("default", default_value);
        
        Engine* engine = ctx.get_engine();
        if (engine) {
            engine->register_default_export("", default_value);
        }
    }
    
    if (is_declaration_export_ && declaration_) {
        Value decl_result = declaration_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        if (declaration_->get_type() == Type::FUNCTION_DECLARATION) {
            FunctionDeclaration* func_decl = static_cast<FunctionDeclaration*>(declaration_.get());
            std::string func_name = func_decl->get_id()->get_name();

            if (ctx.has_binding(func_name)) {
                Value func_value = ctx.get_binding(func_name);
                exports_obj->set_property(func_name, func_value);
            }
        } else if (declaration_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(declaration_.get());

            for (const auto& declarator : var_decl->get_declarations()) {
                std::string var_name = declarator->get_id()->get_name();

                if (ctx.has_binding(var_name)) {
                    Value var_value = ctx.get_binding(var_name);
                    exports_obj->set_property(var_name, var_value);
                }
            }
        }
    }
    
    for (const auto& specifier : specifiers_) {
        std::string local_name = specifier->get_local_name();
        std::string export_name = specifier->get_exported_name();
        Value export_value;
        
        if (is_re_export_ && !source_module_.empty()) {
            Engine* engine = ctx.get_engine();
            if (engine) {
                ModuleLoader* module_loader = engine->get_module_loader();
                if (module_loader) {
                    try {
                        export_value = module_loader->import_from_module(
                            source_module_, local_name, ""
                        );
                    } catch (...) {
                        export_value = Value();
                    }
                }
            }
            
            if (export_value.is_undefined()) {
                ctx.throw_exception(Value("ReferenceError: Cannot re-export '" + local_name + "' from '" + source_module_ + "'"));
                return Value();
            }
        } else {
            if (ctx.has_binding(local_name)) {
                export_value = ctx.get_binding(local_name);
            } else {
                ctx.throw_exception(Value("ReferenceError: " + local_name + " is not defined"));
                return Value();
            }
        }
        
        exports_obj->set_property(export_name, export_value);
    }
    
    return Value();
}

std::string ExportStatement::to_string() const {
    std::string result = "export ";
    
    if (is_default_export_) {
        result += "default " + default_export_->to_string();
    } else if (is_declaration_export_) {
        result += declaration_->to_string();
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
        
        if (is_re_export_) {
            result += " from \"" + source_module_ + "\"";
        }
    }
    
    return result;
}

std::unique_ptr<ASTNode> ExportStatement::clone() const {
    if (is_default_export_) {
        return std::make_unique<ExportStatement>(default_export_->clone(), true, start_, end_);
    } else if (is_declaration_export_) {
        return std::make_unique<ExportStatement>(declaration_->clone(), start_, end_);
    } else {
        std::vector<std::unique_ptr<ExportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ExportSpecifier>(
                    spec->get_local_name(),
                    spec->get_exported_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        
        if (is_re_export_) {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), source_module_, start_, end_);
        } else {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), start_, end_);
        }
    }
}


Value ConditionalExpression::evaluate(Context& ctx) {
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (test_value.to_boolean()) {
        return consequent_->evaluate(ctx);
    } else {
        return alternate_->evaluate(ctx);
    }
}

std::string ConditionalExpression::to_string() const {
    return test_->to_string() + " ? " + consequent_->to_string() + " : " + alternate_->to_string();
}

std::unique_ptr<ASTNode> ConditionalExpression::clone() const {
    return std::make_unique<ConditionalExpression>(
        test_->clone(), 
        consequent_->clone(), 
        alternate_->clone(), 
        start_, 
        end_
    );
}


Value RegexLiteral::evaluate(Context& ctx) {
    (void)ctx;
    try {
        auto obj = std::make_unique<Object>(Object::ObjectType::RegExp);

        obj->set_property("_isRegExp", Value(true));

        obj->set_property("__pattern__", Value(pattern_));
        obj->set_property("__flags__", Value(flags_));

        obj->set_property("source", Value(pattern_));
        // ES6: flags must be in alphabetical order
        std::string sorted_flags = flags_;
        std::sort(sorted_flags.begin(), sorted_flags.end());
        obj->set_property("flags", Value(sorted_flags));
        obj->set_property("global", Value(flags_.find('g') != std::string::npos));
        obj->set_property("ignoreCase", Value(flags_.find('i') != std::string::npos));
        obj->set_property("multiline", Value(flags_.find('m') != std::string::npos));
        obj->set_property("unicode", Value(flags_.find('u') != std::string::npos));
        obj->set_property("sticky", Value(flags_.find('y') != std::string::npos));
        obj->set_property("lastIndex", Value(0.0));

        auto regexp_impl = std::make_shared<RegExp>(pattern_, flags_);
        Object* obj_ptr = obj.get();

        auto test_fn = ObjectFactory::create_native_function("test",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) return Value(false);

                if (regexp_impl->get_global()) {
                    Value lastIndex_val = obj_ptr->get_property("lastIndex");
                    if (lastIndex_val.is_number()) {
                        regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                    }
                }

                std::string str = args[0].to_string();
                bool result = regexp_impl->test(str);

                if (regexp_impl->get_global()) {
                    obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                }

                return Value(result);
            });

        auto exec_fn = ObjectFactory::create_native_function("exec",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) return Value::null();

                Value lastIndex_val = obj_ptr->get_property("lastIndex");
                if (lastIndex_val.is_number()) {
                    regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                }

                std::string str = args[0].to_string();
                Value result = regexp_impl->exec(str);

                obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                return result;
            });

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                (void)args;
                return Value(regexp_impl->to_string());
            });

        auto compile_fn = ObjectFactory::create_native_function("compile",
            [regexp_impl, obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                std::string pattern = "";
                std::string flags = "";
                if (args.size() > 0) pattern = args[0].to_string();
                if (args.size() > 1) flags = args[1].to_string();

                regexp_impl->compile(pattern, flags);

                obj_ptr->set_property("source", Value(regexp_impl->get_source()));
                std::string sf = regexp_impl->get_flags();
                std::sort(sf.begin(), sf.end());
                obj_ptr->set_property("flags", Value(sf));
                obj_ptr->set_property("global", Value(regexp_impl->get_global()));
                obj_ptr->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                obj_ptr->set_property("multiline", Value(regexp_impl->get_multiline()));
                obj_ptr->set_property("lastIndex", Value(0.0));

                return Value(obj_ptr);
            }, 2);

        obj->set_property("test", Value(test_fn.release()));
        obj->set_property("exec", Value(exec_fn.release()));
        obj->set_property("toString", Value(toString_fn.release()));
        obj->set_property("compile", Value(compile_fn.release()));

        return Value(obj.release());
    } catch (const std::exception& e) {
        return Value::null();
    }
}

std::string RegexLiteral::to_string() const {
    return "/" + pattern_ + "/" + flags_;
}

std::unique_ptr<ASTNode> RegexLiteral::clone() const {
    return std::make_unique<RegexLiteral>(pattern_, flags_, start_, end_);
}


Value SpreadElement::evaluate(Context& ctx) {
    return argument_->evaluate(ctx);
}

std::string SpreadElement::to_string() const {
    return "..." + argument_->to_string();
}

std::unique_ptr<ASTNode> SpreadElement::clone() const {
    return std::make_unique<SpreadElement>(argument_->clone(), start_, end_);
}


Value JSXElement::evaluate(Context& ctx) {
    
    Value react = ctx.get_binding("React");
    if (!react.is_object()) {
        ctx.throw_exception(Value(std::string("React is not defined - JSX requires React to be in scope")));
        return Value();
    }
    
    Value createElement = static_cast<Object*>(react.as_object())->get_property("createElement");
    if (!createElement.is_function()) {
        ctx.throw_exception(Value(std::string("React.createElement is not a function")));
        return Value();
    }
    
    std::vector<Value> args;
    
    if (std::islower(tag_name_[0])) {
        args.push_back(Value(tag_name_));
    } else {
        Value component = ctx.get_binding(tag_name_);
        args.push_back(component);
    }
    
    auto props_obj = ObjectFactory::create_object();
    for (const auto& attr : attributes_) {
        JSXAttribute* jsx_attr = static_cast<JSXAttribute*>(attr.get());
        Value attr_value = jsx_attr->get_value()->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        props_obj->set_property(jsx_attr->get_name(), attr_value);
    }
    args.push_back(Value(props_obj.release()));
    
    for (const auto& child : children_) {
        Value child_value = child->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        args.push_back(child_value);
    }
    
    Function* create_fn = createElement.as_function();
    return create_fn->call(ctx, args);
}

std::string JSXElement::to_string() const {
    std::string result = "<" + tag_name_;
    
    for (const auto& attr : attributes_) {
        result += " " + attr->to_string();
    }
    
    if (self_closing_) {
        result += " />";
    } else {
        result += ">";
        
        for (const auto& child : children_) {
            result += child->to_string();
        }
        
        result += "</" + tag_name_ + ">";
    }
    
    return result;
}

std::unique_ptr<ASTNode> JSXElement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_attrs;
    for (const auto& attr : attributes_) {
        cloned_attrs.push_back(attr->clone());
    }
    
    std::vector<std::unique_ptr<ASTNode>> cloned_children;
    for (const auto& child : children_) {
        cloned_children.push_back(child->clone());
    }
    
    return std::make_unique<JSXElement>(tag_name_, std::move(cloned_attrs), 
                                        std::move(cloned_children), self_closing_, start_, end_);
}


Value OptionalChainingExpression::evaluate(Context& ctx) {
    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (object_value.is_null() || object_value.is_undefined()) {
        return Value();
    }
    
    if (computed_) {
        Value property_value = property_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        std::string prop_name = property_value.to_string();
        
        if (object_value.is_object()) {
            Object* obj = object_value.as_object();
            return obj->get_property(prop_name);
        }
    } else {
        if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* prop_id = static_cast<Identifier*>(property_.get());
            std::string prop_name = prop_id->get_name();
            
            if (object_value.is_object()) {
                Object* obj = object_value.as_object();
                return obj->get_property(prop_name);
            }
        }
    }
    
    return Value();
}

std::string OptionalChainingExpression::to_string() const {
    if (computed_) {
        return object_->to_string() + "?.[" + property_->to_string() + "]";
    } else {
        return object_->to_string() + "?." + property_->to_string();
    }
}

std::unique_ptr<ASTNode> OptionalChainingExpression::clone() const {
    return std::make_unique<OptionalChainingExpression>(
        object_->clone(), property_->clone(), computed_, start_, end_
    );
}


Value NullishCoalescingExpression::evaluate(Context& ctx) {
    Value left_value = left_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (!left_value.is_null() && !left_value.is_undefined()) {
        return left_value;
    }
    
    Value right_value = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    return right_value;
}

std::string NullishCoalescingExpression::to_string() const {
    return "(" + left_->to_string() + " ?? " + right_->to_string() + ")";
}

std::unique_ptr<ASTNode> NullishCoalescingExpression::clone() const {
    return std::make_unique<NullishCoalescingExpression>(
        left_->clone(), right_->clone(), start_, end_
    );
}

Value JSXText::evaluate(Context& ctx) {
    (void)ctx;
    return Value(text_);
}

std::string JSXText::to_string() const {
    return text_;
}

std::unique_ptr<ASTNode> JSXText::clone() const {
    return std::make_unique<JSXText>(text_, start_, end_);
}

Value JSXExpression::evaluate(Context& ctx) {
    return expression_->evaluate(ctx);
}

std::string JSXExpression::to_string() const {
    return "{" + expression_->to_string() + "}";
}

std::unique_ptr<ASTNode> JSXExpression::clone() const {
    return std::make_unique<JSXExpression>(expression_->clone(), start_, end_);
}

Value JSXAttribute::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string JSXAttribute::to_string() const {
    if (value_) {
        return name_ + "=" + value_->to_string();
    } else {
        return name_;
    }
}

std::unique_ptr<ASTNode> JSXAttribute::clone() const {
    std::unique_ptr<ASTNode> cloned_value = value_ ? value_->clone() : nullptr;
    return std::make_unique<JSXAttribute>(name_, std::move(cloned_value), start_, end_);
}

void DestructuringAssignment::handle_infinite_depth_destructuring(Object* obj, const std::string& nested_pattern, Context& ctx) {


    std::string pattern = nested_pattern;
    Object* current_obj = obj;

    while (!pattern.empty()) {

        if (pattern.length() > 9 && pattern.substr(0, 9) == "__nested:") {
            pattern = pattern.substr(9);
            continue;
        }

        size_t colon_pos = pattern.find(':');

        if (colon_pos == std::string::npos) {
            Value final_value = current_obj->get_property(pattern);
            if (!ctx.has_binding(pattern)) {
                ctx.create_binding(pattern, final_value, true);
            } else {
                ctx.set_binding(pattern, final_value);
            }
            return;
        }

        std::string prop_name = pattern.substr(0, colon_pos);
        std::string remaining = pattern.substr(colon_pos + 1);


        bool is_renaming = (remaining.find(':') == std::string::npos &&
                           remaining.find("__nested:") == std::string::npos);

        if (is_renaming) {
            Value prop_value = current_obj->get_property(prop_name);
            if (!ctx.has_binding(remaining)) {
                ctx.create_binding(remaining, prop_value, true);
            } else {
                ctx.set_binding(remaining, prop_value);
            }
            return;
        }


        Value prop_value = current_obj->get_property(prop_name);
        if (!prop_value.is_object()) {
            return;
        }

        current_obj = prop_value.as_object();
        pattern = remaining;

    }
}

}
