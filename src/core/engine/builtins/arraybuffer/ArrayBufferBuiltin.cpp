/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/ArrayBufferBuiltin.h"
#include "quanta/core/engine/builtins/TypedArrayBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include <cmath>

namespace Quanta {

// Value::to_number() silently returns NaN instead of throwing when ToPrimitive fails.
static double to_number_checked(Context& ctx, const Value& v) {
    double n = v.to_number();
    if (!std::isnan(n) || !v.is_object()) return n;
    Object* obj = v.as_object();
    bool got_primitive = false;
    Value valueOf_fn = obj->get_property("valueOf");
    if (ctx.has_exception()) return 0.0;
    if (valueOf_fn.is_function()) {
        Value prim = valueOf_fn.as_function()->call(ctx, {}, v);
        if (ctx.has_exception()) return 0.0;
        got_primitive = !prim.is_object() && !prim.is_function();
    }
    if (!got_primitive) {
        Value toString_fn = obj->get_property("toString");
        if (ctx.has_exception()) return 0.0;
        if (toString_fn.is_function()) {
            Value prim = toString_fn.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return 0.0;
            got_primitive = !prim.is_object() && !prim.is_function();
        }
    }
    if (!got_primitive) {
        ctx.throw_type_error("Cannot convert object to primitive value");
        return 0.0;
    }
    return n;
}

// ToIntegerOrInfinity: NaN -> 0, +-Infinity pass through, finite values truncate toward zero.
static double to_integer_or_infinity_checked(Context& ctx, const Value& v) {
    double number = to_number_checked(ctx, v);
    if (ctx.has_exception()) return 0.0;
    if (std::isnan(number)) return 0.0;
    if (std::isinf(number)) return number;
    return number < 0 ? -std::floor(-number) : std::floor(number);
}

// ToIndex: undefined -> 0, NaN -> 0, RangeError outside [0, 2^53-1].
static double to_index_checked(Context& ctx, const Value& v) {
    if (v.is_undefined()) return 0.0;
    double integer = to_integer_or_infinity_checked(ctx, v);
    if (ctx.has_exception()) return 0.0;
    if (integer < 0 || integer > 9007199254740991.0) {
        ctx.throw_range_error("Invalid index: out of range");
        return 0.0;
    }
    return integer;
}

// Reject before actually attempting posix_memalign/memset on a multi-petabyte request.
static constexpr double kMaxAllocatableBytes = 4294967296.0; // 4 GiB

// GetPrototypeFromConstructor: new.target's own "prototype", falling back to default_proto.
static Object* resolve_new_target_prototype(Context& ctx, Object* default_proto) {
    Value nt = ctx.get_new_target();
    if (!nt.is_object() && !nt.is_function()) return default_proto;
    Object* nt_obj = nt.is_function() ? static_cast<Object*>(nt.as_function()) : nt.as_object();
    Value p = nt_obj->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    if (p.is_object()) return p.as_object();
    if (p.is_function()) return static_cast<Object*>(p.as_function());
    return default_proto;
}

// SpeciesConstructor(O, %ArrayBuffer%) then Construct(C, « newLen »), with result invariants checked.
// SpeciesConstructor(O, %ArrayBuffer%|%SharedArrayBuffer%) then Construct(C, « newLen »).
// for_shared flips the default constructor and which buffer kind the result must be.
static ArrayBuffer* array_buffer_species_create(Context& ctx, Object* o, size_t new_len, bool for_shared = false) {
    Value default_ctor = ctx.get_binding(for_shared ? "SharedArrayBuffer" : "ArrayBuffer");
    Function* ctor_fn = default_ctor.is_function() ? default_ctor.as_function() : nullptr;

    Value c = o->get_property("constructor");
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

    Value result = ctor_fn->construct(ctx, {Value(static_cast<double>(new_len))});
    if (ctx.has_exception()) return nullptr;
    if (!result.is_object() || !result.as_object()->is_array_buffer()) {
        ctx.throw_type_error("Species constructor did not return an ArrayBuffer");
        return nullptr;
    }
    ArrayBuffer* new_ab = static_cast<ArrayBuffer*>(result.as_object());
    if (new_ab->is_shared_array_buffer() != for_shared) {
        ctx.throw_type_error(for_shared ? "Species constructor did not return a SharedArrayBuffer"
                                        : "Species constructor returned a SharedArrayBuffer");
        return nullptr;
    }
    if (!for_shared && new_ab->is_detached()) { ctx.throw_type_error("Species constructor returned a detached ArrayBuffer"); return nullptr; }
    if (static_cast<Object*>(new_ab) == o) { ctx.throw_type_error("Species constructor returned the same ArrayBuffer"); return nullptr; }
    if (new_ab->byte_length() < new_len) { ctx.throw_type_error("Species constructor returned a too-small ArrayBuffer"); return nullptr; }
    return new_ab;
}

// ArrayBufferCopyAndDetach: allocate-new + copy + detach-source (observably same as a real zero-copy transfer).
static Value array_buffer_copy_and_detach(Context& ctx, Object* this_obj, const std::vector<Value>& args, bool preserve_resizability) {
    if (!this_obj || !this_obj->is_array_buffer()) { ctx.throw_type_error("not an ArrayBuffer"); return Value(); }
    if (this_obj->is_shared_array_buffer()) { ctx.throw_type_error("Cannot transfer a SharedArrayBuffer"); return Value(); }
    ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
    if (ab->is_detached()) { ctx.throw_type_error("Cannot transfer a detached ArrayBuffer"); return Value(); }

    double new_len_double = (args.empty() || args[0].is_undefined())
        ? static_cast<double>(ab->byte_length()) : to_number_checked(ctx, args[0]);
    if (ctx.has_exception()) return Value();
    if (std::isnan(new_len_double) || new_len_double < 0 || new_len_double != std::floor(new_len_double) ||
        new_len_double > 9007199254740991.0) {
        ctx.throw_range_error("Invalid transfer length");
        return Value();
    }
    size_t new_len = static_cast<size_t>(new_len_double);

    std::unique_ptr<ArrayBuffer> new_buffer;
    if (preserve_resizability && ab->is_resizable()) {
        if (new_len > ab->max_byte_length()) { ctx.throw_range_error("ArrayBuffer size cannot exceed maxByteLength"); return Value(); }
        new_buffer = std::make_unique<ArrayBuffer>(new_len, ab->max_byte_length());
    } else {
        new_buffer = std::make_unique<ArrayBuffer>(new_len);
    }

    size_t copy_len = std::min(new_len, ab->byte_length());
    if (copy_len > 0) {
        std::vector<uint8_t> tmp(copy_len);
        ab->read_bytes(0, tmp.data(), copy_len);
        new_buffer->write_bytes(0, tmp.data(), copy_len);
    }
    ab->detach();
    return Value(new_buffer.release());
}

void register_arraybuffer_builtins(Context& ctx) {
    auto arraybuffer_prototype = ObjectFactory::create_object();
    Object* arraybuffer_proto_ptr = arraybuffer_prototype.get();

    auto arraybuffer_constructor = ObjectFactory::create_native_constructor("ArrayBuffer",
        [arraybuffer_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }

            double byte_length_d = to_index_checked(ctx, args.empty() ? Value() : args[0]);
            if (ctx.has_exception()) return Value();

            bool has_max = false;
            double max_byte_length_d = 0.0;
            if (args.size() > 1 && (args[1].is_object() || args[1].is_function())) {
                Object* opts = args[1].is_function() ? static_cast<Object*>(args[1].as_function()) : args[1].as_object();
                Value mbl = opts->get_property("maxByteLength");
                if (ctx.has_exception()) return Value();
                if (!mbl.is_undefined()) {
                    max_byte_length_d = to_index_checked(ctx, mbl);
                    if (ctx.has_exception()) return Value();
                    has_max = true;
                }
            }
            if (has_max && byte_length_d > max_byte_length_d) {
                ctx.throw_range_error("ArrayBuffer size cannot exceed maxByteLength");
                return Value();
            }

            // Prototype resolves before allocation: a throwing prototype getter
            // must preempt a RangeError from an oversized request.
            Object* proto = resolve_new_target_prototype(ctx, arraybuffer_proto_ptr);
            if (ctx.has_exception()) return Value();

            if ((has_max ? max_byte_length_d : byte_length_d) > kMaxAllocatableBytes) {
                ctx.throw_range_error("ArrayBuffer allocation size is too large");
                return Value();
            }

            try {
                std::unique_ptr<ArrayBuffer> buffer_obj;
                if (has_max) {
                    buffer_obj = std::make_unique<ArrayBuffer>(static_cast<size_t>(byte_length_d), static_cast<size_t>(max_byte_length_d));
                } else {
                    buffer_obj = std::make_unique<ArrayBuffer>(static_cast<size_t>(byte_length_d));
                }
                buffer_obj->set_property("_isArrayBuffer", Value(true));
                buffer_obj->set_prototype(proto);
                return Value(buffer_obj.release());
            } catch (const std::exception&) {
                ctx.throw_range_error("ArrayBuffer allocation failed: out of memory");
                return Value();
            }
        }, 1);
    
    auto arraybuffer_isView = ObjectFactory::create_native_function("isView",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();

            if (obj->has_property("buffer") || obj->has_property("byteLength")) {
                Value buffer_val = obj->get_property("buffer");
                if (buffer_val.is_object()) {
                    return Value(true);
                }
            }

            return Value(false);
        });

    PropertyDescriptor isView_length_desc(Value(1.0), PropertyAttributes::None);
    isView_length_desc.set_configurable(true);
    isView_length_desc.set_enumerable(false);
    isView_length_desc.set_writable(false);
    arraybuffer_isView->set_property_descriptor("length", isView_length_desc);

    arraybuffer_constructor->set_property("isView", Value(arraybuffer_isView.release()), PropertyAttributes::BuiltinFunction);

    auto byteLength_getter = ObjectFactory::create_native_function("get byteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer() || this_obj->is_shared_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.byteLength called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            return Value(static_cast<double>(ab->byte_length()));
        }, 0);

    PropertyDescriptor byteLength_desc;
    byteLength_desc.set_getter(byteLength_getter.release());
    byteLength_desc.set_enumerable(false);
    byteLength_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("byteLength", byteLength_desc);

    auto detached_getter = ObjectFactory::create_native_function("get detached",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer() || this_obj->is_shared_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.detached called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            return Value(ab->is_detached());
        }, 0);

    PropertyDescriptor detached_desc;
    detached_desc.set_getter(detached_getter.release());
    detached_desc.set_enumerable(false);
    detached_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("detached", detached_desc);

    auto ab_slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer()) { ctx.throw_type_error("ArrayBuffer.prototype.slice called on non-ArrayBuffer"); return Value(); }
            if (this_obj->is_shared_array_buffer()) { ctx.throw_type_error("ArrayBuffer.prototype.slice called on a SharedArrayBuffer"); return Value(); }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            if (ab->is_detached()) { ctx.throw_type_error("Cannot slice a detached ArrayBuffer"); return Value(); }
            double len = static_cast<double>(ab->byte_length());
            double start = 0, end = len;
            if (!args.empty()) {
                double s = to_integer_or_infinity_checked(ctx, args[0]);
                if (ctx.has_exception()) return Value();
                start = s < 0 ? std::max(len + s, 0.0) : std::min(s, len);
            }
            if (args.size() > 1 && !args[1].is_undefined()) {
                double e = to_integer_or_infinity_checked(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                end = e < 0 ? std::max(len + e, 0.0) : std::min(e, len);
            }
            size_t new_len = end > start ? static_cast<size_t>(end - start) : 0;
            size_t start_offset = static_cast<size_t>(start);
            ArrayBuffer* new_ab = array_buffer_species_create(ctx, this_obj, new_len);
            if (!new_ab) return Value();
            if (ab->is_detached()) { ctx.throw_type_error("ArrayBuffer was detached during species construction"); return Value(); }
            std::vector<uint8_t> tmp(new_len);
            if (new_len > 0) {
                ab->read_bytes(start_offset, tmp.data(), new_len);
                new_ab->write_bytes(0, tmp.data(), new_len);
            }
            return Value(new_ab);
        }, 2);

    ab_slice_fn->set_property("name", Value(std::string("slice")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("slice", Value(ab_slice_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_resize_fn = ObjectFactory::create_native_function("resize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer() || this_obj->is_shared_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.resize called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            if (!ab->is_resizable()) {
                ctx.throw_type_error("Cannot resize a non-resizable ArrayBuffer");
                return Value();
            }
            // ToIndex(newLength) runs before the detached check: a valueOf that
            // detaches the buffer as a side effect must still be observed.
            double new_len_double = to_index_checked(ctx, args.empty() ? Value() : args[0]);
            if (ctx.has_exception()) return Value();
            if (ab->is_detached()) {
                ctx.throw_type_error("Cannot resize a detached ArrayBuffer");
                return Value();
            }
            if (new_len_double > ab->max_byte_length()) {
                ctx.throw_range_error("ArrayBuffer size cannot exceed maxByteLength");
                return Value();
            }
            ab->resize(static_cast<size_t>(new_len_double));
            return Value();
        }, 1);

    ab_resize_fn->set_property("name", Value(std::string("resize")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("resize", Value(ab_resize_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_transfer_fn = ObjectFactory::create_native_function("transfer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return array_buffer_copy_and_detach(ctx, ctx.get_this_binding(), args, true);
        }, 0);

    ab_transfer_fn->set_property("name", Value(std::string("transfer")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("transfer", Value(ab_transfer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_maxByteLength_fn = ObjectFactory::create_native_function("get maxByteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer() || this_obj->is_shared_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.maxByteLength called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            if (ab->is_detached()) return Value(0.0);
            return Value(static_cast<double>(ab->is_resizable() ? ab->max_byte_length() : ab->byte_length()));
        }, 0);

    PropertyDescriptor maxByteLength_desc;
    maxByteLength_desc.set_getter(ab_maxByteLength_fn.release());
    maxByteLength_desc.set_enumerable(false);
    maxByteLength_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("maxByteLength", maxByteLength_desc);

    auto ab_resizable_fn = ObjectFactory::create_native_function("get resizable",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer() || this_obj->is_shared_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.resizable called on non-ArrayBuffer");
                return Value();
            }
            return Value(static_cast<ArrayBuffer*>(this_obj)->is_resizable());
        }, 0);

    PropertyDescriptor resizable_desc;
    resizable_desc.set_getter(ab_resizable_fn.release());
    resizable_desc.set_enumerable(false);
    resizable_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("resizable", resizable_desc);

    auto ab_transferToFixedLength_fn = ObjectFactory::create_native_function("transferToFixedLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return array_buffer_copy_and_detach(ctx, ctx.get_this_binding(), args, false);
        }, 0);

    ab_transferToFixedLength_fn->set_property("name", Value(std::string("transferToFixedLength")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("transferToFixedLength", Value(ab_transferToFixedLength_fn.release()), PropertyAttributes::BuiltinFunction);

    {
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("ArrayBuffer")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            arraybuffer_prototype->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
    }

    arraybuffer_prototype->set_property("constructor", Value(arraybuffer_constructor.get()), PropertyAttributes::BuiltinFunction);
    arraybuffer_constructor->set_property("prototype", Value(arraybuffer_prototype.release()), PropertyAttributes::None);

    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    return Value(ctx.get_this_binding());
                }, 0);
            PropertyDescriptor desc;
            desc.set_getter(getter.release());
            desc.set_enumerable(false);
            desc.set_configurable(true);
            arraybuffer_constructor->set_property_descriptor(species_sym->to_property_key(), desc);
        }
    }

    ctx.register_built_in_object("ArrayBuffer", arraybuffer_constructor.release());
    
    register_typed_array_builtins(ctx);

    // ES2017: Atomics object with stub operations
    {
        auto atomics_obj = ObjectFactory::create_object();
        const char* atomics_ops[] = {
            "add","and","compareExchange","exchange","isLockFree",
            "load","notify","or","store","sub","wait","xor", nullptr
        };
        for (int i = 0; atomics_ops[i]; ++i) {
            std::string op_name = atomics_ops[i];
            int op_len = (op_name == "isLockFree") ? 1 : 3;
            auto op_fn = ObjectFactory::create_native_function(op_name,
                [](Context&, const std::vector<Value>&) -> Value { return Value(0.0); }, op_len);
            atomics_obj->set_property(op_name, Value(op_fn.release()), PropertyAttributes::BuiltinFunction);
        }
        ctx.register_built_in_object("Atomics", atomics_obj.release());
    }

    // ES2017/ES2024: SharedArrayBuffer, with growable-buffer support (grow/growable/maxByteLength).
    {
        auto sab_prototype = ObjectFactory::create_object();
        Object* sab_proto_ptr = sab_prototype.get();

        auto sab_constructor = ObjectFactory::create_native_constructor("SharedArrayBuffer",
            [sab_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }

                double byte_length_d = to_index_checked(ctx, args.empty() ? Value() : args[0]);
                if (ctx.has_exception()) return Value();

                bool has_max = false;
                double max_byte_length_d = 0.0;
                if (args.size() > 1 && (args[1].is_object() || args[1].is_function())) {
                    Object* opts = args[1].is_function() ? static_cast<Object*>(args[1].as_function()) : args[1].as_object();
                    Value mbl = opts->get_property("maxByteLength");
                    if (ctx.has_exception()) return Value();
                    if (!mbl.is_undefined()) {
                        max_byte_length_d = to_index_checked(ctx, mbl);
                        if (ctx.has_exception()) return Value();
                        has_max = true;
                    }
                }
                // Unlike ArrayBuffer, this RangeError precedes prototype resolution.
                if (has_max && byte_length_d > max_byte_length_d) {
                    ctx.throw_range_error("SharedArrayBuffer size cannot exceed maxByteLength");
                    return Value();
                }

                Object* proto = resolve_new_target_prototype(ctx, sab_proto_ptr);
                if (ctx.has_exception()) return Value();

                if ((has_max ? max_byte_length_d : byte_length_d) > kMaxAllocatableBytes) {
                    ctx.throw_range_error("SharedArrayBuffer allocation size is too large");
                    return Value();
                }

                try {
                    std::unique_ptr<SharedArrayBuffer> buf;
                    if (has_max) {
                        buf = std::make_unique<SharedArrayBuffer>(static_cast<size_t>(byte_length_d), static_cast<size_t>(max_byte_length_d));
                    } else {
                        buf = std::make_unique<SharedArrayBuffer>(static_cast<size_t>(byte_length_d));
                    }
                    buf->set_prototype(proto);
                    return Value(buf.release());
                } catch (const std::exception&) {
                    ctx.throw_range_error("SharedArrayBuffer allocation failed: out of memory");
                    return Value();
                }
            }, 1);

        // RequireInternalSlot(this, [[ArrayBufferData]]) + IsSharedArrayBuffer.
        auto require_sab = [](Context& ctx, const char* name) -> SharedArrayBuffer* {
            Object* obj = ctx.get_this_binding();
            if (!obj || !obj->is_array_buffer() || !obj->is_shared_array_buffer()) {
                ctx.throw_type_error(std::string(name) + " requires a SharedArrayBuffer this");
                return nullptr;
            }
            return static_cast<SharedArrayBuffer*>(obj);
        };

        auto byte_length_getter = ObjectFactory::create_native_function("get byteLength",
            [require_sab](Context& ctx, const std::vector<Value>&) -> Value {
                ArrayBuffer* ab = require_sab(ctx, "byteLength");
                if (!ab) return Value();
                return Value(static_cast<double>(ab->byte_length()));
            }, 0);
        PropertyDescriptor bl_desc;
        bl_desc.set_getter(byte_length_getter.release());
        bl_desc.set_enumerable(false);
        bl_desc.set_configurable(true);
        sab_prototype->set_property_descriptor("byteLength", bl_desc);

        auto growable_getter = ObjectFactory::create_native_function("get growable",
            [require_sab](Context& ctx, const std::vector<Value>&) -> Value {
                ArrayBuffer* ab = require_sab(ctx, "growable");
                if (!ab) return Value();
                return Value(ab->is_resizable());
            }, 0);
        PropertyDescriptor growable_desc;
        growable_desc.set_getter(growable_getter.release());
        growable_desc.set_enumerable(false);
        growable_desc.set_configurable(true);
        sab_prototype->set_property_descriptor("growable", growable_desc);

        auto max_byte_length_getter = ObjectFactory::create_native_function("get maxByteLength",
            [require_sab](Context& ctx, const std::vector<Value>&) -> Value {
                ArrayBuffer* ab = require_sab(ctx, "maxByteLength");
                if (!ab) return Value();
                return Value(static_cast<double>(ab->is_resizable() ? ab->max_byte_length() : ab->byte_length()));
            }, 0);
        PropertyDescriptor mbl_desc;
        mbl_desc.set_getter(max_byte_length_getter.release());
        mbl_desc.set_enumerable(false);
        mbl_desc.set_configurable(true);
        sab_prototype->set_property_descriptor("maxByteLength", mbl_desc);

        auto sab_grow = ObjectFactory::create_native_function("grow",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* obj = ctx.get_this_binding();
                if (!obj || !obj->is_array_buffer() || !static_cast<ArrayBuffer*>(obj)->is_resizable()) {
                    ctx.throw_type_error("grow requires a growable SharedArrayBuffer this");
                    return Value();
                }
                if (!obj->is_shared_array_buffer()) { ctx.throw_type_error("grow requires a SharedArrayBuffer this"); return Value(); }
                ArrayBuffer* ab = static_cast<ArrayBuffer*>(obj);
                // Spec: ToIntegerOrInfinity here, not ToIndex (grow uses a plain range check below).
                double new_len = to_integer_or_infinity_checked(ctx, args.empty() ? Value() : args[0]);
                if (ctx.has_exception()) return Value();
                if (new_len < 0 || new_len > ab->max_byte_length()) {
                    ctx.throw_range_error("Invalid SharedArrayBuffer grow length");
                    return Value();
                }
                ab->resize(static_cast<size_t>(new_len));
                return Value();
            }, 1);
        sab_prototype->set_property_descriptor("grow",
            PropertyDescriptor(Value(sab_grow.release()), PropertyAttributes::BuiltinFunction));

        auto sab_slice = ObjectFactory::create_native_function("slice",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj || !this_obj->is_array_buffer() || !this_obj->is_shared_array_buffer()) {
                    ctx.throw_type_error("SharedArrayBuffer.prototype.slice called on non-SharedArrayBuffer");
                    return Value();
                }
                ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
                double len = static_cast<double>(ab->byte_length());
                double start = 0, end = len;
                if (!args.empty()) {
                    double s = to_integer_or_infinity_checked(ctx, args[0]);
                    if (ctx.has_exception()) return Value();
                    start = s < 0 ? std::max(len + s, 0.0) : std::min(s, len);
                }
                if (args.size() > 1 && !args[1].is_undefined()) {
                    double e = to_integer_or_infinity_checked(ctx, args[1]);
                    if (ctx.has_exception()) return Value();
                    end = e < 0 ? std::max(len + e, 0.0) : std::min(e, len);
                }
                size_t new_len = end > start ? static_cast<size_t>(end - start) : 0;
                size_t start_offset = static_cast<size_t>(start);
                ArrayBuffer* new_ab = array_buffer_species_create(ctx, this_obj, new_len, /*for_shared=*/true);
                if (!new_ab) return Value();
                std::vector<uint8_t> tmp(new_len);
                if (new_len > 0) {
                    ab->read_bytes(start_offset, tmp.data(), new_len);
                    new_ab->write_bytes(0, tmp.data(), new_len);
                }
                return Value(new_ab);
            }, 2);
        sab_prototype->set_property_descriptor("slice",
            PropertyDescriptor(Value(sab_slice.release()), PropertyAttributes::BuiltinFunction));

        Symbol* to_string_tag = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (to_string_tag) {
            PropertyDescriptor tag_desc(Value(std::string("SharedArrayBuffer")),
                static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sab_prototype->set_property_descriptor(to_string_tag->to_property_key(), tag_desc);
        }

        sab_prototype->set_property_descriptor("constructor",
            PropertyDescriptor(Value(sab_constructor.get()),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));
        sab_constructor->set_property("prototype", Value(sab_prototype.release()), PropertyAttributes::None);

        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    return Value(ctx.get_this_binding());
                }, 0);
            PropertyDescriptor species_desc;
            species_desc.set_getter(species_getter.release());
            species_desc.set_enumerable(false);
            species_desc.set_configurable(true);
            sab_constructor->set_property_descriptor(species_sym->to_property_key(), species_desc);
        }

        ctx.register_built_in_object("SharedArrayBuffer", sab_constructor.release());
    }
}

} // namespace Quanta
