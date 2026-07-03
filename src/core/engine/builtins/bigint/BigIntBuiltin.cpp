/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/BigIntBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Symbol.h"

namespace Quanta {

// ToPrimitive(value, number): @@toPrimitive first, then valueOf/toString.
static Value to_primitive_number(Context& ctx, Value v) {
    if (!v.is_object() && !v.is_function()) return v;
    Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
    Symbol* to_prim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    Value fn = to_prim_sym ? obj->get_property(to_prim_sym->to_property_key()) : Value();
    if (ctx.has_exception()) return Value();
    if (fn.is_function()) {
        Value r = fn.as_function()->call(ctx, {Value(std::string("number"))}, v);
        if (ctx.has_exception()) return Value();
        if (r.is_object() || r.is_function()) {
            ctx.throw_type_error("Symbol.toPrimitive returned an object");
            return Value();
        }
        return r;
    }
    if (!fn.is_undefined() && !fn.is_null()) {
        ctx.throw_type_error("Symbol.toPrimitive is not a function");
        return Value();
    }
    for (const char* name : {"valueOf", "toString"}) {
        Value m = obj->get_property(name);
        if (ctx.has_exception()) return Value();
        if (!m.is_function()) continue;
        Value r = m.as_function()->call(ctx, {}, Value(obj));
        if (ctx.has_exception()) return Value();
        if (!r.is_object() && !r.is_function()) return r;
    }
    ctx.throw_type_error("Cannot convert object to primitive value");
    return Value();
}

// StringToBigInt: trimmed empty -> 0n, optional sign only on decimal, 0b/0o/0x
// prefixes without sign, no numeric separators; invalid -> false.
static bool string_to_bigint(const std::string& raw, std::unique_ptr<BigInt>& out) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    size_t b = 0, e = raw.size();
    while (b < e && is_ws(raw[b])) ++b;
    while (e > b && is_ws(raw[e - 1])) --e;
    std::string s = raw.substr(b, e - b);
    if (s.empty()) {
        out = std::make_unique<BigInt>(static_cast<int64_t>(0));
        return true;
    }
    size_t i = 0;
    bool neg = false;
    int base = 10;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        i = 1;
    } else if (s.size() > 2 && s[0] == '0') {
        char p = s[1];
        if (p == 'b' || p == 'B') { base = 2; i = 2; }
        else if (p == 'o' || p == 'O') { base = 8; i = 2; }
        else if (p == 'x' || p == 'X') { base = 16; i = 2; }
    }
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); ++k) {
        char c = s[k];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
    }
    std::string digits = s.substr(i);
    if (base == 16) digits = "0x" + digits;
    else if (base == 8) digits = "0o" + digits;
    else if (base == 2) digits = "0b" + digits;
    try {
        out = std::make_unique<BigInt>(digits);
    } catch (...) {
        return false;
    }
    if (neg) *out = -*out;
    return true;
}

// ToBigInt(argument), after ToPrimitive(number) coercion.
static std::unique_ptr<BigInt> to_bigint(Context& ctx, Value v) {
    Value prim = to_primitive_number(ctx, v);
    if (ctx.has_exception()) return nullptr;
    if (prim.is_bigint()) return std::make_unique<BigInt>(*prim.as_bigint());
    if (prim.is_boolean()) return std::make_unique<BigInt>(static_cast<int64_t>(prim.as_boolean() ? 1 : 0));
    if (prim.is_string()) {
        std::unique_ptr<BigInt> out;
        if (!string_to_bigint(prim.as_string()->str(), out)) {
            ctx.throw_syntax_error("Cannot convert string to a BigInt");
            return nullptr;
        }
        return out;
    }
    ctx.throw_type_error("Cannot convert value to BigInt");
    return nullptr;
}

void register_bigint_builtins(Context& ctx) {
    auto bigint_constructor = ObjectFactory::create_native_constructor("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value prim = to_primitive_number(ctx, args.empty() ? Value() : args[0]);
            if (ctx.has_exception()) return Value();

            if (prim.is_number()) {
                double num = prim.as_number();
                if (!std::isfinite(num) || std::floor(num) != num) {
                    ctx.throw_range_error("Cannot convert non-integer Number to BigInt");
                    return Value();
                }
                return Value(new BigInt(BigInt::from_integral_double(num)));
            }
            std::unique_ptr<BigInt> result = to_bigint(ctx, prim);
            if (!result) return Value();
            return Value(result.release());
        });
    {
        // Shared steps of asIntN/asUintN: bits = ? ToIndex(bits), v = ? ToBigInt(bigint),
        // then mod = v modulo 2**bits (non-negative). Returns false on abrupt completion.
        auto as_n_common = [](Context& ctx, const std::vector<Value>& args,
                              BigInt& mod, BigInt& two_pow, uint64_t& bits_out) -> bool {
            double bn = (args.empty() ? Value() : args[0]).to_number();
            if (ctx.has_exception()) return false;
            if (std::isnan(bn)) bn = 0.0;
            bn = std::trunc(bn);
            if (bn < 0 || bn > 9007199254740991.0) {
                ctx.throw_range_error("Index out of range");
                return false;
            }
            std::unique_ptr<BigInt> v = to_bigint(ctx, args.size() > 1 ? args[1] : Value());
            if (!v) return false;
            if (bn > 1000000.0) {
                ctx.throw_range_error("Maximum BigInt size exceeded");
                return false;
            }
            bits_out = static_cast<uint64_t>(bn);
            if (bits_out == 0) {
                mod = BigInt(static_cast<int64_t>(0));
                two_pow = BigInt(static_cast<int64_t>(1));
                return true;
            }
            two_pow = BigInt(static_cast<int64_t>(1)).left_shift(BigInt(static_cast<int64_t>(bits_out)));
            mod = *v % two_pow;
            if (mod.is_negative()) mod = mod + two_pow;
            return true;
        };

        auto asIntN_fn = ObjectFactory::create_native_function("asIntN",
            [as_n_common](Context& ctx, const std::vector<Value>& args) -> Value {
                BigInt mod, two_pow;
                uint64_t bits;
                if (!as_n_common(ctx, args, mod, two_pow, bits)) return Value();
                if (bits == 0) return Value(new BigInt(mod));
                BigInt half = two_pow.right_shift(BigInt(static_cast<int64_t>(1)));
                if (mod >= half) mod = mod - two_pow;
                return Value(new BigInt(mod));
            }, 2);
        bigint_constructor->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

        auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
            [as_n_common](Context& ctx, const std::vector<Value>& args) -> Value {
                BigInt mod, two_pow;
                uint64_t bits;
                if (!as_n_common(ctx, args, mod, two_pow, bits)) return Value();
                return Value(new BigInt(mod));
            }, 2);
        bigint_constructor->set_property("asUintN", Value(asUintN_fn.release()), PropertyAttributes::BuiltinFunction);
    }
    // BigInt.prototype.toString([radix])
    {
        auto bigint_toString = ObjectFactory::create_native_function("toString",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Value this_val = ctx.get_binding("this");
                BigInt* bi = nullptr;
                if (this_val.is_bigint()) bi = this_val.as_bigint();
                else if (this_val.is_object()) {
                    // thisBigIntValue reads the [[BigIntData]] slot directly, never a user-visible method.
                    Value pv = this_val.as_object()->get_property("[[PrimitiveValue]]");
                    if (pv.is_bigint()) bi = pv.as_bigint();
                }
                if (!bi) { ctx.throw_type_error("BigInt.prototype.toString requires a BigInt this"); return Value(); }
                int radix = 10;
                if (!args.empty() && !args[0].is_undefined()) {
                    if (args[0].is_symbol()) { ctx.throw_type_error("Cannot convert Symbol to number"); return Value(); }
                    double r = args[0].to_number();
                    if (ctx.has_exception()) return Value();
                    radix = static_cast<int>(r);
                    if (radix < 2 || radix > 36) { ctx.throw_range_error("toString radix must be between 2 and 36"); return Value(); }
                }
                if (radix == 10) return Value(bi->to_string());
                // Proper radix conversion using repeated BigInt division
                static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
                bool negative = bi->is_negative();
                BigInt n(*bi);
                if (negative) n = -n;
                BigInt zero(static_cast<int64_t>(0));
                BigInt base(static_cast<int64_t>(radix));
                if (n == zero) return Value(std::string("0"));
                std::string result;
                while (!(n == zero)) {
                    BigInt rem = n % base;
                    int64_t r = rem.to_int64();
                    result = digits[r < 0 ? -r : r] + result;
                    n = n / base;
                }
                if (negative) result = "-" + result;
                return Value(result);
            }, 0);
        Value proto_val = bigint_constructor->get_property("prototype");
        if (proto_val.is_object()) proto_val.as_object()->set_property("toString", Value(bigint_toString.release()), PropertyAttributes::BuiltinFunction);
    }

    // BigInt.prototype.valueOf()
    {
        auto bigint_valueOf = ObjectFactory::create_native_function("valueOf",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_bigint()) return this_val;
                if (this_val.is_object()) {
                    Value pv = this_val.as_object()->get_property("[[PrimitiveValue]]");
                    if (!pv.is_undefined() && pv.is_bigint()) return pv;
                }
                ctx.throw_type_error("BigInt.prototype.valueOf requires a BigInt this");
                return Value();
            }, 0);
        Value proto_val = bigint_constructor->get_property("prototype");
        if (proto_val.is_object()) proto_val.as_object()->set_property("valueOf", Value(bigint_valueOf.release()), PropertyAttributes::BuiltinFunction);
    }

    // BigInt.prototype.toLocaleString(): without Intl, this is just toString().
    {
        Value proto_val = bigint_constructor->get_property("prototype");
        if (proto_val.is_object()) {
            Value to_string_fn = proto_val.as_object()->get_property("toString");
            proto_val.as_object()->set_property("toLocaleString", to_string_fn, PropertyAttributes::BuiltinFunction);
        }
    }

    // BigInt.prototype.constructor === BigInt
    {
        Value proto_val = bigint_constructor->get_property("prototype");
        if (proto_val.is_object()) {
            proto_val.as_object()->set_property("constructor", Value(bigint_constructor.get()), PropertyAttributes::BuiltinFunction);
        }
    }

    // ES2022 20.2.3.10: BigInt.prototype[@@toStringTag] = "BigInt", configurable:true
    {
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            Value proto_val = bigint_constructor->get_property("prototype");
            if (proto_val.is_object()) {
                PropertyDescriptor tag_desc(Value(std::string("BigInt")),
                    static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
                proto_val.as_object()->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
            }
        }
    }
    ctx.register_built_in_object("BigInt", bigint_constructor.release());
}

} // namespace Quanta
