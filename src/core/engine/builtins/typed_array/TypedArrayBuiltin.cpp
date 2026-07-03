/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/TypedArrayBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/engine/builtins/ObjectBuiltin.h"
#include <cmath>
#include "quanta/parser/AST.h"
#include <algorithm>

namespace Quanta {

static void register_uint8array_base64_hex(Context& ctx); // forward declaration

// ToString via ToPrimitive("string"): Value::to_string() for objects ignores user toString.
static std::string to_string_via_toprimitive(Context& ctx, const Value& v) {
    if (v.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return ""; }
    if (!v.is_object() && !v.is_function()) return v.to_string();
    Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
    Value toString_fn = obj->get_property("toString");
    if (ctx.has_exception()) return "";
    if (toString_fn.is_function()) {
        Value prim = toString_fn.as_function()->call(ctx, {}, v);
        if (ctx.has_exception()) return "";
        if (!prim.is_object() && !prim.is_function()) return prim.to_string();
    }
    Value valueOf_fn = obj->get_property("valueOf");
    if (ctx.has_exception()) return "";
    if (valueOf_fn.is_function()) {
        Value prim = valueOf_fn.as_function()->call(ctx, {}, v);
        if (ctx.has_exception()) return "";
        if (!prim.is_object() && !prim.is_function()) return prim.to_string();
    }
    ctx.throw_type_error("Cannot convert object to primitive value");
    return "";
}

// Value::to_number() returns NaN for Symbol/BigInt instead of throwing (no Context access there).
static double to_number_throwing(Context& ctx, const Value& v) {
    if (v.is_symbol() || v.is_bigint()) {
        ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
        return 0.0;
    }
    return v.to_number();
}

// ToIndex: NaN/+-0 -> 0, throws RangeError outside [0, 2^53-1], TypeError for Symbol/BigInt.
static double to_index_checked(Context& ctx, const Value& v) {
    double number = to_number_throwing(ctx, v);
    if (ctx.has_exception()) return 0;
    if (std::isnan(number) || number == 0) return 0;
    double integer = (number < 0) ? std::ceil(number) : std::floor(number);
    if (integer < 0 || integer > 9007199254740991.0) {
        ctx.throw_range_error("Invalid index");
        return 0;
    }
    return integer;
}

// ValidateTypedArray: throws if `this` isn't a typed array, or its buffer is detached.
static TypedArrayBase* validate_typed_array(Context& ctx, Object* this_obj) {
    if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return nullptr; }
    TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
    if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return nullptr; }
    return ta;
}

static const char* default_typed_array_ctor_name(TypedArrayBase::ArrayType t) {
    switch (t) {
        case TypedArrayBase::ArrayType::INT8: return "Int8Array";
        case TypedArrayBase::ArrayType::UINT8: return "Uint8Array";
        case TypedArrayBase::ArrayType::UINT8_CLAMPED: return "Uint8ClampedArray";
        case TypedArrayBase::ArrayType::INT16: return "Int16Array";
        case TypedArrayBase::ArrayType::UINT16: return "Uint16Array";
        case TypedArrayBase::ArrayType::INT32: return "Int32Array";
        case TypedArrayBase::ArrayType::UINT32: return "Uint32Array";
        case TypedArrayBase::ArrayType::FLOAT32: return "Float32Array";
        case TypedArrayBase::ArrayType::FLOAT64: return "Float64Array";
        case TypedArrayBase::ArrayType::BIGINT64: return "BigInt64Array";
        case TypedArrayBase::ArrayType::BIGUINT64: return "BigUint64Array";
        default: return nullptr;
    }
}

// SpeciesConstructor(exemplar, default ctor associated with exemplar's array type).
static Function* get_typed_array_species_constructor(Context& ctx, TypedArrayBase* exemplar) {
    const char* default_name = default_typed_array_ctor_name(exemplar->get_array_type());
    if (!default_name) { ctx.throw_type_error("Unsupported type"); return nullptr; }
    Value default_ctor = ctx.get_binding(default_name);
    Function* ctor_fn = default_ctor.is_function() ? default_ctor.as_function() : nullptr;

    Value c = exemplar->get_property("constructor");
    if (ctx.has_exception()) return nullptr;
    if (!c.is_undefined()) {
        if (!c.is_object() && !c.is_function()) { ctx.throw_type_error("constructor property is not a constructor"); return nullptr; }
        Object* c_obj = c.is_function() ? static_cast<Object*>(c.as_function()) : c.as_object();
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        Value s = species_sym ? c_obj->get_property(species_sym->to_property_key()) : Value();
        if (ctx.has_exception()) return nullptr;
        if (!s.is_null() && !s.is_undefined()) {
            if (s.is_function() && static_cast<Function*>(s.as_function())->is_constructor()) {
                ctor_fn = s.as_function();
            } else {
                ctx.throw_type_error("Species constructor is not a constructor");
                return nullptr;
            }
        }
    }
    if (!ctor_fn) { ctx.throw_type_error("No constructor available"); return nullptr; }
    return ctor_fn;
}

// TypedArraySpeciesCreate: SpeciesConstructor lookup then Construct + ValidateTypedArray + content-type check.
static TypedArrayBase* typed_array_species_create(Context& ctx, TypedArrayBase* exemplar, const std::vector<Value>& ctor_args) {
    Function* ctor_fn = get_typed_array_species_constructor(ctx, exemplar);
    if (!ctor_fn) return nullptr;

    Value result = ctor_fn->construct(ctx, ctor_args);
    if (ctx.has_exception()) return nullptr;
    if (!result.is_object() || !result.as_object()->is_typed_array()) {
        ctx.throw_type_error("Species constructor did not return a TypedArray");
        return nullptr;
    }
    TypedArrayBase* result_ta = static_cast<TypedArrayBase*>(result.as_object());
    if (result_ta->is_out_of_bounds()) {
        ctx.throw_type_error("TypedArray is out of bounds");
        return nullptr;
    }
    auto is_big = [](TypedArrayBase::ArrayType t) {
        return t == TypedArrayBase::ArrayType::BIGINT64 || t == TypedArrayBase::ArrayType::BIGUINT64;
    };
    if (is_big(exemplar->get_array_type()) != is_big(result_ta->get_array_type())) {
        ctx.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
        return nullptr;
    }
    return result_ta;
}

// TypedArrayCreate: for the single-length form, species ctor must not return a shorter-than-requested array.
static TypedArrayBase* typed_array_species_create(Context& ctx, TypedArrayBase* exemplar, size_t length) {
    TypedArrayBase* result_ta = typed_array_species_create(ctx, exemplar, std::vector<Value>{ Value(static_cast<double>(length)) });
    if (!result_ta) return nullptr;
    if (result_ta->length() < length) {
        ctx.throw_type_error("Derived TypedArray constructor created an array which was too small");
        return nullptr;
    }
    return result_ta;
}

// ToIntegerOrInfinity: NaN -> 0, ±Infinity passes through, finite numbers truncate toward zero.
// The result is a mathematical value, so -0 normalizes to +0.
static double to_integer_or_infinity(double num) {
    if (std::isnan(num)) return 0.0;
    if (std::isinf(num)) return num;
    double r = num < 0 ? -std::floor(-num) : std::floor(num);
    return r == 0.0 ? 0.0 : r;
}

// Clamp a relative index to [0, len]: negative wraps from end, Infinity clamps to len.
static double clamp_relative_index(double relative, double len) {
    return relative < 0 ? std::max(len + relative, 0.0) : std::min(relative, len);
}

// TypedArrayCreateSameType: ignores any user-overridden .constructor/@@species, using the intrinsic constructor for exemplar.[[TypedArrayName]]. The result's prototype must be wired explicitly since this bypasses Function::construct()'s usual post-call prototype fixup.
static TypedArrayBase* create_same_type_typed_array(Context& ctx, TypedArrayBase* ta, size_t len) {
    TypedArrayBase* result = nullptr;
    switch (ta->get_array_type()) {
        case TypedArrayBase::ArrayType::INT8: result = TypedArrayFactory::create_int8_array(len).release(); break;
        case TypedArrayBase::ArrayType::UINT8: result = TypedArrayFactory::create_uint8_array(len).release(); break;
        case TypedArrayBase::ArrayType::UINT8_CLAMPED: result = TypedArrayFactory::create_uint8_clamped_array(len).release(); break;
        case TypedArrayBase::ArrayType::INT16: result = TypedArrayFactory::create_int16_array(len).release(); break;
        case TypedArrayBase::ArrayType::UINT16: result = TypedArrayFactory::create_uint16_array(len).release(); break;
        case TypedArrayBase::ArrayType::INT32: result = TypedArrayFactory::create_int32_array(len).release(); break;
        case TypedArrayBase::ArrayType::UINT32: result = TypedArrayFactory::create_uint32_array(len).release(); break;
        case TypedArrayBase::ArrayType::FLOAT32: result = TypedArrayFactory::create_float32_array(len).release(); break;
        case TypedArrayBase::ArrayType::FLOAT64: result = TypedArrayFactory::create_float64_array(len).release(); break;
        case TypedArrayBase::ArrayType::BIGINT64: result = new BigInt64Array(len); break;
        case TypedArrayBase::ArrayType::BIGUINT64: result = new BigUint64Array(len); break;
        default: ctx.throw_type_error("Unsupported type"); return nullptr;
    }
    Object* ctor = ctx.get_built_in_object(TypedArrayBase::array_type_to_string(ta->get_array_type()));
    if (ctor) {
        Value proto = ctor->get_property("prototype");
        if (proto.is_object()) result->set_prototype(proto.as_object());
    }
    return result;
}

// ToBigInt(argument): BigInt/Boolean/String coerce directly; Object goes through ToPrimitive; others throw.
static Value to_bigint_for_typed_array(Context& ctx, const Value& value) {
    if (value.is_bigint()) return value;
    if (value.is_boolean()) return Value(new BigInt(value.as_boolean() ? 1 : 0));
    if (value.is_string()) {
        try {
            return Value(new BigInt(value.to_string()));
        } catch (const std::exception&) {
            ctx.throw_syntax_error("Cannot convert string to BigInt");
            return Value();
        }
    }
    if (value.is_object()) {
        Object* obj = value.as_object();
        // ToPrimitive(argument, hint Number): @@toPrimitive("number") first, then valueOf, then toString.
        Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
        if (toPrim_sym) {
            Value toPrim = obj->get_property(toPrim_sym->to_property_key());
            if (ctx.has_exception()) return Value();
            if (!toPrim.is_null() && !toPrim.is_undefined()) {
                if (!toPrim.is_function()) { ctx.throw_type_error("@@toPrimitive is not callable"); return Value(); }
                Value prim = toPrim.as_function()->call(ctx, {Value(std::string("number"))}, value);
                if (ctx.has_exception()) return Value();
                if (prim.is_object() || prim.is_function()) { ctx.throw_type_error("Cannot convert object to BigInt"); return Value(); }
                return to_bigint_for_typed_array(ctx, prim);
            }
        }
        Value valueOf_fn = obj->get_property("valueOf");
        if (valueOf_fn.is_function()) {
            Value prim = valueOf_fn.as_function()->call(ctx, {}, value);
            if (ctx.has_exception()) return Value();
            if (!prim.is_object() && !prim.is_function()) return to_bigint_for_typed_array(ctx, prim);
        }
        Value toString_fn = obj->get_property("toString");
        if (toString_fn.is_function()) {
            Value prim = toString_fn.as_function()->call(ctx, {}, value);
            if (ctx.has_exception()) return Value();
            if (!prim.is_object() && !prim.is_function()) return to_bigint_for_typed_array(ctx, prim);
        }
        ctx.throw_type_error("Cannot convert object to BigInt");
        return Value();
    }
    ctx.throw_type_error("Cannot convert " + std::string(value.is_number() ? "Number" :
        (value.is_symbol() ? "Symbol" : (value.is_null() ? "null" : "undefined"))) + " to BigInt");
    return Value();
}

// Shared `new XArray(buffer[, byteOffset[, length]])` handling for all 11 typed array
// constructors. Byte-offset/length bounds and alignment are validated by TypedArrayBase's own
// constructor (via validate_offset_and_length); this just parses the JS arguments and converts
// any resulting C++ range_error into the corresponding JS RangeError. Returns nullptr (with no
// exception set) if args[0] isn't an ArrayBuffer, so callers fall through to their other cases.
static std::unique_ptr<TypedArrayBase> construct_typed_array_from_buffer_arg(
        Context& ctx, const std::vector<Value>& args,
        TypedArrayBase::ArrayType type, size_t bytes_per_element) {
    if (args.empty() || !args[0].is_object() || !args[0].as_object()->is_array_buffer()) {
        return nullptr;
    }
    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(args[0].as_object());
    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});

    // Per spec, ToIndex(byteOffset) and ToIndex(length) both happen *before* the detached-buffer
    // check, since either conversion can have side effects (e.g. detach the buffer via valueOf).
    double offset_double = to_index_checked(ctx, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return nullptr;
    size_t byte_offset = static_cast<size_t>(offset_double);

    size_t length = SIZE_MAX;
    if (args.size() > 2 && !args[2].is_undefined()) {
        double length_double = to_index_checked(ctx, args[2]);
        if (ctx.has_exception()) return nullptr;
        length = static_cast<size_t>(length_double);
    }

    if (buffer->is_detached()) {
        ctx.throw_type_error("Cannot construct typed array with a detached ArrayBuffer");
        return nullptr;
    }

    try {
        return TypedArrayFactory::create_from_buffer(type, shared_buffer, byte_offset, length);
    } catch (const std::exception& e) {
        ctx.throw_range_error(e.what());
        return nullptr;
    }
}

static std::unique_ptr<TypedArrayBase> create_typed_array_of_type(TypedArrayBase::ArrayType type, size_t length) {
    switch (type) {
        case TypedArrayBase::ArrayType::INT8: return TypedArrayFactory::create_int8_array(length);
        case TypedArrayBase::ArrayType::UINT8: return TypedArrayFactory::create_uint8_array(length);
        case TypedArrayBase::ArrayType::UINT8_CLAMPED: return TypedArrayFactory::create_uint8_clamped_array(length);
        case TypedArrayBase::ArrayType::INT16: return TypedArrayFactory::create_int16_array(length);
        case TypedArrayBase::ArrayType::UINT16: return TypedArrayFactory::create_uint16_array(length);
        case TypedArrayBase::ArrayType::INT32: return TypedArrayFactory::create_int32_array(length);
        case TypedArrayBase::ArrayType::UINT32: return TypedArrayFactory::create_uint32_array(length);
        case TypedArrayBase::ArrayType::FLOAT32: return TypedArrayFactory::create_float32_array(length);
        case TypedArrayBase::ArrayType::FLOAT64: return TypedArrayFactory::create_float64_array(length);
        case TypedArrayBase::ArrayType::BIGINT64: return std::make_unique<BigInt64Array>(length);
        case TypedArrayBase::ArrayType::BIGUINT64: return std::make_unique<BigUint64Array>(length);
    }
    return nullptr;
}

// Shared `new XArray(arg)` handling for all 11 typed array constructors (buffer-arg is handled
// separately above), so the spec-mandated dispatch order is implemented once, not 11 times.
static Value construct_typed_array_generic(Context& ctx, const std::vector<Value>& args,
        TypedArrayBase::ArrayType type, size_t bytes_per_element) {
    if (args.empty()) {
        return Value(create_typed_array_of_type(type, 0).release());
    }

    // Function is a distinct Value tag from Object here, but a JS function IS an object for
    // the purposes of this dispatch (Function : public Object), so treat both tags the same.
    bool arg0_is_object_like = args[0].is_object() || args[0].is_function();
    Object* arg0_obj = arg0_is_object_like ? args[0].as_object() : nullptr;

    if (arg0_obj && arg0_obj->is_array_buffer()) {
        auto ta = construct_typed_array_from_buffer_arg(ctx, args, type, bytes_per_element);
        if (ctx.has_exception()) return Value();
        return Value(ta.release());
    }

    if (arg0_obj && arg0_obj->is_typed_array()) {
        TypedArrayBase* source = static_cast<TypedArrayBase*>(arg0_obj);
        bool dest_is_bigint = (type == TypedArrayBase::ArrayType::BIGINT64 || type == TypedArrayBase::ArrayType::BIGUINT64);
        TypedArrayBase::ArrayType src_type = source->get_array_type();
        bool src_is_bigint = (src_type == TypedArrayBase::ArrayType::BIGINT64 || src_type == TypedArrayBase::ArrayType::BIGUINT64);
        if (dest_is_bigint != src_is_bigint) {
            ctx.throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
            return Value();
        }
        if (source->is_out_of_bounds()) {
            ctx.throw_type_error("Source TypedArray is out of bounds");
            return Value();
        }
        size_t length = source->length();
        auto typed_array = create_typed_array_of_type(type, length);
        for (size_t i = 0; i < length; i++) {
            typed_array->set_element(i, source->get_element(i));
            if (ctx.has_exception()) return Value();
        }
        return Value(typed_array.release());
    }

    if (arg0_obj) {
        Object* obj = arg0_obj;

        Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
        Value iter_fn = iter_sym ? obj->get_property(iter_sym->to_property_key()) : Value();
        if (ctx.has_exception()) return Value();
        // GetMethod: a present-but-non-callable @@iterator (and not null/undefined) throws.
        if (!iter_fn.is_undefined() && !iter_fn.is_null() && !iter_fn.is_function()) {
            ctx.throw_type_error("Symbol.iterator is not a function");
            return Value();
        }
        if (iter_fn.is_function()) {
            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
            if (!iter_call_ctx) iter_call_ctx = &ctx;
            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
            if (iter_call_ctx->has_exception()) {
                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception(), true);
                return Value();
            }
            std::vector<Value> items;
            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
            while (it) {
                Value next_fn = it->get_property("next");
                if (!next_fn.is_function()) break;
                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                if (!next_call_ctx) next_call_ctx = &ctx;
                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                if (next_call_ctx->has_exception()) {
                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception(), true);
                    return Value();
                }
                if (!res.is_object()) break;
                if (res.as_object()->get_property("done").to_boolean()) break;
                items.push_back(res.as_object()->get_property("value"));
            }
            auto ta = create_typed_array_of_type(type, items.size());
            for (size_t i = 0; i < items.size(); i++) {
                ta->set_element(i, items[i]);
                if (ctx.has_exception()) return Value();
            }
            return Value(ta.release());
        }

        // Array-like fallback (no @@iterator): live Get(i)/Set(i) per InitializeTypedArrayFromArrayLike.
        Value length_val = obj->get_property("length");
        if (ctx.has_exception()) return Value();
        if (length_val.is_symbol() || length_val.is_bigint()) {
            ctx.throw_type_error("Cannot convert length to a number");
            return Value();
        }
        double length_num = length_val.to_number();
        if (ctx.has_exception()) return Value();
        double clamped = std::isnan(length_num) ? 0.0 : std::min(std::max(length_num, 0.0), 9007199254740991.0);
        if (clamped * static_cast<double>(bytes_per_element) > 1024.0 * 1024.0 * 1024.0) {
            ctx.throw_range_error("Invalid typed array length");
            return Value();
        }
        size_t length = static_cast<size_t>(clamped);

        auto typed_array = create_typed_array_of_type(type, length);
        for (size_t i = 0; i < length; i++) {
            Value element = obj->get_property(std::to_string(i));
            if (ctx.has_exception()) return Value();
            typed_array->set_element(i, element);
            if (ctx.has_exception()) return Value();
        }
        return Value(typed_array.release());
    }

    // length-arg: ToIndex(args[0]), then a memory-sanity cap matching ArrayBuffer's own cap.
    double idx = to_index_checked(ctx, args[0]);
    if (ctx.has_exception()) return Value();
    if (idx * static_cast<double>(bytes_per_element) > 1024.0 * 1024.0 * 1024.0) {
        ctx.throw_range_error("Invalid typed array length");
        return Value();
    }
    return Value(create_typed_array_of_type(type, static_cast<size_t>(idx)).release());
}

void register_typed_array_builtins(Context& ctx) {
    auto uint8array_constructor = ObjectFactory::create_native_constructor("Uint8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::UINT8, 1);
        }, 3);
    // ES2025: Uint8Array.fromBase64 / fromHex static methods
    {
        // fromHex(str) -> Uint8Array
        auto fromHex_fn = ObjectFactory::create_native_function("fromHex",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.empty() || !args[0].is_string()) { ctx.throw_type_error("fromHex requires a string"); return Value(); }
                const std::string& hex = args[0].as_string()->str();
                if (hex.size() % 2 != 0) { ctx.throw_syntax_error("fromHex: odd-length string"); return Value(); }
                auto ta = TypedArrayFactory::create_uint8_array(hex.size() / 2);
                for (size_t i = 0; i < hex.size(); i += 2) {
                    auto h = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = h(hex[i]), lo = h(hex[i+1]);
                    if (hi < 0 || lo < 0) { ctx.throw_syntax_error("fromHex: invalid hex character"); return Value(); }
                    ta->set_element(i/2, Value((double)((hi << 4) | lo)));
                }
                return Value(ta.release());
            }, 1);
        uint8array_constructor->set_property("fromHex", Value(fromHex_fn.release()), PropertyAttributes::BuiltinFunction);

        // fromBase64(str, {alphabet, lastChunkHandling}) → Uint8Array
        auto fromBase64_fn = ObjectFactory::create_native_function("fromBase64",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.empty() || !args[0].is_string()) { ctx.throw_type_error("fromBase64 requires a string"); return Value(); }
                std::string str = args[0].as_string()->str();
                bool url_safe = false;
                if (args.size() > 1 && args[1].is_object()) {
                    Value alph = args[1].as_object()->get_property("alphabet");
                    if (!alph.is_undefined() && alph.to_string() == "base64url") url_safe = true;
                }
                // decode base64
                auto decode_char = [url_safe](char c) -> int {
                    if (c >= 'A' && c <= 'Z') return c - 'A';
                    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                    if (c >= '0' && c <= '9') return c - '0' + 52;
                    if (url_safe) { if (c == '-') return 62; if (c == '_') return 63; }
                    else          { if (c == '+') return 62; if (c == '/') return 63; }
                    return -1;
                };
                // strip padding/whitespace, validate
                std::string clean;
                for (char c : str) { if (c == '=') break; if (c != ' ' && c != '\t' && c != '\n' && c != '\r') clean += c; }
                std::vector<uint8_t> bytes;
                size_t i = 0;
                while (i < clean.size()) {
                    int b0 = i < clean.size() ? decode_char(clean[i++]) : 0;
                    int b1 = i < clean.size() ? decode_char(clean[i++]) : 0;
                    int b2 = i < clean.size() ? decode_char(clean[i++]) : -2; // -2 = absent
                    int b3 = i < clean.size() ? decode_char(clean[i++]) : -2;
                    if (b0 < 0 || b1 < 0) { ctx.throw_syntax_error("fromBase64: invalid character"); return Value(); }
                    if (b2 < -1 || b3 < -1) { ctx.throw_syntax_error("fromBase64: invalid character"); return Value(); }
                    bytes.push_back((b0 << 2) | (b1 >> 4));
                    if (b2 >= 0) bytes.push_back(((b1 & 0xF) << 4) | (b2 >> 2));
                    if (b3 >= 0) bytes.push_back(((b2 & 0x3) << 6) | b3);
                }
                auto ta = TypedArrayFactory::create_uint8_array(bytes.size());
                for (size_t j = 0; j < bytes.size(); j++) ta->set_element(j, Value((double)bytes[j]));
                return Value(ta.release());
            }, 1);
        uint8array_constructor->set_property("fromBase64", Value(fromBase64_fn.release()), PropertyAttributes::BuiltinFunction);
    }
    ctx.register_built_in_object("Uint8Array", uint8array_constructor.release());

    auto uint8clampedarray_constructor = ObjectFactory::create_native_constructor("Uint8ClampedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::UINT8_CLAMPED, 1);
        }, 3);
    ctx.register_built_in_object("Uint8ClampedArray", uint8clampedarray_constructor.release());

    auto float32array_constructor = ObjectFactory::create_native_constructor("Float32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::FLOAT32, 4);
        }, 3);
    ctx.register_built_in_object("Float32Array", float32array_constructor.release());

    auto typedarray_constructor = ObjectFactory::create_native_function("TypedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            ctx.throw_type_error("Abstract class TypedArray not intended to be instantiated directly");
            return Value();
        }, 0);
    // %TypedArray% has [[Construct]] per spec (IsConstructor is true); it just always throws when actually invoked. TypedArray.from relies on IsConstructor(this) being true for TypedArray.from(...).
    typedarray_constructor->set_is_constructor(true);

    PropertyDescriptor typedarray_name_desc(Value(std::string("TypedArray")),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("name", typedarray_name_desc);

    PropertyDescriptor typedarray_length_desc(Value(0.0),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("length", typedarray_length_desc);

    {
        auto ta_abstract_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                return Value(ctx.get_this_binding());
            }, 0);
        PropertyDescriptor ta_abstract_species_desc;
        ta_abstract_species_desc.set_getter(ta_abstract_species_getter.release());
        ta_abstract_species_desc.set_enumerable(false);
        ta_abstract_species_desc.set_configurable(true);
        typedarray_constructor->set_property_descriptor("Symbol.species", ta_abstract_species_desc);
    }

    auto typedarray_prototype = ObjectFactory::create_object();

    PropertyDescriptor typedarray_constructor_desc(Value(typedarray_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    typedarray_prototype->set_property_descriptor("constructor", typedarray_constructor_desc);

    {
        Symbol* ta_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (ta_tag_sym) {
            auto ta_tag_getter = ObjectFactory::create_native_function("get [Symbol.toStringTag]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self || !self->is_typed_array()) return Value();
                    TypedArrayBase* ta = static_cast<TypedArrayBase*>(self);
                    return Value(std::string(TypedArrayBase::array_type_to_string(ta->get_array_type())));
                });
            PropertyDescriptor ta_tag_desc;
            ta_tag_desc.set_getter(ta_tag_getter.release());
            ta_tag_desc.set_enumerable(false);
            ta_tag_desc.set_configurable(true);
            typedarray_prototype->set_property_descriptor(ta_tag_sym->to_property_key(), ta_tag_desc);
        }
    }


    auto buffer_getter = ObjectFactory::create_native_function("get buffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.buffer called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            ArrayBuffer* buf = ta->buffer();
            // Buffers allocated internally (length-arg/typed-array-arg construction) never go through Function::construct()'s prototype fixup, so wire the plain intrinsic prototype lazily here.
            if (buf && !buf->get_prototype_raw()) {
                Object* ab_ctor = ctx.get_built_in_object("ArrayBuffer");
                Value proto = ab_ctor ? ab_ctor->get_property("prototype") : Value();
                if (proto.is_object()) buf->set_prototype(proto.as_object());
            }
            return Value(buf);
        }, 0);
    PropertyDescriptor buffer_desc;
    buffer_desc.set_getter(buffer_getter.release());
    buffer_desc.set_enumerable(false);
    buffer_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("buffer", buffer_desc);

    auto byteLength_getter = ObjectFactory::create_native_function("get byteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.byteLength called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(static_cast<double>(ta->byte_length()));
        }, 0);
    PropertyDescriptor byteLength_desc;
    byteLength_desc.set_getter(byteLength_getter.release());
    byteLength_desc.set_enumerable(false);
    byteLength_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("byteLength", byteLength_desc);

    auto byteOffset_getter = ObjectFactory::create_native_function("get byteOffset",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.byteOffset called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (ta->is_out_of_bounds()) return Value(0.0);
            return Value(static_cast<double>(ta->byte_offset()));
        }, 0);
    PropertyDescriptor byteOffset_desc;
    byteOffset_desc.set_getter(byteOffset_getter.release());
    byteOffset_desc.set_enumerable(false);
    byteOffset_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("byteOffset", byteOffset_desc);

    auto length_getter = ObjectFactory::create_native_function("get length",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.length called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(static_cast<double>(ta->length()));
        }, 0);
    PropertyDescriptor length_desc;
    length_desc.set_getter(length_getter.release());
    length_desc.set_enumerable(false);
    length_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("length", length_desc);

    Object* typedarray_proto_ptr = typedarray_prototype.get();


    auto typedarray_at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            // len is captured before coercion; the coercion can resize the buffer.
            double len = static_cast<double>(ta->length());
            double rel = args.empty() ? 0.0 : to_integer_or_infinity(to_number_throwing(ctx, args[0]));
            if (ctx.has_exception()) return Value();
            double index = rel >= 0 ? rel : len + rel;

            if (index < 0 || index >= len) {
                return Value();
            }
            // Get(O, k): an index no longer valid after a resize reads as undefined.
            if (!ta->is_valid_integer_index(index)) {
                return Value();
            }
            return ta->get_element(static_cast<size_t>(index));
        }, 1);
    PropertyDescriptor typedarray_at_desc(Value(typedarray_at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("at", typedarray_at_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("forEach requires a callback function"); return Value(); }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            size_t len = ta->length();
            for (size_t i = 0; i < len; i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(ctx.get_this_binding()) };
                callback->call(ctx, cb_args, thisArg);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    PropertyDescriptor forEach_desc(Value(forEach_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("forEach", forEach_desc);

    auto map_fn = ObjectFactory::create_native_function("map",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.map called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("map requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            // Unlike filter, map's TypedArraySpeciesCreate happens *before* the loop.
            TypedArrayBase* result = typed_array_species_create(ctx, ta, length);
            if (!result) return Value();

            for (size_t i = 0; i < length; i++) {
                std::vector<Value> callback_args = {
                    ta->get_element(i),
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                Value mapped = callback->call(ctx, callback_args, thisArg);
                if (ctx.has_exception()) return Value();
                result->set_element(i, mapped);
                if (ctx.has_exception()) return Value();
            }
            return Value(result);
        }, 1);
    PropertyDescriptor map_desc(Value(map_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("map", map_desc);

    auto filter_fn = ObjectFactory::create_native_function("filter",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.filter called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("filter requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            std::vector<Value> filtered;

            for (size_t i = 0; i < length; i++) {
                Value element = ta->get_element(i);
                std::vector<Value> callback_args = {
                    element,
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (result.to_boolean()) {
                    filtered.push_back(element);
                }
            }

            // TypedArraySpeciesCreate happens *after* the full filter loop (all callbackfn calls
            // happen before any species-constructor lookup/construction), per spec ordering.
            TypedArrayBase* result = typed_array_species_create(ctx, ta, filtered.size());
            if (!result) return Value();
            for (size_t i = 0; i < filtered.size(); i++) {
                result->set_element(i, filtered[i]);
                if (ctx.has_exception()) return Value();
            }
            return Value(result);
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("filter", filter_desc);

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            size_t len = ta->length();
            for (size_t i = 0; i < len; i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(ctx.get_this_binding()) };
                Value r = cb->call(ctx, cb_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (!r.to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("every", PropertyDescriptor(Value(every_fn.release()), PropertyAttributes::BuiltinFunction));

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            size_t len = ta->length();
            for (size_t i = 0; i < len; i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(ctx.get_this_binding()) };
                Value r = cb->call(ctx, cb_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (r.to_boolean()) return Value(true);
            }
            return Value(false);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("some", PropertyDescriptor(Value(some_fn.release()), PropertyAttributes::BuiltinFunction));

    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            size_t len = ta->length();
            for (size_t i = 0; i < len; i++) {
                Value el = ta->get_element(i);
                std::vector<Value> cb_args = { el, Value(static_cast<double>(i)), Value(ctx.get_this_binding()) };
                Value r = cb->call(ctx, cb_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (r.to_boolean()) return el;
            }
            return Value();
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("find", PropertyDescriptor(Value(find_fn.release()), PropertyAttributes::BuiltinFunction));

    auto findIndex_fn = ObjectFactory::create_native_function("findIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            size_t len = ta->length();
            for (size_t i = 0; i < len; i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(ctx.get_this_binding()) };
                Value r = cb->call(ctx, cb_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (r.to_boolean()) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("findIndex", PropertyDescriptor(Value(findIndex_fn.release()), PropertyAttributes::BuiltinFunction));

    auto findLast_fn = ObjectFactory::create_native_function("findLast",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (int64_t i = static_cast<int64_t>(ta->length()) - 1; i >= 0; i--) {
                Value el = ta->get_element(static_cast<size_t>(i));
                std::vector<Value> cb_args = { el, Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return el;
            }
            return Value();
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("findLast", PropertyDescriptor(Value(findLast_fn.release()), PropertyAttributes::BuiltinFunction));

    auto findLastIndex_fn = ObjectFactory::create_native_function("findLastIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (int64_t i = static_cast<int64_t>(ta->length()) - 1; i >= 0; i--) {
                std::vector<Value> cb_args = { ta->get_element(static_cast<size_t>(i)), Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("findLastIndex", PropertyDescriptor(Value(findLastIndex_fn.release()), PropertyAttributes::BuiltinFunction));

    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            double len = static_cast<double>(ta->length());
            if (args.empty() || len == 0) return Value(-1.0);
            Value search = args[0];
            double rel = args.size() > 1 ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : 0.0;
            if (ctx.has_exception()) return Value();
            if (rel == std::numeric_limits<double>::infinity()) return Value(-1.0);
            // fromIndex coercion can detach or make a fixed-length view OOB; nothing to find.
            if (ta->is_out_of_bounds()) return Value(-1.0);
            if (rel == 0.0) rel = 0.0;
            double from = rel < 0 ? std::max(len + rel, 0.0) : std::min(rel, len);
            if (from == 0.0) from = 0.0; // normalize -0
            for (double i = from; i < len; i++) {
                if (ta->get_element(static_cast<size_t>(i)).strict_equals(search)) return Value(i);
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("indexOf", PropertyDescriptor(Value(indexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            double len = static_cast<double>(ta->length());
            if (args.empty() || len == 0) return Value(-1.0);
            Value search = args[0];
            double rel = args.size() > 1 ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : len - 1;
            if (ctx.has_exception()) return Value();
            if (rel == -std::numeric_limits<double>::infinity()) return Value(-1.0);
            // fromIndex coercion can detach or make a fixed-length view OOB; nothing to find.
            if (ta->is_out_of_bounds()) return Value(-1.0);
            if (rel == 0.0) rel = 0.0;
            double from = rel < 0 ? len + rel : std::min(rel, len - 1);
            if (from == 0.0) from = 0.0; // normalize -0
            for (double i = from; i >= 0; i--) {
                if (ta->get_element(static_cast<size_t>(i)).strict_equals(search)) return Value(i);
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("lastIndexOf", PropertyDescriptor(Value(lastIndexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            size_t len = ta->length(), k = 0; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else {
                if (len == 0) { ctx.throw_type_error("Reduce of empty array with no initial value"); return Value(); }
                acc = ta->get_element(static_cast<size_t>(0)); k = 1;
            }
            for (; k < len; k++) {
                std::vector<Value> cb_args = { acc, ta->get_element(k), Value(static_cast<double>(k)), Value(ctx.get_this_binding()) };
                acc = cb->call(ctx, cb_args, Value());
                if (ctx.has_exception()) return Value();
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduce", PropertyDescriptor(Value(reduce_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            Function* cb = args[0].as_function();
            int64_t len = static_cast<int64_t>(ta->length()), k = len - 1; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else {
                if (len == 0) { ctx.throw_type_error("Reduce of empty array with no initial value"); return Value(); }
                acc = ta->get_element(static_cast<size_t>(k)); k--;
            }
            for (; k >= 0; k--) {
                std::vector<Value> cb_args = { acc, ta->get_element(static_cast<size_t>(k)), Value(static_cast<double>(k)), Value(ctx.get_this_binding()) };
                acc = cb->call(ctx, cb_args, Value());
                if (ctx.has_exception()) return Value();
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduceRight", PropertyDescriptor(Value(reduceRight_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_join_fn = ObjectFactory::create_native_function("join",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            // Spec order: length is captured before ToString(separator), which can resize the buffer.
            size_t len = ta->length();
            Value sep_arg = args.empty() ? Value() : args[0];
            if (sep_arg.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a string"); return Value(); }
            std::string sep = sep_arg.is_undefined() ? std::string(",") : to_string_via_toprimitive(ctx, sep_arg);
            if (ctx.has_exception()) return Value();
            std::string result;
            for (size_t i = 0; i < len; i++) {
                if (i > 0) result += sep;
                Value elem = ta->get_element(i);
                if (!elem.is_undefined() && !elem.is_null()) {
                    result += elem.to_string();
                    if (ctx.has_exception()) return Value();
                }
            }
            return Value(result);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("join", PropertyDescriptor(Value(ta_join_fn.release()), PropertyAttributes::BuiltinFunction));

    // %TypedArray%.prototype.toString is the exact same function object as Array.prototype.toString.
    {
        Object* array_ctor = ctx.get_built_in_object("Array");
        Value array_proto = array_ctor ? array_ctor->get_property("prototype") : Value();
        Value array_to_string = array_proto.is_object() ? array_proto.as_object()->get_property("toString") : Value();
        if (array_to_string.is_function()) {
            typedarray_proto_ptr->set_property_descriptor("toString", PropertyDescriptor(array_to_string, PropertyAttributes::BuiltinFunction));
        }
    }

    auto ta_tolocalestring_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            size_t len = ta->length();
            if (len == 0) return Value(std::string(""));
            std::string result;
            for (size_t i = 0; i < len; i++) {
                if (i > 0) result += ",";
                Value elem = ta->get_element(i);
                // undefined/null elements (e.g. past a shrunk resizable buffer) contribute the empty string, not "undefined".
                if (elem.is_undefined() || elem.is_null()) continue;
                // Look up toLocaleString via the element's prototype chain (numbers need
                // Number.prototype.toLocaleString, not the raw numeric string).
                Value locale_fn;
                if (elem.is_object() || elem.is_function()) {
                    Object* obj = elem.is_function() ? static_cast<Object*>(elem.as_function()) : elem.as_object();
                    locale_fn = obj->get_property("toLocaleString");
                } else if (elem.is_number() || elem.is_bigint()) {
                    const char* ctor_name = elem.is_bigint() ? "BigInt" : "Number";
                    Value ctor = ctx.get_binding(ctor_name);
                    if (ctor.is_function()) {
                        Value np = static_cast<Object*>(ctor.as_function())->get_property("prototype");
                        if (!ctx.has_exception()) {
                            Object* np_obj = np.is_object() ? np.as_object() : (np.is_function() ? static_cast<Object*>(np.as_function()) : nullptr);
                            if (np_obj) {
                                Value tls = np_obj->get_property("toLocaleString");
                                if (!ctx.has_exception() && tls.is_function()) locale_fn = tls;
                            }
                        }
                        if (ctx.has_exception()) ctx.clear_exception();
                    }
                }
                if (ctx.has_exception()) return Value();
                Value str_val;
                if (locale_fn.is_function()) {
                    str_val = locale_fn.as_function()->call(ctx, {}, elem);
                    if (ctx.has_exception()) return Value();
                } else {
                    str_val = elem;
                }
                result += to_string_via_toprimitive(ctx, str_val);
                if (ctx.has_exception()) return Value();
            }
            return Value(result);
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("toLocaleString", PropertyDescriptor(Value(ta_tolocalestring_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            Object* this_obj = ctx.get_this_binding();
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_function()) {
                ctx.throw_type_error("TypedArray.prototype.sort: comparefn must be callable or undefined");
                return Value();
            }
            size_t len = ta->length();
            if (len <= 1) return Value(this_obj);
            Function* cmp = (!args.empty() && args[0].is_function()) ? args[0].as_function() : nullptr;
            std::vector<Value> els; els.reserve(len);
            for (size_t i = 0; i < len; i++) els.push_back(ta->get_element(i));
            // Plain `<` on doubles is UB for std::sort with NaN present (NaN<x and x<NaN both false); NaN must sort last. Spec also requires stability and comparefn-throw propagation.
            std::stable_sort(els.begin(), els.end(), [&](const Value& a, const Value& b) {
                if (ctx.has_exception()) return false;
                if (cmp) {
                    std::vector<Value> ca = {a, b};
                    double r = cmp->call(ctx, ca, Value()).to_number();
                    if (ctx.has_exception()) return false;
                    return r < 0;
                }
                if (a.is_bigint() && b.is_bigint()) return *a.as_bigint() < *b.as_bigint();
                double ad = a.to_number(), bd = b.to_number();
                bool a_nan = std::isnan(ad), b_nan = std::isnan(bd);
                if (a_nan) return false;
                if (b_nan) return true;
                if (ad < bd) return true;
                if (ad > bd) return false;
                // ad == bd numerically (including -0 == +0): -0 sorts before +0 per spec.
                return std::signbit(ad) && !std::signbit(bd);
            });
            if (ctx.has_exception()) return Value();
            for (size_t i = 0; i < len; i++) ta->set_element(i, els[i]);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("sort", PropertyDescriptor(Value(ta_sort_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            Object* this_obj = ctx.get_this_binding();
            size_t len = ta->length();
            for (size_t i = 0; i < len / 2; i++) { Value t = ta->get_element(i); ta->set_element(i, ta->get_element(len - 1 - i)); ta->set_element(len - 1 - i, t); }
            return Value(this_obj);
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("reverse", PropertyDescriptor(Value(ta_reverse_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            int64_t len = ta->length(), start = 0, end = len;
            if (!args.empty()) {
                double sd = to_number_throwing(ctx, args[0]);
                if (ctx.has_exception()) return Value();
                // ToIntegerOrInfinity: clamp before casting -- (int64_t)Infinity is UB.
                int64_t s = std::isnan(sd) ? 0 : sd >= 9.2e18 ? len : sd <= -9.2e18 ? -len : static_cast<int64_t>(sd);
                start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s);
            }
            if (args.size() > 1 && !args[1].is_undefined()) {
                double ed = to_number_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                int64_t e = std::isnan(ed) ? 0 : ed >= 9.2e18 ? len : ed <= -9.2e18 ? -len : static_cast<int64_t>(ed);
                end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e);
            }
            size_t sl = end > start ? end - start : 0;
            TypedArrayBase* r = typed_array_species_create(ctx, ta, sl);
            if (!r) return Value();
            if (sl > 0) {
                // start/end conversion above can resize/detach the buffer (e.g. via valueOf), so
                // re-validate and re-clamp against the *current* length before actually copying.
                if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                int64_t current_len = static_cast<int64_t>(ta->length());
                if (end > current_len) end = current_len;
                sl = end > start ? static_cast<size_t>(end - start) : 0;
                for (size_t i = 0; i < sl && i < r->length(); i++) r->set_element(i, ta->get_element(start + i));
            }
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("slice", PropertyDescriptor(Value(ta_slice_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            double len = static_cast<double>(ta->length());

            Value raw_val = args.empty() ? Value() : args[0];
            bool is_big = ta->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                          ta->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;
            // Value is converted ONCE before index computation, not per-element.
            Value fill_val;
            if (is_big) {
                fill_val = to_bigint_for_typed_array(ctx, raw_val);
            } else {
                fill_val = Value(to_number_throwing(ctx, raw_val));
            }
            if (ctx.has_exception()) return Value();

            double rel_start = args.size() > 1 ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : 0.0;
            if (ctx.has_exception()) return Value();
            double rel_end   = (args.size() > 2 && !args[2].is_undefined())
                               ? to_integer_or_infinity(to_number_throwing(ctx, args[2])) : len;
            if (ctx.has_exception()) return Value();

            // Re-validate after coercions (they can detach/resize via valueOf).
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            len = static_cast<double>(ta->length());
            double actual_start = clamp_relative_index(rel_start, len);
            double actual_end   = clamp_relative_index(rel_end,   len);

            for (double k = actual_start; k < actual_end; k++) {
                ta->set_element(static_cast<size_t>(k), fill_val);
                if (ctx.has_exception()) return Value();
            }
            return Value(ctx.get_this_binding());
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("fill", PropertyDescriptor(Value(ta_fill_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            double orig_len = static_cast<double>(ta->length());

            double rel_tgt   = args.empty() ? 0.0 : to_integer_or_infinity(to_number_throwing(ctx, args[0]));
            if (ctx.has_exception()) return Value();
            double rel_start = args.size() > 1 ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : 0.0;
            if (ctx.has_exception()) return Value();
            double rel_end   = (args.size() > 2 && !args[2].is_undefined())
                               ? to_integer_or_infinity(to_number_throwing(ctx, args[2])) : orig_len;
            if (ctx.has_exception()) return Value();

            // Per spec, to/from/final/count are computed with orig_len (before coercion side
            // effects); then re-validated against the current (possibly grown/shrunk) length.
            double to    = clamp_relative_index(rel_tgt,   orig_len);
            double from  = clamp_relative_index(rel_start, orig_len);
            double final = clamp_relative_index(rel_end,   orig_len);
            double count = std::min(final - from, orig_len - to);

            if (ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            double cur_len = static_cast<double>(ta->length());
            to    = std::min(to,    cur_len);
            from  = std::min(from,  cur_len);
            count = std::min(count, cur_len - to);
            if (count <= 0) return Value(ctx.get_this_binding());

            // Snapshot source range first (handles overlap); skip write if source index was OOB
            // (buffer may have shrunk during coercions, leaving the tail of count OOB).
            size_t cnt = static_cast<size_t>(count);
            size_t cur = static_cast<size_t>(cur_len);
            std::vector<Value> tmp(cnt);
            std::vector<bool> valid(cnt, false);
            for (size_t i = 0; i < cnt; i++) {
                size_t f = static_cast<size_t>(from) + i;
                if (f < cur) { tmp[i] = ta->get_element(f); valid[i] = true; }
            }
            for (size_t i = 0; i < cnt; i++) {
                if (valid[i]) ta->set_element(static_cast<size_t>(to) + i, tmp[i]);
            }
            return Value(ctx.get_this_binding());
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("copyWithin", PropertyDescriptor(Value(ta_copyWithin_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_with_fn = ObjectFactory::create_native_function("with",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            int64_t len = static_cast<int64_t>(ta->length());
            double rel = args.empty() ? 0.0 : to_number_throwing(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            double rel_int = to_integer_or_infinity(rel);
            double actual_index_d = rel_int >= 0 ? rel_int : static_cast<double>(len) + rel_int;
            bool is_bigint_kind = ta->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                                   ta->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;
            Value numeric_value = args.size() > 1 ? args[1] : Value();
            numeric_value = is_bigint_kind ? to_bigint_for_typed_array(ctx, numeric_value) : Value(numeric_value.to_number());
            if (ctx.has_exception()) return Value();
            // IsValidIntegerIndex runs after all coercions, against the current (possibly
            // resized) state -- not against the length captured at entry.
            if (!ta->is_valid_integer_index(actual_index_d)) {
                ctx.throw_range_error("Invalid typed array index");
                return Value();
            }
            int64_t actual_index = static_cast<int64_t>(actual_index_d);
            TypedArrayBase* r = create_same_type_typed_array(ctx, ta, static_cast<size_t>(len));
            if (!r) return Value();
            for (int64_t i = 0; i < len; i++) {
                Value v = i == actual_index ? numeric_value
                        : (static_cast<size_t>(i) < ta->length() ? ta->get_element(static_cast<size_t>(i)) : Value());
                r->set_element(static_cast<size_t>(i), v);
            }
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("with", PropertyDescriptor(Value(ta_with_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_toSorted_fn = ObjectFactory::create_native_function("toSorted",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!args.empty() && !args[0].is_undefined() && !args[0].is_function()) {
                ctx.throw_type_error("comparefn must be a function or undefined");
                return Value();
            }
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            size_t len = ta->length();
            Function* cmp = (!args.empty() && args[0].is_function()) ? args[0].as_function() : nullptr;
            std::vector<Value> els; els.reserve(len);
            for (size_t i = 0; i < len; i++) els.push_back(ta->get_element(i));
            std::sort(els.begin(), els.end(), [&](const Value& a, const Value& b) {
                if (ctx.has_exception()) return false;
                if (cmp) { std::vector<Value> ca = {a, b}; return cmp->call(ctx, ca, Value()).to_number() < 0; }
                if (a.is_bigint() && b.is_bigint()) return *a.as_bigint() < *b.as_bigint();
                return a.to_number() < b.to_number();
            });
            if (ctx.has_exception()) return Value();
            TypedArrayBase* r = create_same_type_typed_array(ctx, ta, len);
            if (!r) return Value();
            for (size_t i = 0; i < len; i++) r->set_element(i, els[i]);
            return Value(r);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("toSorted", PropertyDescriptor(Value(ta_toSorted_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_toReversed_fn = ObjectFactory::create_native_function("toReversed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            size_t len = ta->length();
            TypedArrayBase* r = create_same_type_typed_array(ctx, ta, len);
            if (!r) return Value();
            for (size_t i = 0; i < len; i++) r->set_element(i, ta->get_element(len - 1 - i));
            return Value(r);
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("toReversed", PropertyDescriptor(Value(ta_toReversed_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* _ta = static_cast<TypedArrayBase*>(this_obj); if (_ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            auto iter = ObjectFactory::create_object();
            if (Iterator::s_array_iterator_prototype_) iter->set_prototype(Iterator::s_array_iterator_prototype_);
            iter->set_property("__idx", Value(0.0)); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                // Re-derive length fresh each call (not cached at iterator-creation time), so a length-tracking view sees a mid-iteration resize of its buffer.
                (void)a; Object* it = ctx.get_this_binding();
                Value arr_val = it->get_property("__arr");
                auto res = ObjectFactory::create_object();
                // An exhausted iterator stays done regardless of later buffer resizes.
                if (!arr_val.is_object()) { res->set_property("done", Value(true)); res->set_property("value", Value()); return Value(res.release()); }
                TypedArrayBase* live = static_cast<TypedArrayBase*>(arr_val.as_object());
                if (live->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = live->length();
                if (idx >= len) { it->set_property("__arr", Value()); res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { auto pair = ObjectFactory::create_array(2); pair->set_element(0, Value((double)idx)); pair->set_element(1, live->get_element(idx));
                    res->set_property("done", Value(false)); res->set_property("value", Value(pair.release())); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release()));
            return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("entries", PropertyDescriptor(Value(ta_entries_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* _ta = static_cast<TypedArrayBase*>(this_obj); if (_ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            auto iter = ObjectFactory::create_object();
            if (Iterator::s_array_iterator_prototype_) iter->set_prototype(Iterator::s_array_iterator_prototype_);
            iter->set_property("__idx", Value(0.0)); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                // Re-derive length fresh each call (not cached at iterator-creation time), so a length-tracking view sees a mid-iteration resize of its buffer.
                (void)a; Object* it = ctx.get_this_binding();
                Value arr_val = it->get_property("__arr");
                auto res = ObjectFactory::create_object();
                if (!arr_val.is_object()) { res->set_property("done", Value(true)); res->set_property("value", Value()); return Value(res.release()); }
                TypedArrayBase* live = static_cast<TypedArrayBase*>(arr_val.as_object());
                if (live->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = live->length();
                if (idx >= len) { it->set_property("__arr", Value()); res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", Value((double)idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release()));
            return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("keys", PropertyDescriptor(Value(ta_keys_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* _ta = static_cast<TypedArrayBase*>(this_obj); if (_ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            auto iter = ObjectFactory::create_object();
            if (Iterator::s_array_iterator_prototype_) iter->set_prototype(Iterator::s_array_iterator_prototype_);
            iter->set_property("__idx", Value(0.0)); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                // Re-derive length fresh each call (not cached at iterator-creation time), so a length-tracking view sees a mid-iteration resize of its buffer.
                (void)a; Object* it = ctx.get_this_binding();
                Value arr_val = it->get_property("__arr");
                auto res = ObjectFactory::create_object();
                if (!arr_val.is_object()) { res->set_property("done", Value(true)); res->set_property("value", Value()); return Value(res.release()); }
                TypedArrayBase* live = static_cast<TypedArrayBase*>(arr_val.as_object());
                if (live->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = live->length();
                if (idx >= len) { it->set_property("__arr", Value()); res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", live->get_element(idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release()));
            return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("values", PropertyDescriptor(Value(ta_values_fn.release()), PropertyAttributes::BuiltinFunction));

    // ES6: %TypedArray%.prototype[Symbol.iterator] is the exact same function object as .values.
    {
        Value values_fn = typedarray_proto_ptr->get_property("values");
        PropertyDescriptor sym_iter_desc(values_fn, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
        typedarray_proto_ptr->set_property_descriptor("Symbol.iterator", sym_iter_desc);
    }

    auto ta_subarray_fn = ObjectFactory::create_native_function("subarray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            double len = static_cast<double>(ta->length());

            double rel_begin = args.empty() ? 0.0 : to_integer_or_infinity(to_number_throwing(ctx, args[0]));
            if (ctx.has_exception()) return Value();
            double begin_index = clamp_relative_index(rel_begin, len);
            double byte_offset = static_cast<double>(ta->byte_offset()) + begin_index * ta->bytes_per_element();

            bool end_given = args.size() > 1 && !args[1].is_undefined();
            std::vector<Value> ctor_args;
            if (!end_given && ta->is_length_tracking()) {
                ctor_args = { Value(ta->buffer()), Value(byte_offset) };
            } else {
                double rel_end = end_given ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : len;
                if (ctx.has_exception()) return Value();
                double end_index = clamp_relative_index(rel_end, len);
                double new_length = std::max(end_index - begin_index, 0.0);
                ctor_args = { Value(ta->buffer()), Value(byte_offset), Value(new_length) };
            }

            TypedArrayBase* r = typed_array_species_create(ctx, ta, ctor_args);
            if (!r) return Value();
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("subarray", PropertyDescriptor(Value(ta_subarray_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_set_fn = ObjectFactory::create_native_function("set",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* target = static_cast<TypedArrayBase*>(this_obj);

            Value source_arg = args.empty() ? Value() : args[0];
            bool source_is_typed_array = source_arg.is_object() && source_arg.as_object()->is_typed_array();

            // ToIntegerOrInfinity(offset) happens before ToObject(source) for the array-like path.
            Value offset_arg = args.size() > 1 ? args[1] : Value();
            double offset_double = to_integer_or_infinity(to_number_throwing(ctx, offset_arg));
            if (ctx.has_exception()) return Value();
            if (offset_double < 0) {
                ctx.throw_range_error("Invalid offset");
                return Value();
            }

            bool target_is_big = target->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                                  target->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;

            if (source_is_typed_array) {
                TypedArrayBase* src_ta = static_cast<TypedArrayBase*>(source_arg.as_object());
                if (target->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                size_t target_length = target->length();
                if (src_ta->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
                size_t src_len = src_ta->length();
                if (offset_double + static_cast<double>(src_len) > static_cast<double>(target_length)) {
                    ctx.throw_range_error("Source is too large");
                    return Value();
                }
                bool source_is_big = src_ta->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                                      src_ta->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;
                if (target_is_big != source_is_big) {
                    ctx.throw_type_error("Cannot mix BigInt and non-BigInt typed array content");
                    return Value();
                }
                size_t offset = static_cast<size_t>(offset_double);
                // Same buffer: snapshot source first to avoid reads/writes corrupting overlapping bytes.
                if (src_ta->buffer() == target->buffer()) {
                    std::vector<Value> snapshot(src_len);
                    for (size_t i = 0; i < src_len; i++) snapshot[i] = src_ta->get_element(i);
                    for (size_t i = 0; i < src_len; i++) target->set_element(offset + i, snapshot[i]);
                } else {
                    for (size_t i = 0; i < src_len; i++) target->set_element(offset + i, src_ta->get_element(i));
                }
                return Value();
            }

            if (target->is_out_of_bounds()) { ctx.throw_type_error("TypedArray is out of bounds"); return Value(); }
            size_t target_length = target->length();

            Object* source = to_object_or_throw(ctx, source_arg);
            if (ctx.has_exception()) return Value();

            Value len_val = source->get_property("length");
            if (ctx.has_exception()) return Value();
            if (len_val.is_symbol() || len_val.is_bigint()) {
                ctx.throw_type_error("Cannot convert length to a number");
                return Value();
            }
            double len_double = len_val.to_number();
            if (ctx.has_exception()) return Value();
            size_t src_len = (std::isnan(len_double) || len_double < 0) ? 0 :
                static_cast<size_t>(std::min(len_double, 9007199254740991.0));

            if (offset_double + static_cast<double>(src_len) > static_cast<double>(target_length)) {
                ctx.throw_range_error("Source is too large");
                return Value();
            }
            size_t offset = static_cast<size_t>(offset_double);
            // Bounds re-checked live per set_element (source getter can resize/detach mid-loop); OOB silently skipped.
            for (size_t i = 0; i < src_len; i++) {
                Value v = source->get_property(std::to_string(i));
                if (ctx.has_exception()) return Value();
                if (target_is_big) {
                    v = to_bigint_for_typed_array(ctx, v);
                    if (ctx.has_exception()) return Value();
                } else if (v.is_symbol()) {
                    ctx.throw_type_error("Cannot convert a Symbol value to a number");
                    return Value();
                } else if (v.is_object()) {
                    // set_element's internal to_number() doesn't invoke a user valueOf/toString; do ToNumber here so an abrupt completion propagates.
                    v = Value(to_number_throwing(ctx, v));
                    if (ctx.has_exception()) return Value();
                }
                target->set_element(offset + i, v);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("set", PropertyDescriptor(Value(ta_set_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            TypedArrayBase* ta = validate_typed_array(ctx, ctx.get_this_binding());
            if (!ta) return Value();
            double len = static_cast<double>(ta->length());
            Value search = args.empty() ? Value() : args[0];
            // Spec checks len === 0 before ToInteger(fromIndex), so a throwing fromIndex must not run on an empty typed array.
            if (len == 0) return Value(false);
            double rel = args.size() > 1 ? to_integer_or_infinity(to_number_throwing(ctx, args[1])) : 0.0;
            if (ctx.has_exception()) return Value();
            if (rel == std::numeric_limits<double>::infinity()) return Value(false);
            double from = rel < 0 ? std::max(len + rel, 0.0) : std::min(rel, len);
            for (double i = from; i < len; i++) {
                Value element = ta->get_element(static_cast<size_t>(i));
                if (search.is_number() && element.is_number()) {
                    double sn = search.to_number(), en = element.to_number();
                    if (std::isnan(sn) && std::isnan(en)) return Value(true);
                    if (sn == en) return Value(true);
                } else if (element.strict_equals(search)) {
                    return Value(true);
                }
            }
            return Value(false);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("includes", PropertyDescriptor(Value(ta_includes_fn.release()), PropertyAttributes::BuiltinFunction));

    PropertyDescriptor typedarray_prototype_desc(Value(typedarray_prototype.release()), PropertyAttributes::None);
    typedarray_constructor->set_property_descriptor("prototype", typedarray_prototype_desc);


    auto typedarray_from = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            Function* ctor = this_obj ? dynamic_cast<Function*>(this_obj) : nullptr;
            if (!ctor || !ctor->is_constructor()) {
                ctx.throw_type_error("TypedArray.from must be called on a concrete TypedArray constructor");
                return Value();
            }
            Value mapfn = args.size() > 1 ? args[1] : Value();
            if (!mapfn.is_undefined() && !mapfn.is_function()) {
                ctx.throw_type_error("mapfn is not a function");
                return Value();
            }
            Value this_arg = args.size() > 2 ? args[2] : Value();
            Value source = args.empty() ? Value() : args[0];

            std::vector<Value> items;
            bool got_iterator = false;
            if (source.is_object()) {
                Object* src_obj = source.as_object();
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_fn = src_obj->get_property(iter_sym->to_property_key());
                    if (ctx.has_exception()) return Value();
                    if (iter_fn.is_function()) {
                        got_iterator = true;
                        Value iterator = iter_fn.as_function()->call(ctx, {}, source);
                        if (ctx.has_exception()) return Value();
                        Object* it = iterator.is_object() ? iterator.as_object() : nullptr;
                        while (it) {
                            Value next_fn = it->get_property("next");
                            if (!next_fn.is_function()) break;
                            Value res = next_fn.as_function()->call(ctx, {}, iterator);
                            if (ctx.has_exception()) return Value();
                            if (!res.is_object()) break;
                            Value done_val = res.as_object()->get_property("done");
                            if (ctx.has_exception()) return Value();
                            if (done_val.to_boolean()) break;
                            Value item_val = res.as_object()->get_property("value");
                            if (ctx.has_exception()) return Value();
                            items.push_back(item_val);
                        }
                    }
                }
            }
            if (!got_iterator) {
                if (source.is_undefined() || source.is_null()) {
                    ctx.throw_type_error("Cannot convert undefined or null to object");
                    return Value();
                }
                if (source.is_object()) {
                    Object* obj = source.as_object();
                    uint32_t len;
                    if (obj->is_typed_array()) {
                        len = static_cast<uint32_t>(static_cast<TypedArrayBase*>(obj)->length());
                    } else {
                        Value len_val = obj->get_property("length");
                        if (ctx.has_exception()) return Value();
                        double n = len_val.is_symbol() ? (ctx.throw_type_error("Cannot convert a Symbol value to a number"), 0.0) : len_val.to_number();
                        if (ctx.has_exception()) return Value();
                        if (std::isnan(n) || n <= 0) len = 0;
                        else len = static_cast<uint32_t>(std::min(n, (double)UINT32_MAX));
                    }
                    for (uint32_t i = 0; i < len; i++) {
                        items.push_back(obj->get_property(std::to_string(i)));
                        if (ctx.has_exception()) return Value();
                    }
                }
            }

            std::vector<Value> ctor_args = { Value(static_cast<double>(items.size())) };
            Value result = ctor->construct(ctx, ctor_args);
            if (ctx.has_exception()) return Value();
            if (!result.is_object() || !result.as_object()->is_typed_array()) {
                ctx.throw_type_error("TypedArray constructor did not return a TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(result.as_object());
            // TypedArrayCreate: the constructed target must not be shorter than the requested length.
            if (ta->length() < items.size()) {
                ctx.throw_type_error("Derived TypedArray constructor created an array which was too small");
                return Value();
            }
            bool target_is_big = ta->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                                  ta->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;
            Function* mfn = mapfn.is_function() ? mapfn.as_function() : nullptr;
            // mapfn runs per-element, interleaved with Set into the result. Per IntegerIndexedElementSet, a mapfn that detaches/resizes the result's buffer just makes the next Set's index invalid, silently skipped rather than a throw.
            for (size_t i = 0; i < items.size(); i++) {
                Value v = items[i];
                if (mfn) {
                    std::vector<Value> cb_args = { v, Value(static_cast<double>(i)) };
                    v = mfn->call(ctx, cb_args, this_arg);
                    if (ctx.has_exception()) return Value();
                }
                if (target_is_big) {
                    v = to_bigint_for_typed_array(ctx, v);
                    if (ctx.has_exception()) return Value();
                } else if (v.is_symbol()) {
                    // value.to_number() silently returns NaN for a Symbol; ToNumber must throw.
                    ctx.throw_type_error("Cannot convert a Symbol value to a number");
                    return Value();
                }
                if (!ta->is_out_of_bounds() && i < ta->length()) {
                    ta->set_element(i, v);
                    if (ctx.has_exception()) return Value();
                }
            }
            return result;
        }, 1);
    PropertyDescriptor from_desc(Value(typedarray_from.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("from", from_desc);

    auto typedarray_of = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            Function* ctor = this_obj ? dynamic_cast<Function*>(this_obj) : nullptr;
            if (!ctor) {
                ctx.throw_type_error("TypedArray.of must be called on a concrete TypedArray constructor");
                return Value();
            }
            std::vector<Value> ctor_args = { Value(static_cast<double>(args.size())) };
            Value result = ctor->construct(ctx, ctor_args);
            if (ctx.has_exception()) return Value();
            if (!result.is_object() || !result.as_object()->is_typed_array()) {
                ctx.throw_type_error("TypedArray constructor did not return a TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(result.as_object());
            // TypedArrayCreate: a custom ctor may return a longer-or-equal instance, never shorter.
            if (ta->is_out_of_bounds() || ta->length() < args.size()) {
                ctx.throw_type_error("Derived TypedArray constructor created an array which was too small");
                return Value();
            }
            bool target_is_big = ta->get_array_type() == TypedArrayBase::ArrayType::BIGINT64 ||
                                  ta->get_array_type() == TypedArrayBase::ArrayType::BIGUINT64;
            for (size_t i = 0; i < args.size(); i++) {
                Value v = args[i];
                if (target_is_big) {
                    v = to_bigint_for_typed_array(ctx, v);
                    if (ctx.has_exception()) return Value();
                } else if (v.is_symbol()) {
                    ctx.throw_type_error("Cannot convert a Symbol value to a number");
                    return Value();
                }
                ta->set_element(i, v);
                if (ctx.has_exception()) return Value();
            }
            return result;
        }, 0);
    PropertyDescriptor of_desc(Value(typedarray_of.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("of", of_desc);

    ctx.register_built_in_object("TypedArray", typedarray_constructor.release());

    auto int8array_constructor = ObjectFactory::create_native_constructor("Int8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::INT8, 1);
        }, 3);
    ctx.register_built_in_object("Int8Array", int8array_constructor.release());

    auto uint16array_constructor = ObjectFactory::create_native_constructor("Uint16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::UINT16, 2);
        }, 3);
    ctx.register_built_in_object("Uint16Array", uint16array_constructor.release());

    auto int16array_constructor = ObjectFactory::create_native_constructor("Int16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::INT16, 2);
        }, 3);
    ctx.register_built_in_object("Int16Array", int16array_constructor.release());

    auto uint32array_constructor = ObjectFactory::create_native_constructor("Uint32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::UINT32, 4);
        }, 3);
    ctx.register_built_in_object("Uint32Array", uint32array_constructor.release());

    auto int32array_constructor = ObjectFactory::create_native_constructor("Int32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::INT32, 4);
        }, 3);
    ctx.register_built_in_object("Int32Array", int32array_constructor.release());

    auto float64array_constructor = ObjectFactory::create_native_constructor("Float64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::FLOAT64, 8);
        }, 3);
    ctx.register_built_in_object("Float64Array", float64array_constructor.release());

    auto bigint64array_constructor = ObjectFactory::create_native_constructor("BigInt64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::BIGINT64, 8);
        }, 3);
    ctx.register_built_in_object("BigInt64Array", bigint64array_constructor.release());

    auto biguint64array_constructor = ObjectFactory::create_native_constructor("BigUint64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            return construct_typed_array_generic(ctx, args, TypedArrayBase::ArrayType::BIGUINT64, 8);
        }, 3);
    ctx.register_built_in_object("BigUint64Array", biguint64array_constructor.release());

    // Set up prototype chains: XArray.prototype.__proto__ = TypedArray.prototype
    // Also: XArray.__proto__ = TypedArray (constructor chain)
    Object* typedarray_ctor_ptr = ctx.get_built_in_object("TypedArray");
    struct TypedInfo { const char* name; int bytes; };
    TypedInfo typed_infos[] = {
        {"Int8Array", 1}, {"Uint8Array", 1}, {"Uint8ClampedArray", 1},
        {"Int16Array", 2}, {"Uint16Array", 2}, {"Int32Array", 4}, {"Uint32Array", 4},
        {"Float32Array", 4}, {"Float64Array", 8},
        {"BigInt64Array", 8}, {"BigUint64Array", 8}
    };
    Symbol* ta_species_sym = Symbol::get_well_known(Symbol::SPECIES);
    for (const auto& info : typed_infos) {
        Object* ctor = ctx.get_built_in_object(info.name);
        if (ctor) {
            if (typedarray_ctor_ptr) {
                ctor->set_prototype(typedarray_ctor_ptr);
            }
            if (ta_species_sym) {
                auto getter = ObjectFactory::create_native_function("get [Symbol.species]",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        return Value(ctx.get_this_binding());
                    }, 0);
                PropertyDescriptor species_desc;
                species_desc.set_getter(getter.release());
                species_desc.set_enumerable(false);
                species_desc.set_configurable(true);
                ctor->set_property_descriptor(ta_species_sym->to_property_key(), species_desc);
            }
            Value proto_val = ctor->get_property("prototype");
            if (proto_val.is_object_like() && proto_val.as_object()) {
                Object* proto = proto_val.as_object();
                proto->set_prototype(typedarray_proto_ptr);
                PropertyDescriptor bpe_desc(Value(static_cast<double>(info.bytes)), PropertyAttributes::None);
                bpe_desc.set_enumerable(false);
                bpe_desc.set_writable(false);
                bpe_desc.set_configurable(false);
                proto->set_property_descriptor("BYTES_PER_ELEMENT", bpe_desc);
                PropertyDescriptor ctor_desc(Value(static_cast<Function*>(ctor)), PropertyAttributes::BuiltinFunction);
                ctor_desc.set_enumerable(false);
                proto->set_property_descriptor("constructor", ctor_desc);
            }
            // BYTES_PER_ELEMENT is also a static own property of the constructor itself (spec 23.2.6.1), not just the prototype.
            PropertyDescriptor ctor_bpe_desc(Value(static_cast<double>(info.bytes)), PropertyAttributes::None);
            ctor_bpe_desc.set_enumerable(false);
            ctor_bpe_desc.set_writable(false);
            ctor_bpe_desc.set_configurable(false);
            ctor->set_property_descriptor("BYTES_PER_ELEMENT", ctor_bpe_desc);
        }
    }

    auto dataview_prototype = ObjectFactory::create_object();
    Object* dataview_proto_ptr = dataview_prototype.get();

    auto dataview_constructor = ObjectFactory::create_native_constructor("DataView",
        [dataview_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            Value result = DataView::constructor(ctx, args);
            if (!result.is_object()) return result;

            // GetPrototypeFromConstructor runs after offset/length validation,
            // so a throwing prototype getter is never reached on invalid input.
            Object* proto = dataview_proto_ptr;
            Value nt = ctx.get_new_target();
            if (nt.is_object() || nt.is_function()) {
                Object* nt_obj = nt.is_function() ? static_cast<Object*>(nt.as_function())
                                                  : nt.as_object();
                Value p = nt_obj->get_property("prototype");
                if (ctx.has_exception()) return Value();
                if (p.is_object()) proto = p.as_object();
                else if (p.is_function()) proto = static_cast<Object*>(p.as_function());
            }
            result.as_object()->set_prototype(proto);
            return result;
        });

    auto get_uint8_proto = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8, 1);
    dataview_prototype->set_property("getUint8", Value(get_uint8_proto.release()));

    auto set_uint8_proto = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8, 2);
    dataview_prototype->set_property("setUint8", Value(set_uint8_proto.release()));

    auto get_int8_proto = ObjectFactory::create_native_function("getInt8", DataView::js_get_int8, 1);
    dataview_prototype->set_property("getInt8", Value(get_int8_proto.release()));

    auto set_int8_proto = ObjectFactory::create_native_function("setInt8", DataView::js_set_int8, 2);
    dataview_prototype->set_property("setInt8", Value(set_int8_proto.release()));

    auto get_int16_proto = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16, 1);
    dataview_prototype->set_property("getInt16", Value(get_int16_proto.release()));

    auto set_int16_proto = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16, 2);
    dataview_prototype->set_property("setInt16", Value(set_int16_proto.release()));

    auto get_uint16_proto = ObjectFactory::create_native_function("getUint16", DataView::js_get_uint16, 1);
    dataview_prototype->set_property("getUint16", Value(get_uint16_proto.release()));

    auto set_uint16_proto = ObjectFactory::create_native_function("setUint16", DataView::js_set_uint16, 2);
    dataview_prototype->set_property("setUint16", Value(set_uint16_proto.release()));

    auto get_int32_proto = ObjectFactory::create_native_function("getInt32", DataView::js_get_int32, 1);
    dataview_prototype->set_property("getInt32", Value(get_int32_proto.release()));

    auto set_int32_proto = ObjectFactory::create_native_function("setInt32", DataView::js_set_int32, 2);
    dataview_prototype->set_property("setInt32", Value(set_int32_proto.release()));

    auto get_uint32_proto = ObjectFactory::create_native_function("getUint32", DataView::js_get_uint32, 1);
    dataview_prototype->set_property("getUint32", Value(get_uint32_proto.release()));

    auto set_uint32_proto = ObjectFactory::create_native_function("setUint32", DataView::js_set_uint32, 2);
    dataview_prototype->set_property("setUint32", Value(set_uint32_proto.release()));

    auto get_float32_proto = ObjectFactory::create_native_function("getFloat32", DataView::js_get_float32, 1);
    dataview_prototype->set_property("getFloat32", Value(get_float32_proto.release()));

    auto set_float32_proto = ObjectFactory::create_native_function("setFloat32", DataView::js_set_float32, 2);
    dataview_prototype->set_property("setFloat32", Value(set_float32_proto.release()));

    auto get_float64_proto = ObjectFactory::create_native_function("getFloat64", DataView::js_get_float64, 1);
    dataview_prototype->set_property("getFloat64", Value(get_float64_proto.release()));

    auto set_float64_proto = ObjectFactory::create_native_function("setFloat64", DataView::js_set_float64, 2);
    dataview_prototype->set_property("setFloat64", Value(set_float64_proto.release()));

    auto get_bigint64_proto = ObjectFactory::create_native_function("getBigInt64", DataView::js_get_bigint64, 1);
    dataview_prototype->set_property("getBigInt64", Value(get_bigint64_proto.release()));

    auto set_bigint64_proto = ObjectFactory::create_native_function("setBigInt64", DataView::js_set_bigint64, 2);
    dataview_prototype->set_property("setBigInt64", Value(set_bigint64_proto.release()));

    auto get_biguint64_proto = ObjectFactory::create_native_function("getBigUint64", DataView::js_get_biguint64, 1);
    dataview_prototype->set_property("getBigUint64", Value(get_biguint64_proto.release()));

    auto set_biguint64_proto = ObjectFactory::create_native_function("setBigUint64", DataView::js_set_biguint64, 2);
    dataview_prototype->set_property("setBigUint64", Value(set_biguint64_proto.release()));

    auto get_float16_proto = ObjectFactory::create_native_function("getFloat16", DataView::js_get_float16, 1);
    dataview_prototype->set_property_descriptor("getFloat16",
        PropertyDescriptor(Value(get_float16_proto.release()), PropertyAttributes::BuiltinFunction));

    auto set_float16_proto = ObjectFactory::create_native_function("setFloat16", DataView::js_set_float16, 2);
    dataview_prototype->set_property_descriptor("setFloat16",
        PropertyDescriptor(Value(set_float16_proto.release()), PropertyAttributes::BuiltinFunction));

    dataview_prototype->set_property_descriptor("constructor",
        PropertyDescriptor(Value(dataview_constructor.get()),
            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));

    PropertyDescriptor dataview_tag_desc(Value(std::string("DataView")), PropertyAttributes::Configurable);
    dataview_prototype->set_property_descriptor("Symbol.toStringTag", dataview_tag_desc);

    // Accessor properties on DataView.prototype: buffer, byteLength, byteOffset
    auto dv_check = [](Context& ctx) -> DataView* {
        Object* obj = ctx.get_this_binding();
        if (!obj || !obj->is_data_view()) { ctx.throw_type_error("DataView accessor called on non-DataView"); return nullptr; }
        return static_cast<DataView*>(obj);
    };
    {
        auto g = ObjectFactory::create_native_function("get buffer",
            [dv_check](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; DataView* dv = dv_check(ctx); if (!dv) return Value();
                return Value(dv->buffer());
            }, 0);
        PropertyDescriptor d; d.set_getter(g.release()); d.set_enumerable(false); d.set_configurable(true);
        dataview_prototype->set_property_descriptor("buffer", d);
    }
    {
        auto g = ObjectFactory::create_native_function("get byteLength",
            [dv_check](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; DataView* dv = dv_check(ctx); if (!dv) return Value();
                if (dv->is_out_of_bounds()) { ctx.throw_type_error("DataView is out of bounds of its buffer"); return Value(); }
                return Value(static_cast<double>(dv->current_byte_length()));
            }, 0);
        PropertyDescriptor d; d.set_getter(g.release()); d.set_enumerable(false); d.set_configurable(true);
        dataview_prototype->set_property_descriptor("byteLength", d);
    }
    {
        auto g = ObjectFactory::create_native_function("get byteOffset",
            [dv_check](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; DataView* dv = dv_check(ctx); if (!dv) return Value();
                if (dv->is_out_of_bounds()) { ctx.throw_type_error("DataView is out of bounds of its buffer"); return Value(); }
                return Value(static_cast<double>(dv->byte_offset()));
            }, 0);
        PropertyDescriptor d; d.set_getter(g.release()); d.set_enumerable(false); d.set_configurable(true);
        dataview_prototype->set_property_descriptor("byteOffset", d);
    }

    dataview_constructor->set_property("prototype", Value(dataview_prototype.release()), PropertyAttributes::None);

    ctx.register_built_in_object("DataView", dataview_constructor.release());



    Value function_ctor_value = ctx.get_global_object()->get_property("Function");
    if (function_ctor_value.is_function()) {
        Function* function_ctor = function_ctor_value.as_function();
        Value func_proto_value = function_ctor->get_property("prototype");
        if (func_proto_value.is_object()) {
            Object* function_proto_ptr = func_proto_value.as_object();

            const char* constructor_names[] = {
                "Array", "Object", "String", "Number", "Boolean", "BigInt", "Symbol",
                "Error", "TypeError", "ReferenceError", "SyntaxError", "RangeError", "URIError", "EvalError", "AggregateError",
                "Promise", "Map", "Set", "WeakMap", "WeakSet",
                "Date", "RegExp", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray",
                "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
                "DataView"
            };

            for (const char* name : constructor_names) {
                Value ctor = ctx.get_global_object()->get_property(name);
                if (ctor.is_function()) {
                    Function* func = ctor.as_function();
                    static_cast<Object*>(func)->set_prototype(function_proto_ptr);
                }
            }

            // ES6: Typed array constructors' __proto__ should be %TypedArray%, not Function.prototype
            Object* ta_ctor = ctx.get_built_in_object("TypedArray");
            if (ta_ctor) {
                const char* ta_names[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array"};
                for (const char* name : ta_names) {
                    Value ctor = ctx.get_global_object()->get_property(name);
                    if (ctor.is_function()) {
                        static_cast<Object*>(ctor.as_function())->set_prototype(ta_ctor);
                    }
                }
            }
        }
    }
    register_uint8array_base64_hex(ctx);
}

// ES2025: Uint8Array.prototype.toBase64/toHex/setFromBase64/setFromHex
static void register_uint8array_base64_hex(Context& ctx) {
    Object* u8ctor = ctx.get_built_in_object("Uint8Array");
    if (!u8ctor) return;
    Value proto_val = u8ctor->get_property("prototype");
    if (!proto_val.is_object()) return;
    Object* proto = proto_val.as_object();

    proto->set_property("toHex", Value(ObjectFactory::create_native_function("toHex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* obj = ctx.get_this_binding();
            if (!obj || !obj->is_typed_array()) { ctx.throw_type_error("toHex: not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(obj);
            if (ta->get_array_type() != TypedArrayBase::ArrayType::UINT8) { ctx.throw_type_error("toHex: requires Uint8Array"); return Value(); }
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("toHex: TypedArray is out of bounds"); return Value(); }
            static const char hex[] = "0123456789abcdef";
            std::string result;
            result.reserve(ta->length() * 2);
            for (size_t i = 0; i < ta->length(); i++) {
                uint8_t b = static_cast<uint8_t>(ta->get_element(i).to_number());
                result += hex[b >> 4];
                result += hex[b & 0xF];
            }
            return Value(result);
        }, 0).release()), PropertyAttributes::BuiltinFunction);

    proto->set_property("toBase64", Value(ObjectFactory::create_native_function("toBase64",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || !obj->is_typed_array()) { ctx.throw_type_error("toBase64: not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(obj);
            if (ta->get_array_type() != TypedArrayBase::ArrayType::UINT8) { ctx.throw_type_error("toBase64: requires Uint8Array"); return Value(); }
            if (ta->is_out_of_bounds()) { ctx.throw_type_error("toBase64: TypedArray is out of bounds"); return Value(); }
            bool url_safe = false, omit_padding = false;
            if (!args.empty() && args[0].is_object()) {
                Value alph = args[0].as_object()->get_property("alphabet");
                if (!alph.is_undefined() && alph.to_string() == "base64url") url_safe = true;
                Value op = args[0].as_object()->get_property("omitPadding");
                if (!op.is_undefined()) omit_padding = op.to_boolean();
            }
            const char* tbl = url_safe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                                        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string result;
            size_t n = ta->length();
            for (size_t i = 0; i < n; i += 3) {
                uint8_t b0 = static_cast<uint8_t>(ta->get_element(i).to_number());
                uint8_t b1 = i+1 < n ? static_cast<uint8_t>(ta->get_element(i+1).to_number()) : 0;
                uint8_t b2 = i+2 < n ? static_cast<uint8_t>(ta->get_element(i+2).to_number()) : 0;
                result += tbl[b0 >> 2];
                result += tbl[((b0 & 3) << 4) | (b1 >> 4)];
                if (i+1 < n) result += tbl[((b1 & 0xF) << 2) | (b2 >> 6)];
                else if (!omit_padding) result += '=';
                if (i+2 < n) result += tbl[b2 & 0x3F];
                else if (!omit_padding) result += '=';
            }
            return Value(result);
        }, 0).release()), PropertyAttributes::BuiltinFunction);

    proto->set_property("setFromHex", Value(ObjectFactory::create_native_function("setFromHex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || !obj->is_typed_array()) { ctx.throw_type_error("setFromHex: not a TypedArray"); return Value(); }
            TypedArrayBase* ta2 = static_cast<TypedArrayBase*>(obj);
            if (ta2->get_array_type() != TypedArrayBase::ArrayType::UINT8) { ctx.throw_type_error("setFromHex: requires Uint8Array"); return Value(); }
            if (ta2->is_out_of_bounds()) { ctx.throw_type_error("setFromHex: TypedArray is out of bounds"); return Value(); }
            if (args.empty() || !args[0].is_string()) { ctx.throw_type_error("setFromHex requires a string"); return Value(); }
            TypedArrayBase* ta = ta2;
            const std::string& hex = args[0].as_string()->str();
            if (hex.size() % 2 != 0) { ctx.throw_syntax_error("setFromHex: odd-length string"); return Value(); }
            auto h = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            size_t written = 0;
            for (size_t i = 0; i < hex.size() && written < ta->length(); i += 2, written++) {
                int hi = h(hex[i]), lo = h(hex[i+1]);
                if (hi < 0 || lo < 0) { ctx.throw_syntax_error("setFromHex: invalid hex character"); return Value(); }
                ta->set_element(written, Value((double)((hi << 4) | lo)));
            }
            auto res = ObjectFactory::create_object();
            res->set_property("read", Value((double)(written * 2)));
            res->set_property("written", Value((double)written));
            return Value(res.release());
        }, 1).release()), PropertyAttributes::BuiltinFunction);

    proto->set_property("setFromBase64", Value(ObjectFactory::create_native_function("setFromBase64",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || !obj->is_typed_array()) { ctx.throw_type_error("setFromBase64: not a TypedArray"); return Value(); }
            TypedArrayBase* ta2 = static_cast<TypedArrayBase*>(obj);
            if (ta2->get_array_type() != TypedArrayBase::ArrayType::UINT8) { ctx.throw_type_error("setFromBase64: requires Uint8Array"); return Value(); }
            if (ta2->is_out_of_bounds()) { ctx.throw_type_error("setFromBase64: TypedArray is out of bounds"); return Value(); }
            if (args.empty() || !args[0].is_string()) { ctx.throw_type_error("setFromBase64 requires a string"); return Value(); }
            TypedArrayBase* ta = ta2;
            std::string str = args[0].as_string()->str();
            bool url_safe = false;
            if (args.size() > 1 && args[1].is_object()) {
                Value alph = args[1].as_object()->get_property("alphabet");
                if (!alph.is_undefined() && alph.to_string() == "base64url") url_safe = true;
            }
            auto decode_char = [url_safe](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (url_safe) { if (c == '-') return 62; if (c == '_') return 63; }
                else          { if (c == '+') return 62; if (c == '/') return 63; }
                return -1;
            };
            std::string clean;
            size_t chars_read = 0;
            for (size_t i = 0; i < str.size(); i++) {
                char c = str[i];
                if (c == '=') { chars_read = i; break; }
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
                clean += c;
                chars_read = i + 1;
            }
            if (chars_read == 0 && !str.empty()) chars_read = str.size();
            size_t written = 0;
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i < clean.size(); ) {
                int b0 = i < clean.size() ? decode_char(clean[i++]) : 0;
                int b1 = i < clean.size() ? decode_char(clean[i++]) : 0;
                int b2_absent = (i >= clean.size()); int b2 = !b2_absent ? decode_char(clean[i++]) : 0;
                int b3_absent = (i >= clean.size()); int b3 = !b3_absent ? decode_char(clean[i++]) : 0;
                if (b0 < 0 || b1 < 0 || (!b2_absent && b2 < 0) || (!b3_absent && b3 < 0)) {
                    ctx.throw_syntax_error("setFromBase64: invalid character"); return Value();
                }
                bytes.push_back((b0 << 2) | (b1 >> 4));
                if (!b2_absent) bytes.push_back(((b1 & 0xF) << 4) | (b2 >> 2));
                if (!b3_absent) bytes.push_back(((b2 & 0x3) << 6) | b3);
            }
            written = std::min(bytes.size(), ta->length());
            for (size_t i = 0; i < written; i++) ta->set_element(i, Value((double)bytes[i]));
            auto res = ObjectFactory::create_object();
            res->set_property("read", Value((double)chars_read));
            res->set_property("written", Value((double)written));
            return Value(res.release());
        }, 1).release()), PropertyAttributes::BuiltinFunction);
}

} // namespace Quanta
