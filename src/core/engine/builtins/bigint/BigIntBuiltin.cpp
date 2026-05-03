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

namespace Quanta {

void register_bigint_builtins(Context& ctx) {
    auto bigint_constructor = ObjectFactory::create_native_constructor("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("BigInt constructor requires an argument")));
                return Value();
            }
            
            try {
                if (args[0].is_number()) {
                    double num = args[0].as_number();
                    if (std::floor(num) != num) {
                        ctx.throw_exception(Value(std::string("Cannot convert non-integer Number to BigInt")));
                        return Value();
                    }
                    auto bigint = std::make_unique<BigInt>(static_cast<int64_t>(num));
                    return Value(bigint.release());
                } else if (args[0].is_string()) {
                    auto bigint = std::make_unique<BigInt>(args[0].to_string());
                    return Value(bigint.release());
                } else {
                    ctx.throw_exception(Value(std::string("Cannot convert value to BigInt")));
                    return Value();
                }
            } catch (const std::exception& e) {
                ctx.throw_exception(Value("Invalid BigInt: " + std::string(e.what())));
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
    ctx.register_built_in_object("BigInt", bigint_constructor.release());
}

} // namespace Quanta
