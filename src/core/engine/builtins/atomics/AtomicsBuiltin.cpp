/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/AtomicsBuiltin.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace Quanta {

namespace {

using AT = TypedArrayBase::ArrayType;

double to_number_throwing(Context& ctx, const Value& v) {
    if (v.is_symbol() || v.is_bigint()) {
        ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
        return 0.0;
    }
    return v.to_number();
}

double to_integer_or_infinity(Context& ctx, const Value& v) {
    double n = to_number_throwing(ctx, v);
    if (ctx.has_exception()) return 0.0;
    if (std::isnan(n)) return 0.0;
    if (std::isinf(n)) return n;
    double r = n < 0 ? -std::floor(-n) : std::floor(n);
    return r == 0.0 ? 0.0 : r; // ToInteger normalizes -0 to +0
}

double to_index(Context& ctx, const Value& v) {
    if (v.is_undefined()) return 0.0;
    double n = to_integer_or_infinity(ctx, v);
    if (ctx.has_exception()) return 0.0;
    if (n < 0 || n > 9007199254740991.0) {
        ctx.throw_range_error("Atomics: invalid index");
        return 0.0;
    }
    return n;
}

// ToBigInt(value): boolean/string/bigint coerce directly; objects go through ToPrimitive(number).
Value to_bigint_value(Context& ctx, const Value& v) {
    if (v.is_bigint()) return v;
    if (v.is_boolean()) return Value(new BigInt(v.as_boolean() ? 1 : 0));
    if (v.is_string()) {
        try {
            return Value(new BigInt(v.as_string()->str()));
        } catch (...) {
            ctx.throw_syntax_error("Cannot convert string to a BigInt");
            return Value();
        }
    }
    if (v.is_object() || v.is_function()) {
        Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
        Symbol* to_prim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
        Value to_prim_fn = to_prim_sym ? obj->get_property(to_prim_sym->to_property_key()) : Value();
        if (ctx.has_exception()) return Value();
        if (!to_prim_fn.is_undefined() && !to_prim_fn.is_null()) {
            if (!to_prim_fn.is_function()) { ctx.throw_type_error("Symbol.toPrimitive is not a function"); return Value(); }
            Value r = to_prim_fn.as_function()->call(ctx, {Value(std::string("number"))}, v);
            if (ctx.has_exception()) return Value();
            if (r.is_object() || r.is_function()) { ctx.throw_type_error("Cannot convert object to a BigInt"); return Value(); }
            return to_bigint_value(ctx, r);
        }
        for (const char* name : {"valueOf", "toString"}) {
            Value fn = obj->get_property(name);
            if (ctx.has_exception()) return Value();
            if (!fn.is_function()) continue;
            Value r = fn.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return Value();
            if (!r.is_object() && !r.is_function()) return to_bigint_value(ctx, r);
        }
    }
    ctx.throw_type_error("Cannot convert value to BigInt");
    return Value();
}

bool is_bigint_array_type(AT t) { return t == AT::BIGINT64 || t == AT::BIGUINT64; }

bool is_atomics_allowed_type(AT t, bool waitable) {
    if (waitable) return t == AT::INT32 || t == AT::BIGINT64;
    switch (t) {
        case AT::INT8: case AT::UINT8: case AT::INT16: case AT::UINT16:
        case AT::INT32: case AT::UINT32: case AT::BIGINT64: case AT::BIGUINT64:
            return true;
        default:
            return false;
    }
}

// Reproduces set_element's modular wraparound so compareExchange/wait's `expected`
// value is canonicalized the same way storing it would be.
double truncate_to_element_type(double raw, AT t) {
    int bits;
    bool is_signed;
    switch (t) {
        case AT::INT8: bits = 8; is_signed = true; break;
        case AT::UINT8: bits = 8; is_signed = false; break;
        case AT::INT16: bits = 16; is_signed = true; break;
        case AT::UINT16: bits = 16; is_signed = false; break;
        case AT::INT32: bits = 32; is_signed = true; break;
        case AT::UINT32: bits = 32; is_signed = false; break;
        default: return raw;
    }
    if (std::isnan(raw) || std::isinf(raw) || raw == 0.0) return 0.0;
    double truncated = std::trunc(raw);
    double mod = std::pow(2.0, bits);
    double mod_val = std::fmod(truncated, mod);
    if (mod_val < 0) mod_val += mod;
    if (is_signed && mod_val >= mod / 2.0) mod_val -= mod;
    return mod_val;
}

// Truncates a BigInt to the stored 64-bit representation, so e.g. -5n compares
// equal to a BigUint64Array slot holding 2^64-5.
Value truncate_bigint_to_element_type(const BigInt& b, AT t) {
    int64_t raw = b.to_int64();
    if (t == AT::BIGINT64) return Value(new BigInt(raw));
    return Value(new BigInt(std::to_string(static_cast<uint64_t>(raw))));
}

// ValidateIntegerTypedArray: RequireInternalSlot + allowed-type check, which must run (and throw
// TypeError) before the index/value arguments are ever coerced.
TypedArrayBase* validate_integer_typed_array(Context& ctx, const Value& v, bool waitable) {
    if (!v.is_object() || !v.as_object()->is_typed_array()) {
        ctx.throw_type_error("Atomics: argument is not a TypedArray");
        return nullptr;
    }
    TypedArrayBase* ta = static_cast<TypedArrayBase*>(v.as_object());
    if (!is_atomics_allowed_type(ta->get_array_type(), waitable)) {
        ctx.throw_type_error("Atomics: TypedArray type not supported for this operation");
        return nullptr;
    }
    if (ta->is_out_of_bounds()) { ctx.throw_type_error("Atomics: TypedArray is out of bounds"); return nullptr; }
    return ta;
}

// ValidateAtomicAccess: ToIndex(index), then bounds-checked against the current (post-coercion) length.
double validate_atomic_access(Context& ctx, TypedArrayBase* ta, const Value& index_val) {
    // Length is sampled before index coercion: a valueOf that grows/resizes the
    // buffer must not retroactively make an originally out-of-range index valid.
    double length = static_cast<double>(ta->length());
    double idx = to_index(ctx, index_val);
    if (ctx.has_exception()) return 0.0;
    if (idx >= length) {
        ctx.throw_range_error("Atomics access index out of range");
        return 0.0;
    }
    return idx;
}

// A pending waitAsync is a promise keyed by absolute buffer position; a same-agent
// Atomics.notify() finds and resolves it here, no cross-thread wakeup needed.
struct PendingWaiter {
    ArrayBuffer* buffer;
    size_t byte_index;
    Promise* promise;
};

std::vector<PendingWaiter>& pending_waiters() {
    static std::vector<PendingWaiter> registry;
    return registry;
}

enum class RmwOp { Add, And, Or, Xor, Sub, Exchange };

Value atomic_read_modify_write(Context& ctx, const std::vector<Value>& args, RmwOp op) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], false);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool big = is_bigint_array_type(ta->get_array_type());
    Value raw = args.size() > 2 ? args[2] : Value();
    Value coerced = big ? to_bigint_value(ctx, raw) : Value(to_number_throwing(ctx, raw));
    if (ctx.has_exception()) return Value();

    size_t idx = static_cast<size_t>(idx_d);
    Value old_val = ta->get_element(idx);
    Value new_val;
    if (op == RmwOp::Exchange) {
        new_val = coerced;
    } else if (big) {
        BigInt& a = *old_val.as_bigint();
        BigInt& b = *coerced.as_bigint();
        switch (op) {
            case RmwOp::Add: new_val = Value(new BigInt(a + b)); break;
            case RmwOp::And: new_val = Value(new BigInt(a.bitwise_and(b))); break;
            case RmwOp::Or:  new_val = Value(new BigInt(a.bitwise_or(b))); break;
            case RmwOp::Xor: new_val = Value(new BigInt(a.bitwise_xor(b))); break;
            case RmwOp::Sub: new_val = Value(new BigInt(a - b)); break;
            default: break;
        }
    } else {
        int64_t a = static_cast<int64_t>(old_val.to_number());
        int64_t b = static_cast<int64_t>(coerced.to_number());
        int64_t r = 0;
        switch (op) {
            case RmwOp::Add: r = a + b; break;
            case RmwOp::And: r = a & b; break;
            case RmwOp::Or:  r = a | b; break;
            case RmwOp::Xor: r = a ^ b; break;
            case RmwOp::Sub: r = a - b; break;
            default: break;
        }
        new_val = Value(static_cast<double>(r));
    }
    ta->set_element(idx, new_val);
    return old_val;
}

Value atomics_compare_exchange(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], false);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool big = is_bigint_array_type(ta->get_array_type());
    Value expected_raw = args.size() > 2 ? args[2] : Value();
    Value replacement_raw = args.size() > 3 ? args[3] : Value();
    Value expected = big ? to_bigint_value(ctx, expected_raw) : Value(to_number_throwing(ctx, expected_raw));
    if (ctx.has_exception()) return Value();
    Value replacement = big ? to_bigint_value(ctx, replacement_raw) : Value(to_number_throwing(ctx, replacement_raw));
    if (ctx.has_exception()) return Value();

    size_t idx = static_cast<size_t>(idx_d);
    Value old_val = ta->get_element(idx);
    bool equal;
    if (big) {
        Value expected_canon = truncate_bigint_to_element_type(*expected.as_bigint(), ta->get_array_type());
        equal = (*old_val.as_bigint() == *expected_canon.as_bigint());
    } else {
        equal = (old_val.to_number() == truncate_to_element_type(expected.to_number(), ta->get_array_type()));
    }
    if (equal) ta->set_element(idx, replacement);
    return old_val;
}

Value atomics_load(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], false);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    return ta->get_element(static_cast<size_t>(idx_d));
}

Value atomics_store(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], false);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool big = is_bigint_array_type(ta->get_array_type());
    Value raw = args.size() > 2 ? args[2] : Value();
    // Spec: store's v is ToInteger(value), not width-truncated -- that only happens on write.
    Value coerced = big ? to_bigint_value(ctx, raw) : Value(to_integer_or_infinity(ctx, raw));
    if (ctx.has_exception()) return Value();
    size_t idx = static_cast<size_t>(idx_d);
    ta->set_element(idx, coerced);
    return coerced;
}

Value atomics_is_lock_free(Context& ctx, const std::vector<Value>& args) {
    double n = to_integer_or_infinity(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    return Value(n == 1.0 || n == 2.0 || n == 4.0 || n == 8.0);
}

// Atomics.wait: nothing can ever notify a blocking wait here, so a mismatch returns
// "not-equal" and a match sleeps out the (capped) timeout as "timed-out".
Value atomics_wait(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], true);
    if (!ta) return Value();
    if (!ta->buffer() || !ta->buffer()->is_shared_array_buffer()) {
        ctx.throw_type_error("Atomics.wait requires a SharedArrayBuffer-backed TypedArray");
        return Value();
    }
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool big = is_bigint_array_type(ta->get_array_type());
    Value raw_value = args.size() > 2 ? args[2] : Value();
    Value expected = big ? to_bigint_value(ctx, raw_value) : Value(to_number_throwing(ctx, raw_value));
    if (ctx.has_exception()) return Value();
    double timeout_raw = to_number_throwing(ctx, args.size() > 3 ? args[3] : Value());
    if (ctx.has_exception()) return Value();
    double timeout_ms = std::isnan(timeout_raw) ? std::numeric_limits<double>::infinity() : std::max(timeout_raw, 0.0);

    size_t idx = static_cast<size_t>(idx_d);
    Value current = ta->get_element(idx);
    bool equal;
    if (big) {
        Value expected_canon = truncate_bigint_to_element_type(*expected.as_bigint(), ta->get_array_type());
        equal = (*current.as_bigint() == *expected_canon.as_bigint());
    } else {
        equal = (current.to_number() == truncate_to_element_type(expected.to_number(), ta->get_array_type()));
    }
    if (!equal) return Value(std::string("not-equal"));

    double sleep_ms = std::isinf(timeout_ms) ? 1000.0 : std::min(timeout_ms, 1000.0);
    if (sleep_ms > 0.0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleep_ms));
    return Value(std::string("timed-out"));
}

// Atomics.notify: wakes up to `count` same-agent waitAsync promises at this buffer position.
Value atomics_notify(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], true);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    Value count_val = args.size() > 2 ? args[2] : Value();
    double count = std::numeric_limits<double>::infinity();
    if (!count_val.is_undefined()) {
        count = to_integer_or_infinity(ctx, count_val);
        if (ctx.has_exception()) return Value();
        count = std::max(count, 0.0);
    }

    ArrayBuffer* buffer = ta->buffer();
    size_t byte_index = ta->byte_offset() + static_cast<size_t>(idx_d) * ta->bytes_per_element();
    double woken = 0.0;
    auto& reg = pending_waiters();
    for (auto it = reg.begin(); it != reg.end() && woken < count;) {
        if (it->buffer == buffer && it->byte_index == byte_index) {
            it->promise->fulfill(Value(std::string("ok")));
            it = reg.erase(it);
            woken += 1.0;
        } else {
            ++it;
        }
    }
    return Value(woken);
}

// Atomics.waitAsync: sync {value:"not-equal"} on mismatch, sync {value:"timed-out"} when
// timeout <= 0, otherwise a pending promise resolvable by a same-agent notify() (see above).
Value atomics_wait_async(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], true);
    if (!ta) return Value();
    if (!ta->buffer() || !ta->buffer()->is_shared_array_buffer()) {
        ctx.throw_type_error("Atomics.waitAsync requires a SharedArrayBuffer-backed TypedArray");
        return Value();
    }
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool big = is_bigint_array_type(ta->get_array_type());
    Value raw_value = args.size() > 2 ? args[2] : Value();
    Value expected = big ? to_bigint_value(ctx, raw_value) : Value(to_number_throwing(ctx, raw_value));
    if (ctx.has_exception()) return Value();
    double timeout_raw = to_number_throwing(ctx, args.size() > 3 ? args[3] : Value());
    if (ctx.has_exception()) return Value();
    double timeout_ms = std::isnan(timeout_raw) ? std::numeric_limits<double>::infinity() : std::max(timeout_raw, 0.0);

    size_t idx = static_cast<size_t>(idx_d);
    Value current = ta->get_element(idx);
    bool equal;
    if (big) {
        Value expected_canon = truncate_bigint_to_element_type(*expected.as_bigint(), ta->get_array_type());
        equal = (*current.as_bigint() == *expected_canon.as_bigint());
    } else {
        equal = (current.to_number() == truncate_to_element_type(expected.to_number(), ta->get_array_type()));
    }

    auto result = ObjectFactory::create_object();
    if (!equal) {
        result->set_property("async", Value(false));
        result->set_property("value", Value(std::string("not-equal")));
        return Value(result.release());
    }
    if (timeout_ms <= 0.0) {
        result->set_property("async", Value(false));
        result->set_property("value", Value(std::string("timed-out")));
        return Value(result.release());
    }

    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* promise = static_cast<Promise*>(promise_obj.release());
    size_t byte_index = ta->byte_offset() + idx * ta->bytes_per_element();
    pending_waiters().push_back({ta->buffer(), byte_index, promise});
    result->set_property("async", Value(true));
    result->set_property("value", Value(promise));
    return Value(result.release());
}

Value atomics_pause(Context& ctx, const std::vector<Value>& args) {
    if (!args.empty() && !args[0].is_undefined()) {
        if (!args[0].is_number()) { ctx.throw_type_error("Atomics.pause: iterationNumber must be a Number"); return Value(); }
        double n = args[0].as_number();
        if (std::isnan(n) || std::isinf(n) || n != std::trunc(n)) {
            ctx.throw_type_error("Atomics.pause: iterationNumber must be an integer");
            return Value();
        }
    }
    return Value();
}

} // namespace

void trace_atomics_gc_roots(Visitor& v) {
    for (const auto& w : pending_waiters()) {
        v.visit_object(w.buffer);
        v.visit_object(w.promise);
    }
}

void register_atomics_builtins(Context& ctx) {
    auto atomics_obj = ObjectFactory::create_object();

    struct Entry {
        const char* name;
        Value (*fn)(Context&, const std::vector<Value>&);
        uint32_t arity;
    };

    static const Entry entries[] = {
        {"compareExchange", atomics_compare_exchange, 4},
        {"load", atomics_load, 2},
        {"store", atomics_store, 3},
        {"isLockFree", atomics_is_lock_free, 1},
        {"wait", atomics_wait, 4},
        {"waitAsync", atomics_wait_async, 4},
        {"notify", atomics_notify, 3},
        {"pause", atomics_pause, 0},
    };
    for (const Entry& e : entries) {
        auto fn = ObjectFactory::create_native_function(e.name, e.fn, e.arity);
        atomics_obj->set_property(e.name, Value(fn.release()), PropertyAttributes::BuiltinFunction);
    }

    auto add_rmw = [&](const char* name, RmwOp op) {
        auto fn = ObjectFactory::create_native_function(name,
            [op](Context& ctx, const std::vector<Value>& args) -> Value {
                return atomic_read_modify_write(ctx, args, op);
            }, 3);
        atomics_obj->set_property(name, Value(fn.release()), PropertyAttributes::BuiltinFunction);
    };
    add_rmw("add", RmwOp::Add);
    add_rmw("and", RmwOp::And);
    add_rmw("or", RmwOp::Or);
    add_rmw("xor", RmwOp::Xor);
    add_rmw("sub", RmwOp::Sub);
    add_rmw("exchange", RmwOp::Exchange);

    Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (tag_sym) {
        PropertyDescriptor tag_desc(Value(std::string("Atomics")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        atomics_obj->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
    }

    ctx.register_built_in_object("Atomics", atomics_obj.release());
}

} // namespace Quanta
