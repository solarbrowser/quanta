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

// ToNumeric: ToPrimitive with a number hint, then a Number unless the
// primitive is a BigInt. The hand-written version here consulted valueOf and
// toString directly and never looked at @@toPrimitive, so an object that only
// defines that method converted to NaN.
static Value ordinary_to_numeric(Context& ctx, const Value& val) {
    if (val.is_bigint()) return val;
    Value prim = val;
    if (val.is_object() || val.is_function()) {
        Object* obj = val.is_function()
            ? static_cast<Object*>(val.as_function())
            : val.as_object();
        if (!obj) return Value(std::numeric_limits<double>::quiet_NaN());
        prim = obj->to_primitive(ctx, "number");
        if (ctx.has_exception()) return Value();
    }
    if (prim.is_bigint()) return prim;
    if (prim.is_symbol()) {
        ctx.throw_type_error("Cannot convert a Symbol value to a number");
        return Value();
    }
    return Value(prim.to_number());
}

Value UnaryExpression::to_numeric(Context& ctx, const Value& v) {
    return ordinary_to_numeric(ctx, v);
}

// `#name in obj` (ergonomic brand check), shared with Op::HasPrivate so the
// compiled and interpreted paths cannot disagree.
Value private_name_in(Context& ctx, const std::string& iname, const Value& target) {
    if (!target.is_object() && !target.is_function()) {
        ctx.throw_type_error("Cannot use 'in' operator to search for '" + iname + "' in non-object");
        return Value();
    }
    Object* obj = target.is_function()
        ? static_cast<Object*>(target.as_function())
        : target.as_object();
    // Find the expected class brand for this private name from the call stack.
    // This distinguishes Parent#field from Child#field when both have the same name.
    Object* expected_brand = nullptr;
    Function* brand_fn = nullptr;
    CallStack& cs = CallStack::instance();
    for (size_t i = cs.depth(); i > 0; --i) {
        Function* fn = cs.at(i - 1).function_ptr;
        if (!fn) continue;
        if (Object* brands = fn->private_brands()) {
            Value name_brand = brands->get_property(iname);
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
            Value pm_names_val = brand_fn->get_internal_slot("__private_method_names__");
            if (pm_names_val.is_object()) {
                Value is_method = pm_names_val.as_object()->get_property(iname);
                if (is_method.to_boolean()) {
                    const std::string& pm_slot = brand_fn->pm_brand_slot();
                    if (!pm_slot.empty())
                        return Value(obj->has_private_slot(pm_slot));
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



Value BinaryExpression::apply_operator(Context& ctx, Operator op, const Value& left_value, const Value& right_value) {
    // Two strings added together are concatenated, and nothing on the way to
    // that answer can observe anything: ToPrimitive returns a string
    // unchanged, neither operand can be a symbol or a bigint, and nothing
    // throws. The general path below still asks all of those questions, and
    // for a template literal -- which is additions and nothing else -- they
    // are the bulk of what it costs.
    if (op == Operator::ADD && left_value.is_string() && right_value.is_string()) {
        return Value(String::make_concat(left_value.as_string(), right_value.as_string()));
    }
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
                double result = js_modulo(left_num, right_num);
                if (std::isnan(result)) return Value::nan();
                return Value(result);
            }
            default:
                break;
        }
    }
    
    auto toPrimitive = [&ctx](const Value& val, const std::string& hint = "default") -> Value {
        if (!val.is_object_like() || val.is_string()) return val;
        Object* obj = val.is_function() ? static_cast<Object*>(val.as_function()) : val.as_object();
        return obj ? obj->to_primitive(ctx, hint) : val;
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
            // ToNumeric on the left completely -- conversion included -- before
            // the right is touched at all. Converting both to primitives first
            // and only then to numbers runs the right operand's valueOf even
            // when the left one already has no numeric value.
            Value left_coerced = ordinary_to_numeric(ctx, left_value);
            if (ctx.has_exception()) return Value();
            Value right_coerced = ordinary_to_numeric(ctx, right_value);
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
            Value lv = ordinary_to_numeric(ctx, left_value);
            if (ctx.has_exception()) return Value();
            Value rv = ordinary_to_numeric(ctx, right_value);
            if (ctx.has_exception()) return Value();
            try {
                if (op == Operator::DIVIDE) return lv.divide(rv);
                if (op == Operator::MODULO) return lv.modulo(rv);
                return lv.power(rv);
            } catch (const BigIntRangeError& e) { ctx.throw_range_error(e.what()); return Value(); }
              catch (const BigIntTypeError& e) { ctx.throw_type_error(e.what()); return Value(); }
        }
            
        case Operator::EQUAL: {
            // Two objects are compared as references and nothing is converted:
            // the spec's first step is IsStrictlyEqual once the types match.
            // Converting anyway turned every pair of plain objects into
            // "[object Object]" and made them equal to each other -- and every
            // pair of empty arrays into "", and two identical function
            // literals into the same source text.
            // null and undefined are loosely equal to each other and to nothing
            // else. An object compared with either is simply not equal, and
            // converting it would run its valueOf -- which `x != null`, the
            // most common null check there is, must never do.
            if (left_value.is_null() || left_value.is_undefined() ||
                right_value.is_null() || right_value.is_undefined()) {
                bool left_nullish = left_value.is_null() || left_value.is_undefined();
                bool right_nullish = right_value.is_null() || right_value.is_undefined();
                return Value(left_nullish && right_nullish);
            }
            if (left_value.is_object_like() && right_value.is_object_like()) {
                return Value(left_value.strict_equals(right_value));
            }
            Value lp = toPrimitive(left_value, "default");
            if (ctx.has_exception()) return Value();
            Value rp = toPrimitive(right_value, "default");
            if (ctx.has_exception()) return Value();
            return Value(lp.loose_equals(rp));
        }
        case Operator::NOT_EQUAL: {
            // See EQUAL above.
            // null and undefined are loosely equal to each other and to nothing
            // else. An object compared with either is simply not equal, and
            // converting it would run its valueOf -- which `x != null`, the
            // most common null check there is, must never do.
            if (left_value.is_null() || left_value.is_undefined() ||
                right_value.is_null() || right_value.is_undefined()) {
                bool left_nullish = left_value.is_null() || left_value.is_undefined();
                bool right_nullish = right_value.is_null() || right_value.is_undefined();
                return Value(!(left_nullish && right_nullish));
            }
            if (left_value.is_object_like() && right_value.is_object_like()) {
                return Value(!left_value.strict_equals(right_value));
            }
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
            // While nothing anywhere has its own Symbol.hasInstance, resolving
            // and calling Function.prototype[Symbol.hasInstance] on a function
            // right-hand side could only ever reach that same builtin -- run
            // its algorithm directly instead of the property lookup and the
            // native call to get there.
            if (right_value.is_function() && Object::has_instance_protector_intact()) {
                bool result = ordinary_has_instance(ctx, right_value, left_value);
                if (ctx.has_exception()) return Value();
                return Value(result);
            }
            // ES6: Check Symbol.hasInstance
            if (right_value.is_function() || right_value.is_object()) {
                Object* rhs = right_value.is_function()
                    ? static_cast<Object*>(right_value.as_function())
                    : right_value.as_object();
                // Built once: the key is past what a std::string keeps inline,
                // so spelling it here put a heap allocation on every
                // `instanceof` before any of the work started.
                static const std::string kHasInstance = "Symbol.hasInstance";
                Value hasInstance = rhs->get_property(kHasInstance);
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
            Value lv = ordinary_to_numeric(ctx, left_value);
            if (ctx.has_exception()) return Value();
            Value rv = ordinary_to_numeric(ctx, right_value);
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
                Function* setter_fn = as_function(desc.get_setter());
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
                Function* setter_fn = as_function(pd.get_setter());
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
