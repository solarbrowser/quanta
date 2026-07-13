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

// Coerce an object to BigInt via valueOf/toString when the other operand is BigInt.
// Returns the coerced value, or the original if not an object.
static Value toBigIntCoerce(Context& ctx, const Value& v) {
    if (!v.is_object()) return v;
    Object* obj = v.as_object();
    Value valueOf = obj->get_property("valueOf");
    if (valueOf.is_function()) {
        Value result = valueOf.as_function()->call(ctx, {}, v);
        if (!ctx.has_exception() && result.is_bigint()) return result;
        ctx.clear_exception();
    }
    Value toString = obj->get_property("toString");
    if (toString.is_function()) {
        Value result = toString.as_function()->call(ctx, {}, v);
        if (!ctx.has_exception() && result.is_bigint()) return result;
        ctx.clear_exception();
    }
    return v;
}

// ToNumeric for ++/--, calling ToPrimitive("number") on objects with exception propagation.
static Value postfix_to_numeric(Context& ctx, const Value& val) {
    if (val.is_bigint()) return val;
    if (val.is_object() || val.is_function()) {
        Object* obj = val.is_function()
            ? static_cast<Object*>(val.as_function())
            : val.as_object();
        if (!obj) return Value(std::numeric_limits<double>::quiet_NaN());
        if (obj->has_property("[[PrimitiveValue]]")) {
            Value pv = obj->get_property("[[PrimitiveValue]]");
            if (pv.is_bigint()) return pv;
            // Skip for Symbol wrappers so overridden valueOf/toString are honored below.
            if (!pv.is_object() && !pv.is_symbol()) return Value(pv.to_number());
        }
        Value valueOf_method = obj->get_property("valueOf");
        if (ctx.has_exception()) return Value();
        if (valueOf_method.is_function()) {
            Value result = valueOf_method.as_function()->call(ctx, {}, val);
            if (ctx.has_exception()) return Value();
            if (!result.is_object() && !result.is_function()) {
                if (result.is_bigint()) return result;
                return Value(result.to_number());
            }
        }
        Value toString_method = obj->get_property("toString");
        if (ctx.has_exception()) return Value();
        if (toString_method.is_function()) {
            Value result = toString_method.as_function()->call(ctx, {}, val);
            if (ctx.has_exception()) return Value();
            if (!result.is_object() && !result.is_function()) {
                return Value(result.to_number());
            }
        }
        ctx.throw_type_error("Cannot convert object to primitive value");
        return Value();
    }
    return Value(val.to_number());
}

Value UnaryExpression::to_numeric(Context& ctx, const Value& v) {
    return postfix_to_numeric(ctx, v);
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
                    // ES5 8.7.2: PutValue on unresolvable reference -- set on global object
                    Object* global = ctx.get_global_object();
                    if (global) global->set_property(name, result_value);
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
                    if (key_value.is_symbol()) {
                        key = key_value.as_symbol()->to_property_key();
                    } else {
                        key = key_value.to_string();
                    }
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
    
    if (operator_ == Operator::IN && left_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* id = static_cast<Identifier*>(left_.get());
        const std::string& iname = id->get_name();
        if (!iname.empty() && iname[0] == '#') {
            Value right_value = right_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (!right_value.is_object() && !right_value.is_function()) {
                ctx.throw_type_error("Cannot use 'in' operator to search for '" + iname + "' in non-object");
                return Value();
            }
            Object* obj = right_value.is_function()
                ? static_cast<Object*>(right_value.as_function())
                : right_value.as_object();
            // Find the expected class brand for this private name from the call stack.
            // This distinguishes Parent#field from Child#field when both have the same name.
            Object* expected_brand = nullptr;
            Function* brand_fn = nullptr;
            CallStack& cs = CallStack::instance();
            for (size_t i = cs.depth(); i > 0; --i) {
                Function* fn = cs.at(i - 1).function_ptr;
                if (!fn) continue;
                Value brands_val = fn->get_property("__private_brands__");
                if (brands_val.is_object()) {
                    Value name_brand = brands_val.as_object()->get_property(iname);
                    if (name_brand.is_object() || name_brand.is_function()) {
                        expected_brand = name_brand.is_function()
                            ? static_cast<Object*>(name_brand.as_function())
                            : name_brand.as_object();
                        brand_fn = fn;
                        break;
                    }
                }
            }
            if (expected_brand) {
                // Brand check: the object must be an instance of the class that owns this private name.
                bool brand_ok = false;
                Object* proto = obj;
                while (proto) {
                    if (proto == expected_brand) { brand_ok = true; break; }
                    proto = proto->get_prototype();
                }
                if (!brand_ok) return Value(false);
                // For private methods: also check the per-instance brand slot.
                if (brand_fn) {
                    Value pm_names_val = brand_fn->get_property("__private_method_names__");
                    if (pm_names_val.is_object()) {
                        Value is_method = pm_names_val.as_object()->get_property(iname);
                        if (is_method.to_boolean()) {
                            Value pm_slot_val = brand_fn->get_property("__pm_brand_slot__");
                            if (pm_slot_val.is_string())
                                return Value(obj->has_private_slot(pm_slot_val.to_string()));
                        }
                    }
                }
            }
            // Fields are stored under a qualified key, see resolve_private_storage_key in CallStack.cpp.
            std::string qualified = expected_brand
                ? iname + "@" + std::to_string(reinterpret_cast<uintptr_t>(expected_brand))
                : resolve_private_storage_key(iname, obj);
            if (obj->has_private_slot(qualified) || obj->has_private_slot(iname)) return Value(true);
            Object* proto = obj->get_prototype();
            while (proto) {
                if (proto->has_private_slot(qualified) || proto->has_private_slot(iname)) return Value(true);
                proto = proto->get_prototype();
            }
            return Value(false);
        }
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
    
    return apply_operator(ctx, operator_, left_value, right_value);
}

Value BinaryExpression::apply_operator(Context& ctx, Operator op, const Value& left_value, const Value& right_value) {
    if (LIKELY(left_value.is_number() && right_value.is_number())) {
        double left_num = left_value.as_number();
        double right_num = right_value.as_number();
        
        switch (op) {
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
                    if (std::isnan(left_num) || left_num == 0.0) return Value::nan();
                    bool neg = std::signbit(left_num) != std::signbit(right_num);
                    return neg ? Value::negative_infinity() : Value::positive_infinity();
                }
                double result = left_num / right_num;
                if (std::isinf(result)) return std::signbit(result) ? Value::negative_infinity() : Value::positive_infinity();
                if (std::isnan(result)) return Value::nan();
                return Value(result);
            }
            case Operator::MODULO: {
                double result = std::fmod(left_num, right_num);
                if (std::isnan(result)) return Value::nan();
                return Value(result);
            }
            default:
                break;
        }
    }
    
    // ES6 ToPrimitive: Check Symbol.toPrimitive, Date objects prefer toString, others prefer valueOf
    auto toPrimitive = [&ctx](const Value& val, const std::string& hint = "default") -> Value {
        if (!val.is_object_like() || val.is_string()) return val;
        Object* obj = val.is_function() ? static_cast<Object*>(val.as_function()) : val.as_object();
        if (!obj) return val;
        // ES6: Check Symbol.toPrimitive via well-known symbol key
        Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
        std::string toPrim_key = toPrim_sym ? toPrim_sym->to_property_key() : "Symbol.toPrimitive";
        Value toPrim = obj->get_property(toPrim_key);
        if (ctx.has_exception()) return Value();
        if (toPrim.is_function()) {
            Value result = toPrim.as_function()->call(ctx, {Value(hint)}, val);
            if (ctx.has_exception()) return Value();
            if (!result.is_object_like()) return result;
            ctx.throw_type_error("Cannot convert object to primitive value");
            return Value();
        }
        // GetMethod: a present but non-callable @@toPrimitive is a TypeError.
        if (!toPrim.is_undefined() && !toPrim.is_null()) {
            ctx.throw_type_error("Symbol.toPrimitive is not a function");
            return Value();
        }
        bool prefer_string = obj->has_property("_isDate") || hint == "string";

        if (prefer_string) {
            Value toString_method = obj->get_property("toString");
            if (toString_method.is_function()) {
                Value result = toString_method.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return Value();
                if (!result.is_object_like()) return result;
            }
            Value valueOf_method = obj->get_property("valueOf");
            if (valueOf_method.is_function()) {
                Value result = valueOf_method.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return Value();
                if (!result.is_object_like()) return result;
            }
        } else {
            Value valueOf_method = obj->get_property("valueOf");
            if (valueOf_method.is_function()) {
                Value result = valueOf_method.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return Value();
                if (!result.is_object_like()) return result;
            }
            Value toString_method = obj->get_property("toString");
            if (toString_method.is_function()) {
                Value result = toString_method.as_function()->call(ctx, {}, val);
                if (ctx.has_exception()) return Value();
                if (!result.is_object_like()) return result;
            }
        }
        // Both valueOf and toString returned objects — spec requires TypeError
        ctx.throw_type_error("Cannot convert object to primitive value");
        return Value();
    };

    switch (op) {
        case Operator::ADD: {
            Value left_coerced = toPrimitive(left_value);
            if (ctx.has_exception()) return Value();
            Value right_coerced = toPrimitive(right_value);
            if (ctx.has_exception()) return Value();

            // ES6: Symbols cannot be coerced in addition
            if (left_coerced.is_symbol() || right_coerced.is_symbol()) {
                ctx.throw_type_error("Cannot convert a Symbol value to a string");
                return Value();
            }

            // If one side is BigInt, coerce object operands via valueOf/toString
            if (left_coerced.is_bigint() && right_coerced.is_object())
                right_coerced = toBigIntCoerce(ctx, right_coerced);
            else if (right_coerced.is_bigint() && left_coerced.is_object())
                left_coerced = toBigIntCoerce(ctx, left_coerced);
            if (ctx.has_exception()) return Value();
            try {
                return left_coerced.add(right_coerced);
            } catch (const BigIntTypeError& e) {
                ctx.throw_type_error(e.what());
                return Value();
            }
        }
        case Operator::SUBTRACT:
        case Operator::MULTIPLY: {
            Value left_coerced = toPrimitive(left_value, "number");
            if (ctx.has_exception()) return Value();
            Value right_coerced = toPrimitive(right_value, "number");
            if (ctx.has_exception()) return Value();
            try {
                if (op == Operator::SUBTRACT) {
                    return left_coerced.subtract(right_coerced);
                } else {
                    return left_coerced.multiply(right_coerced);
                }
            } catch (const BigIntTypeError& e) {
                ctx.throw_type_error(e.what());
                return Value();
            }
        }
        case Operator::DIVIDE:
        case Operator::MODULO:
        case Operator::EXPONENT: {
            Value lv = left_value, rv = right_value;
            if (lv.is_bigint() && rv.is_object()) rv = toBigIntCoerce(ctx, rv);
            else if (rv.is_bigint() && lv.is_object()) lv = toBigIntCoerce(ctx, lv);
            if (ctx.has_exception()) return Value();
            // Also apply toPrimitive so Object(1n) becomes 1n
            if (lv.is_object()) lv = toPrimitive(lv);
            if (rv.is_object()) rv = toPrimitive(rv);
            if (ctx.has_exception()) return Value();
            try {
                if (op == Operator::DIVIDE) return lv.divide(rv);
                if (op == Operator::MODULO) return lv.modulo(rv);
                return lv.power(rv);
            } catch (const BigIntRangeError& e) { ctx.throw_range_error(e.what()); return Value(); }
              catch (const BigIntTypeError& e) { ctx.throw_type_error(e.what()); return Value(); }
        }
            
        case Operator::EQUAL: {
            Value lp = toPrimitive(left_value, "default");
            if (ctx.has_exception()) return Value();
            Value rp = toPrimitive(right_value, "default");
            if (ctx.has_exception()) return Value();
            return Value(lp.loose_equals(rp));
        }
        case Operator::NOT_EQUAL: {
            Value lp = toPrimitive(left_value, "default");
            if (ctx.has_exception()) return Value();
            Value rp = toPrimitive(right_value, "default");
            if (ctx.has_exception()) return Value();
            return Value(!lp.loose_equals(rp));
        }
        case Operator::STRICT_EQUAL:
            return Value(left_value.strict_equals(right_value));
        case Operator::STRICT_NOT_EQUAL:
            return Value(!left_value.strict_equals(right_value));
        case Operator::LESS_THAN:
        case Operator::GREATER_THAN:
        case Operator::LESS_EQUAL:
        case Operator::GREATER_EQUAL: {
            Value lp = toPrimitive(left_value, "number");
            if (ctx.has_exception()) return Value();
            Value rp = toPrimitive(right_value, "number");
            if (ctx.has_exception()) return Value();

            // -1 b<d, 0 b==d, 1 b>d, INT_MIN undefined (d is NaN). Exact: integer
            // parts compared as BigInts, the fraction breaks ties.
            auto bigint_vs_double = [](const BigInt& b, double d) -> int {
                if (std::isnan(d)) return INT_MIN;
                if (std::isinf(d)) return d > 0 ? -1 : 1;
                double ipart = std::trunc(d);
                BigInt di = BigInt::from_integral_double(ipart);
                if (b < di) return -1;
                if (di < b) return 1;
                double frac = d - ipart;
                if (frac > 0) return -1;
                if (frac < 0) return 1;
                return 0;
            };

            // Abstract Relational Comparison (spec 7.2.13).
            // Returns -1 (px < py), 0 (px >= py), INT_MIN (undefined: NaN involved).
            auto abstract_less = [&bigint_vs_double](const Value& px, const Value& py) -> int {
                if (px.is_string() && py.is_string()) {
                    const std::string& ls = px.as_string()->str();
                    const std::string& rs = py.as_string()->str();
                    return ls < rs ? -1 : 0;
                }
                if (px.is_bigint() && py.is_bigint()) {
                    return (*px.as_bigint() < *py.as_bigint()) ? -1 : 0;
                }
                if (px.is_bigint() && py.is_number()) {
                    int c = bigint_vs_double(*px.as_bigint(), py.as_number());
                    return c == INT_MIN ? INT_MIN : (c < 0 ? -1 : 0);
                }
                if (px.is_number() && py.is_bigint()) {
                    int c = bigint_vs_double(*py.as_bigint(), px.as_number());
                    return c == INT_MIN ? INT_MIN : (c > 0 ? -1 : 0);
                }
                if ((px.is_bigint() && py.is_string()) || (px.is_string() && py.is_bigint())) {
                    // StringToBigInt: an unparseable string makes the comparison undefined.
                    try {
                        BigInt sb(px.is_string() ? px.as_string()->str() : py.as_string()->str());
                        if (px.is_bigint()) return *px.as_bigint() < sb ? -1 : 0;
                        return sb < *py.as_bigint() ? -1 : 0;
                    } catch (...) {
                        return INT_MIN;
                    }
                }
                double ln = px.to_number();
                double rn = py.to_number();
                if (std::isnan(ln) || std::isnan(rn)) return INT_MIN;
                return ln < rn ? -1 : 0;
            };

            // Spec 13.10: < uses ARC(lp,rp); > uses ARC(rp,lp);
            // <= uses ARC(rp,lp)==0 (false result); >= uses ARC(lp,rp)==0.
            if (op == Operator::LESS_THAN) {
                return Value(abstract_less(lp, rp) == -1);
            }
            if (op == Operator::GREATER_THAN) {
                return Value(abstract_less(rp, lp) == -1);
            }
            if (op == Operator::LESS_EQUAL) {
                int r = abstract_less(rp, lp);
                return Value(r == 0);
            }
            // GREATER_EQUAL
            int r = abstract_less(lp, rp);
            return Value(r == 0);
        }
            
        case Operator::INSTANCEOF: {
            // ES6: Check Symbol.hasInstance
            if (right_value.is_function() || right_value.is_object()) {
                Object* rhs = right_value.is_function()
                    ? static_cast<Object*>(right_value.as_function())
                    : right_value.as_object();
                Value hasInstance = rhs->get_property("Symbol.hasInstance");
                if (!hasInstance.is_undefined() && hasInstance.is_function()) {
                    Value result = hasInstance.as_function()->call(ctx, {left_value}, right_value);
                    return Value(result.to_boolean());
                }
            }
            if (!right_value.is_function()) {
                // Also allow Proxy wrapping a function (the get trap already ran above)
                if (right_value.is_object() && right_value.as_object()->get_type() == Object::ObjectType::Proxy) {
                    Proxy* proxy_rhs = static_cast<Proxy*>(right_value.as_object());
                    Object* proxy_target = proxy_rhs->get_proxy_target();
                    if (proxy_target && proxy_target->is_function()) {
                        // Use the proxy's prototype (via get trap) for the check
                        Value proto_val = right_value.as_object()->get_property("prototype");
                        if (ctx.has_exception()) return Value();
                        bool result = left_value.instanceof_check(Value(static_cast<Function*>(proxy_target)));
                        if (ctx.has_exception()) return Value();
                        return Value(result);
                    }
                }
                ctx.throw_type_error("Right-hand side of instanceof is not callable");
                return Value(false);
            }
            // OrdinaryHasInstance step 3: if O is not Object, return false (no prototype check needed)
            if (!left_value.is_object() && !left_value.is_function()) {
                return Value(false);
            }
            // ES5 15.3.5.3 step 3: if F.prototype is not an object, throw TypeError
            {
                Value proto_prop = right_value.as_function()->get_property("prototype");
                if (!proto_prop.is_object() && !proto_prop.is_function()) {
                    ctx.throw_type_error("Function has non-object prototype in instanceof check");
                    return Value(false);
                }
            }
            {
                bool result = left_value.instanceof_check(right_value);
                if (ctx.has_exception()) return Value();
                return Value(result);
            }
        }

        case Operator::IN: {
            Value left_prim = toPrimitive(left_value, "string");
            std::string property_name;
            if (left_prim.is_symbol()) {
                property_name = left_prim.as_symbol()->to_property_key();
            } else {
                property_name = left_prim.to_string();
            }
            if (!right_value.is_object() && !right_value.is_function()) {
                ctx.throw_type_error("Cannot use 'in' operator to search for '" + property_name + "' in " + right_value.to_string());
                return Value(false);
            }
            Object* obj = right_value.is_function()
                ? static_cast<Object*>(right_value.as_function())
                : right_value.as_object();
            if (obj->get_type() == Object::ObjectType::Proxy) {
                return Value(static_cast<Proxy*>(obj)->has_trap(Value(property_name)));
            }
            return Value(obj->has_property(property_name));
        }
        
        case Operator::BITWISE_AND:
        case Operator::BITWISE_OR:
        case Operator::BITWISE_XOR:
        case Operator::LEFT_SHIFT:
        case Operator::RIGHT_SHIFT:
        case Operator::UNSIGNED_RIGHT_SHIFT: {
            Value lv = toPrimitive(left_value);
            if (ctx.has_exception()) return Value();
            Value rv = toPrimitive(right_value);
            if (ctx.has_exception()) return Value();
            if (lv.is_bigint() && rv.is_object()) rv = toBigIntCoerce(ctx, rv);
            else if (rv.is_bigint() && lv.is_object()) lv = toBigIntCoerce(ctx, lv);
            if (ctx.has_exception()) return Value();
            try {
                if (op == Operator::BITWISE_AND) return lv.bitwise_and(rv);
                if (op == Operator::BITWISE_OR)  return lv.bitwise_or(rv);
                if (op == Operator::BITWISE_XOR) return lv.bitwise_xor(rv);
                if (op == Operator::LEFT_SHIFT)  return lv.left_shift(rv);
                if (op == Operator::RIGHT_SHIFT) return lv.right_shift(rv);
                return lv.unsigned_right_shift(rv);
            } catch (const BigIntTypeError& e) { ctx.throw_type_error(e.what()); return Value(); }
        }
            
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

// Mirrors MemberExpression::evaluate's super-read resolution. __super__ may be a non-function
// sentinel for object-literal methods, so is_function() must gate the class-method lookup.
static Object* resolve_super_lookup_proto(Context& ctx) {
    Value super_ctor = ctx.get_binding("__super__");
    if (super_ctor.is_function()) {
        if (ctx.has_binding("__super_is_static__")) {
            return super_ctor.as_function();
        }
        Value proto_val = super_ctor.as_function()->get_property("prototype");
        return proto_val.is_object() ? proto_val.as_object() : nullptr;
    }
    Value home = ctx.get_binding("__home_object__");
    if (!home.is_undefined() && !home.is_null()) {
        Object* home_obj = home.is_function() ? static_cast<Object*>(home.as_function()) : home.as_object();
        return home_obj ? home_obj->get_prototype() : nullptr;
    }
    Value this_val = ctx.get_binding("this");
    if (this_val.is_object_like()) {
        Object* this_obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();
        return this_obj ? this_obj->get_prototype() : nullptr;
    }
    return nullptr;
}

// Writes the result of ++/-- on a MemberExpression target. For super.x, the write targets
// 'this' but the setter lookup must start at the super base.
static bool write_member_update_result(Context& ctx, MemberExpression* member, const Value& new_value) {
    bool is_super = member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
        static_cast<Identifier*>(member->get_object())->get_name() == "super";

    Object* write_obj = nullptr;
    Object* lookup_obj = nullptr;
    if (is_super) {
        Value this_val = ctx.get_binding("this");
        write_obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function())
                  : (this_val.is_object() ? this_val.as_object() : nullptr);
        lookup_obj = resolve_super_lookup_proto(ctx);
        if (!lookup_obj) {
            ctx.throw_type_error("Cannot assign to property of null (super property)");
            return false;
        }
    } else {
        Value obj = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return false;
        if (!obj.is_object() && !obj.is_function()) {
            ctx.throw_exception(Value(std::string("Cannot assign to property of non-object")));
            return false;
        }
        write_obj = obj.is_function() ? static_cast<Object*>(obj.as_function()) : obj.as_object();
    }

    std::string prop_name;
    if (member->is_computed()) {
        Value prop_value = member->get_property()->evaluate(ctx);
        if (ctx.has_exception()) return false;
        if (prop_value.is_symbol()) {
            prop_name = prop_value.as_symbol()->to_property_key();
        } else {
            prop_name = prop_value.to_string();
        }
    } else if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
        prop_name = static_cast<Identifier*>(member->get_property())->get_name();
    } else {
        ctx.throw_exception(Value(std::string("Invalid property name")));
        return false;
    }
    if (ctx.has_exception()) return false;

    if (is_super) {
        PropertyDescriptor desc = lookup_obj->get_property_descriptor(prop_name);
        if (!desc.is_accessor_descriptor() && !lookup_obj->has_own_property(prop_name)) {
            Object* proto = lookup_obj->get_prototype();
            while (proto) {
                PropertyDescriptor proto_desc = proto->get_property_descriptor(prop_name);
                if (proto_desc.is_accessor_descriptor()) { desc = proto_desc; break; }
                if (proto_desc.has_value()) break;
                proto = proto->get_prototype();
            }
        }
        if (desc.is_accessor_descriptor()) {
            if (desc.has_setter()) {
                Function* setter_fn = dynamic_cast<Function*>(desc.get_setter());
                if (setter_fn && write_obj) setter_fn->call(ctx, {new_value}, Value(write_obj));
            }
        } else if (write_obj) {
            write_obj->ordinary_set(prop_name, new_value);
        }
    } else if (write_obj) {
        // ++obj.#x: fields live under the qualified key and are written raw --
        // never through Proxy traps or exotic overrides (spec: private state
        // bypasses [[Set]]).
        if (!member->is_computed() && !prop_name.empty() && prop_name[0] == '#') {
            std::string qualified = resolve_private_storage_key(prop_name, write_obj);
            if (write_obj->has_private_slot(qualified)) {
                write_obj->set_private_slot_value(qualified, new_value);
                return true;
            }
            // Prototype-held private accessor/method: write through the setter
            // with `this` = the instance -- falling through to set_property
            // would silently plant an ordinary bare-"#x" data property.
            Object* owner = resolve_private_accessor_owner(prop_name);
            PropertyDescriptor pd;
            bool found = false;
            if (owner) {
                pd = owner->get_property_descriptor(qualified);
                found = pd.is_accessor_descriptor() || pd.has_value();
                if (!found) {
                    pd = owner->get_property_descriptor(prop_name);
                    found = pd.is_accessor_descriptor() || pd.has_value();
                }
            }
            if (!found) {
                for (Object* proto = write_obj->get_prototype(); proto; proto = proto->get_prototype()) {
                    pd = proto->get_property_descriptor(qualified);
                    if (pd.is_accessor_descriptor() || pd.has_value()) { found = true; break; }
                    pd = proto->get_property_descriptor(prop_name);
                    if (pd.is_accessor_descriptor() || pd.has_value()) { found = true; break; }
                }
            }
            if (found && pd.is_accessor_descriptor()) {
                if (!pd.has_setter()) {
                    ctx.throw_type_error("'" + prop_name + "' was defined without a setter");
                    return false;
                }
                Function* setter_fn = dynamic_cast<Function*>(pd.get_setter());
                if (setter_fn) {
                    Value recv = write_obj->is_function()
                        ? Value(static_cast<Function*>(write_obj)) : Value(write_obj);
                    setter_fn->call(ctx, {new_value}, recv);
                    if (ctx.has_exception()) return false;
                }
                return true;
            }
            if (found) {
                // Data (a private method): not assignable.
                ctx.throw_type_error("'" + prop_name + "' is a private method and cannot be assigned to");
                return false;
            }
        }
        write_obj->set_property(prop_name, new_value);
    }
    return true;
}

Value UnaryExpression::evaluate(Context& ctx) {
    switch (operator_) {
        case Operator::PLUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (operand_value.is_object() || operand_value.is_function()) {
                Value prim = postfix_to_numeric(ctx, operand_value);
                if (ctx.has_exception()) return Value();
                if (prim.is_bigint()) {
                    ctx.throw_type_error("Cannot convert a BigInt value to a number");
                    return Value();
                }
                return prim;
            }
            return operand_value.unary_plus();
        }
        case Operator::MINUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (operand_value.is_object() || operand_value.is_function()) {
                Value prim = postfix_to_numeric(ctx, operand_value);
                if (ctx.has_exception()) return Value();
                return prim.unary_minus();
            }
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
            if (operand_value.is_object() || operand_value.is_function()) {
                Value prim = postfix_to_numeric(ctx, operand_value);
                if (ctx.has_exception()) return Value();
                return prim.bitwise_not();
            }
            return operand_value.bitwise_not();
        }
        case Operator::TYPEOF: {
            if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                const std::string& id_name = static_cast<Identifier*>(operand_.get())->get_name();
                if (ctx.is_in_tdz(id_name)) {
                    ctx.throw_reference_error("Cannot access '" + id_name + "' before initialization");
                    return Value();
                }
            }
            Value operand_value = operand_->evaluate(ctx);

            // typeof only suppresses ReferenceError when the operand itself is the unresolvable reference (`typeof x`), not a nested one (`typeof x.x`).
            if (ctx.has_exception()) {
                if (operand_->get_type() == ASTNode::Type::IDENTIFIER) {
                    ctx.clear_exception();
                    return Value(std::string("undefined"));
                }
                return Value();
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
                // delete super.prop always throws ReferenceError (spec 13.5.1.2)
                if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
                     static_cast<Identifier*>(member->get_object())->get_name() == "super") {
                    ctx.throw_reference_error("Cannot delete a super property");
                    return Value();
                }
                Value object_value = member->get_object()->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                Object* obj = nullptr;
                if (object_value.is_object()) {
                    obj = object_value.as_object();
                } else if (object_value.is_function()) {
                    obj = object_value.as_function();
                }
                if (!obj) {
                    // null/undefined: ToObject throws TypeError (spec 12.5.3.2 step 5b)
                    if (object_value.is_null() || object_value.is_undefined()) {
                        ctx.throw_type_error("Cannot convert undefined or null to object");
                        return Value();
                    }
                    // Primitive wrapper: delete on temporary object always succeeds
                    return Value(true);
                }
                std::string property_name;
                
                if (member->is_computed()) {
                    Value prop_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    if (prop_value.is_symbol()) {
                        property_name = prop_value.as_symbol()->to_property_key();
                    } else {
                        property_name = prop_value.to_string();
                    }
                } else {
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* id = static_cast<Identifier*>(member->get_property());
                        property_name = id->get_name();
                    } else {
                        ctx.throw_exception(Value(std::string("Invalid property access in delete")));
                        return Value();
                    }
                }
                
                bool deleted;
                if (obj->get_type() == Object::ObjectType::Proxy) {
                    deleted = static_cast<Proxy*>(obj)->delete_trap(Value(property_name));
                } else {
                    deleted = obj->delete_property(property_name);
                }
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
                Environment* ref_env = ctx.find_binding_env(id->get_name());
                if (!ref_env) {
                    ctx.throw_reference_error(id->get_name() + " is not defined");
                    return Value();
                }
                if (ctx.is_in_tdz(id->get_name())) {
                    ctx.throw_reference_error("Cannot access '" + id->get_name() + "' before initialization");
                    return Value();
                }
                Value current = ref_env->get_binding_direct(id->get_name(), &ctx);
                if (ctx.has_exception()) return Value();
                Value numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value incremented;
                if (numeric.is_bigint()) {
                    incremented = Value(new BigInt(*numeric.as_bigint() + BigInt(1)));
                } else {
                    incremented = Value(numeric.to_number() + 1.0);
                }
                if (!ref_env->set_binding_direct(id->get_name(), incremented, &ctx)) {
                    if (ctx.has_exception()) return Value();
                    if (ctx.is_strict_mode() || ctx.is_strict_const(id->get_name())) {
                        ctx.throw_type_error("Assignment to constant variable '" + id->get_name() + "'");
                        return Value();
                    }
                }
                return incremented;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value incremented;
                if (numeric.is_bigint()) {
                    incremented = Value(new BigInt(*numeric.as_bigint() + BigInt(1)));
                } else {
                    incremented = Value(numeric.to_number() + 1.0);
                }

                if (!write_member_update_result(ctx, member, incremented)) return Value();
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
                Environment* ref_env = ctx.find_binding_env(id->get_name());
                if (!ref_env) {
                    ctx.throw_reference_error(id->get_name() + " is not defined");
                    return Value();
                }
                if (ctx.is_in_tdz(id->get_name())) {
                    ctx.throw_reference_error("Cannot access '" + id->get_name() + "' before initialization");
                    return Value();
                }
                Value current = ref_env->get_binding_direct(id->get_name(), &ctx);
                if (ctx.has_exception()) return Value();
                Value old_numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value incremented;
                if (old_numeric.is_bigint()) {
                    incremented = Value(new BigInt(*old_numeric.as_bigint() + BigInt(1)));
                } else {
                    incremented = Value(old_numeric.to_number() + 1.0);
                }
                if (!ref_env->set_binding_direct(id->get_name(), incremented, &ctx)) {
                    if (ctx.has_exception()) return Value();
                    if (ctx.is_strict_mode() || ctx.is_strict_const(id->get_name())) {
                        ctx.throw_type_error("Assignment to constant variable '" + id->get_name() + "'");
                        return Value();
                    }
                }
                return old_numeric;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value old_numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value incremented;
                if (old_numeric.is_bigint()) {
                    incremented = Value(new BigInt(*old_numeric.as_bigint() + BigInt(1)));
                } else {
                    incremented = Value(old_numeric.to_number() + 1.0);
                }

                if (!write_member_update_result(ctx, member, incremented)) return Value();
                return old_numeric;
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
                Environment* ref_env = ctx.find_binding_env(id->get_name());
                if (!ref_env) {
                    ctx.throw_reference_error(id->get_name() + " is not defined");
                    return Value();
                }
                if (ctx.is_in_tdz(id->get_name())) {
                    ctx.throw_reference_error("Cannot access '" + id->get_name() + "' before initialization");
                    return Value();
                }
                Value current = ref_env->get_binding_direct(id->get_name(), &ctx);
                if (ctx.has_exception()) return Value();
                Value numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value decremented;
                if (numeric.is_bigint()) {
                    decremented = Value(new BigInt(*numeric.as_bigint() - BigInt(1)));
                } else {
                    decremented = Value(numeric.to_number() - 1.0);
                }
                if (!ref_env->set_binding_direct(id->get_name(), decremented, &ctx)) {
                    if (ctx.has_exception()) return Value();
                    if (ctx.is_strict_mode() || ctx.is_strict_const(id->get_name())) {
                        ctx.throw_type_error("Assignment to constant variable '" + id->get_name() + "'");
                        return Value();
                    }
                }
                return decremented;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value decremented;
                if (numeric.is_bigint()) {
                    decremented = Value(new BigInt(*numeric.as_bigint() - BigInt(1)));
                } else {
                    decremented = Value(numeric.to_number() - 1.0);
                }

                if (!write_member_update_result(ctx, member, decremented)) return Value();
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
                Environment* ref_env = ctx.find_binding_env(id->get_name());
                if (!ref_env) {
                    ctx.throw_reference_error(id->get_name() + " is not defined");
                    return Value();
                }
                if (ctx.is_in_tdz(id->get_name())) {
                    ctx.throw_reference_error("Cannot access '" + id->get_name() + "' before initialization");
                    return Value();
                }
                Value current = ref_env->get_binding_direct(id->get_name(), &ctx);
                if (ctx.has_exception()) return Value();
                Value old_numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value decremented;
                if (old_numeric.is_bigint()) {
                    decremented = Value(new BigInt(*old_numeric.as_bigint() - BigInt(1)));
                } else {
                    decremented = Value(old_numeric.to_number() - 1.0);
                }
                if (!ref_env->set_binding_direct(id->get_name(), decremented, &ctx)) {
                    if (ctx.has_exception()) return Value();
                    if (ctx.is_strict_mode() || ctx.is_strict_const(id->get_name())) {
                        ctx.throw_type_error("Assignment to constant variable '" + id->get_name() + "'");
                        return Value();
                    }
                }
                return old_numeric;
            } else if (operand_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(operand_.get());
                Value current = member->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                Value old_numeric = postfix_to_numeric(ctx, current);
                if (ctx.has_exception()) return Value();
                Value decremented;
                if (old_numeric.is_bigint()) {
                    decremented = Value(new BigInt(*old_numeric.as_bigint() - BigInt(1)));
                } else {
                    decremented = Value(old_numeric.to_number() - 1.0);
                }

                if (!write_member_update_result(ctx, member, decremented)) return Value();
                return old_numeric;
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


} // namespace Quanta
