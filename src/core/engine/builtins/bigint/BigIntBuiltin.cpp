/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/BigIntBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Symbol.h"

namespace Quanta {

void register_bigint_builtins(Context& ctx) {
    auto bigint_constructor = ObjectFactory::create_native_constructor("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value prim = args.empty() ? Value() : args[0];
            // ToPrimitive(value, number) before conversion.
            if (prim.is_object() || prim.is_function()) {
                Object* obj = prim.is_function() ? static_cast<Object*>(prim.as_function()) : prim.as_object();
                Symbol* to_prim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
                Value fn = to_prim_sym ? obj->get_property(to_prim_sym->to_property_key()) : Value();
                if (ctx.has_exception()) return Value();
                if (fn.is_function()) {
                    prim = fn.as_function()->call(ctx, {Value(std::string("number"))}, prim);
                    if (ctx.has_exception()) return Value();
                    if (prim.is_object() || prim.is_function()) {
                        ctx.throw_type_error("Symbol.toPrimitive returned an object");
                        return Value();
                    }
                } else {
                    prim = Value();
                    for (const char* name : {"valueOf", "toString"}) {
                        Value m = obj->get_property(name);
                        if (ctx.has_exception()) return Value();
                        if (!m.is_function()) continue;
                        Value r = m.as_function()->call(ctx, {}, Value(obj));
                        if (ctx.has_exception()) return Value();
                        if (!r.is_object() && !r.is_function()) { prim = r; break; }
                    }
                }
            }

            try {
                if (prim.is_bigint()) {
                    return prim;
                } else if (prim.is_number()) {
                    double num = prim.as_number();
                    if (!std::isfinite(num) || std::floor(num) != num) {
                        ctx.throw_range_error("Cannot convert non-integer Number to BigInt");
                        return Value();
                    }
                    auto bigint = std::make_unique<BigInt>(static_cast<int64_t>(num));
                    return Value(bigint.release());
                } else if (prim.is_string()) {
                    auto bigint = std::make_unique<BigInt>(prim.to_string());
                    return Value(bigint.release());
                } else if (prim.is_boolean()) {
                    auto bigint = std::make_unique<BigInt>(static_cast<int64_t>(prim.to_boolean() ? 1 : 0));
                    return Value(bigint.release());
                } else {
                    ctx.throw_type_error("Cannot convert value to BigInt");
                    return Value();
                }
            } catch (const std::exception& e) {
                ctx.throw_syntax_error("Invalid BigInt: " + std::string(e.what()));
                return Value();
            }
        });
    {
        auto asIntN_fn = ObjectFactory::create_native_function("asIntN",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) { ctx.throw_type_error("BigInt.asIntN requires 2 arguments"); return Value(); }
                int64_t n = static_cast<int64_t>(args[0].to_number());
                if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
                if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
                int64_t val = args[1].as_bigint()->to_int64();
                if (n == 0) return Value(new BigInt(0));
                if (n == 64) return Value(new BigInt(val));
                int64_t mod = 1LL << n;
                int64_t result = val & (mod - 1);
                if (result >= (mod >> 1)) result -= mod;
                return Value(new BigInt(result));
            });
        bigint_constructor->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

        auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) { ctx.throw_type_error("BigInt.asUintN requires 2 arguments"); return Value(); }
                int64_t n = static_cast<int64_t>(args[0].to_number());
                if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
                if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
                int64_t val = args[1].as_bigint()->to_int64();
                if (n == 0) return Value(new BigInt(0));
                if (n == 64) return Value(new BigInt(val));
                uint64_t mask = (1ULL << n) - 1;
                uint64_t result = static_cast<uint64_t>(val) & mask;
                return Value(new BigInt(static_cast<int64_t>(result)));
            });
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
                    Value pv = this_val.as_object()->get_property("valueOf");
                    if (!ctx.has_exception() && pv.is_function()) {
                        Value r = pv.as_function()->call(ctx, {}, this_val);
                        if (!ctx.has_exception() && r.is_bigint()) bi = r.as_bigint();
                    }
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
            }, 1);
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
