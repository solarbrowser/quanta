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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
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

// Pending waitAsync promises, keyed by element address. Thread-local: a
// promise is a GC cell of its own agent, only a same-agent notify may touch it.
struct PendingWaiter {
    const void* addr;
    Promise* promise;
};

std::vector<PendingWaiter>& pending_waiters() {
    static thread_local std::vector<PendingWaiter> registry;
    return registry;
}

// Blocked Atomics.wait calls, process-wide: a notify in any agent must wake
// them. The mutex also covers wait's value re-check, so a concurrent
// store+notify cannot slip in before enrollment (no lost wakeup).
struct BlockingWaiter {
    const void* addr = nullptr;
    std::condition_variable cv;
    bool notified = false;
};

std::mutex& blocking_waiter_mutex() {
    static std::mutex m;
    return m;
}

std::vector<BlockingWaiter*>& blocking_waiters() {
    static std::vector<BlockingWaiter*> waiters;
    return waiters;
}

enum class RmwOp { Add, And, Or, Xor, Sub, Exchange };

uint8_t* atomic_element_ptr(TypedArrayBase* ta, size_t idx) {
    return ta->buffer()->data() + ta->byte_offset() + idx * ta->bytes_per_element();
}

// The value/expected coercion can run arbitrary JS (valueOf) that detaches
// or shrinks the buffer; the raw pointer access needs the bounds re-proved.
bool revalidate_atomic_access(Context& ctx, TypedArrayBase* ta, size_t idx) {
    ArrayBuffer* buf = ta->buffer();
    if (!buf || buf->is_detached() || !buf->data()) {
        ctx.throw_type_error("Atomics: buffer is detached");
        return false;
    }
    if (ta->is_out_of_bounds() || idx >= ta->length()) {
        ctx.throw_range_error("Atomics access index out of range");
        return false;
    }
    return true;
}

// Coerced JS operand -> the element's raw bits in an int64; pairs with
// atomic_load_bits so bit patterns compare directly.
int64_t operand_bits(const Value& coerced, AT type) {
    if (is_bigint_array_type(type)) return coerced.as_bigint()->to_int64();
    return static_cast<int64_t>(truncate_to_element_type(coerced.to_number(), type));
}

template <typename T>
T atomic_rmw_bits(uint8_t* addr, RmwOp op, T operand) {
    T* p = reinterpret_cast<T*>(addr);
    switch (op) {
        case RmwOp::Add:      return __atomic_fetch_add(p, operand, __ATOMIC_SEQ_CST);
        case RmwOp::Sub:      return __atomic_fetch_sub(p, operand, __ATOMIC_SEQ_CST);
        case RmwOp::And:      return __atomic_fetch_and(p, operand, __ATOMIC_SEQ_CST);
        case RmwOp::Or:       return __atomic_fetch_or(p, operand, __ATOMIC_SEQ_CST);
        case RmwOp::Xor:      return __atomic_fetch_xor(p, operand, __ATOMIC_SEQ_CST);
        case RmwOp::Exchange: return __atomic_exchange_n(p, operand, __ATOMIC_SEQ_CST);
    }
    return 0;
}

// Runs fn with T bound to the element's C type. The spec forces byteOffset %
// element size == 0, so the raw pointer is always aligned for __atomic ops.
template <typename Fn>
Value atomic_with_element_type(AT type, Fn&& fn) {
    switch (type) {
        case AT::INT8:      return Value(static_cast<double>(fn(int8_t{})));
        case AT::UINT8:     return Value(static_cast<double>(fn(uint8_t{})));
        case AT::INT16:     return Value(static_cast<double>(fn(int16_t{})));
        case AT::UINT16:    return Value(static_cast<double>(fn(uint16_t{})));
        case AT::INT32:     return Value(static_cast<double>(fn(int32_t{})));
        case AT::UINT32:    return Value(static_cast<double>(fn(uint32_t{})));
        case AT::BIGINT64:  return Value(new BigInt(static_cast<int64_t>(fn(int64_t{}))));
        case AT::BIGUINT64: return Value(new BigInt(std::to_string(static_cast<uint64_t>(fn(uint64_t{})))));
        default:            return Value();
    }
}

// Sign/zero-extended-to-int64 seq_cst load (wait/waitAsync equality checks).
int64_t atomic_load_bits(uint8_t* addr, AT type) {
    switch (type) {
        case AT::INT8:      return __atomic_load_n(reinterpret_cast<int8_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::UINT8:     return __atomic_load_n(reinterpret_cast<uint8_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::INT16:     return __atomic_load_n(reinterpret_cast<int16_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::UINT16:    return __atomic_load_n(reinterpret_cast<uint16_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::INT32:     return __atomic_load_n(reinterpret_cast<int32_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::UINT32:    return __atomic_load_n(reinterpret_cast<uint32_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::BIGINT64:  return __atomic_load_n(reinterpret_cast<int64_t*>(addr), __ATOMIC_SEQ_CST);
        case AT::BIGUINT64: return static_cast<int64_t>(__atomic_load_n(reinterpret_cast<uint64_t*>(addr), __ATOMIC_SEQ_CST));
        default:            return 0;
    }
}

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
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    int64_t bits = operand_bits(coerced, ta->get_array_type());
    return atomic_with_element_type(ta->get_array_type(), [&](auto tag) {
        using T = decltype(tag);
        return atomic_rmw_bits<T>(addr, op, static_cast<T>(bits));
    });
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
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    int64_t expected_bits = operand_bits(expected, ta->get_array_type());
    int64_t replacement_bits = operand_bits(replacement, ta->get_array_type());
    return atomic_with_element_type(ta->get_array_type(), [&](auto tag) {
        using T = decltype(tag);
        // After the CAS, `witnessed` holds the old element value either way
        // (rewritten on failure, untouched on success) -- the spec's result.
        T witnessed = static_cast<T>(expected_bits);
        __atomic_compare_exchange_n(reinterpret_cast<T*>(addr), &witnessed,
                                    static_cast<T>(replacement_bits), false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return witnessed;
    });
}

Value atomics_load(Context& ctx, const std::vector<Value>& args) {
    TypedArrayBase* ta = validate_integer_typed_array(ctx, args.empty() ? Value() : args[0], false);
    if (!ta) return Value();
    double idx_d = validate_atomic_access(ctx, ta, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    size_t idx = static_cast<size_t>(idx_d);
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    return atomic_with_element_type(ta->get_array_type(), [&](auto tag) {
        using T = decltype(tag);
        return __atomic_load_n(reinterpret_cast<T*>(addr), __ATOMIC_SEQ_CST);
    });
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
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    // The written bits are width-truncated (infinity -> 0); the return
    // value is the un-truncated coercion.
    double for_bits = coerced.is_bigint() ? 0.0 : coerced.to_number();
    int64_t bits = coerced.is_bigint()
        ? coerced.as_bigint()->to_int64()
        : static_cast<int64_t>(truncate_to_element_type(for_bits, ta->get_array_type()));
    atomic_with_element_type(ta->get_array_type(), [&](auto tag) {
        using T = decltype(tag);
        __atomic_store_n(reinterpret_cast<T*>(addr), static_cast<T>(bits), __ATOMIC_SEQ_CST);
        return T{};
    });
    return coerced;
}

Value atomics_is_lock_free(Context& ctx, const std::vector<Value>& args) {
    double n = to_integer_or_infinity(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    return Value(n == 1.0 || n == 2.0 || n == 4.0 || n == 8.0);
}

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
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    int64_t expected_bits = operand_bits(expected, ta->get_array_type());

    std::unique_lock<std::mutex> lock(blocking_waiter_mutex());
    if (atomic_load_bits(addr, ta->get_array_type()) != expected_bits) {
        return Value(std::string("not-equal"));
    }
    if (timeout_ms <= 0.0) {
        return Value(std::string("timed-out"));
    }

    BlockingWaiter self;
    self.addr = addr;
    blocking_waiters().push_back(&self);
    if (std::isinf(timeout_ms)) {
        self.cv.wait(lock, [&] { return self.notified; });
    } else {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double, std::milli>(timeout_ms));
        self.cv.wait_until(lock, deadline, [&] { return self.notified; });
    }
    auto& waiters = blocking_waiters();
    waiters.erase(std::find(waiters.begin(), waiters.end(), &self));
    return Value(std::string(self.notified ? "ok" : "timed-out"));
}

// Atomics.notify: wakes up to `count` blocked waits (any agent), then up to
// the remainder of `count` same-agent waitAsync promises, at this address.
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

    const void* addr = atomic_element_ptr(ta, static_cast<size_t>(idx_d));
    double woken = 0.0;
    {
        std::lock_guard<std::mutex> lock(blocking_waiter_mutex());
        for (BlockingWaiter* w : blocking_waiters()) {
            if (woken >= count) break;
            if (w->addr == addr && !w->notified) {
                w->notified = true;
                w->cv.notify_one();
                woken += 1.0;
            }
        }
    }
    auto& reg = pending_waiters();
    for (auto it = reg.begin(); it != reg.end() && woken < count;) {
        if (it->addr == addr) {
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
    if (!revalidate_atomic_access(ctx, ta, idx)) return Value();

    uint8_t* addr = atomic_element_ptr(ta, idx);
    bool equal = atomic_load_bits(addr, ta->get_array_type()) ==
                 operand_bits(expected, ta->get_array_type());

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
    pending_waiters().push_back({addr, promise});
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
