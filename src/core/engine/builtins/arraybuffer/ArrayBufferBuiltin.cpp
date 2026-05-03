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

namespace Quanta {

void register_arraybuffer_builtins(Context& ctx) {
    auto arraybuffer_constructor = ObjectFactory::create_native_constructor("ArrayBuffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            double length_double = 0.0;

            if (!args.empty()) {
                if (!args[0].is_number()) {
                    ctx.throw_type_error("ArrayBuffer size must be a number");
                    return Value();
                }
                length_double = args[0].as_number();
            }
            if (length_double < 0 || length_double != std::floor(length_double)) {
                ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
                return Value();
            }
            
            size_t byte_length = static_cast<size_t>(length_double);
            
            try {
                auto buffer_obj = std::make_unique<ArrayBuffer>(byte_length);
                buffer_obj->set_property("byteLength", Value(static_cast<double>(byte_length)));
                buffer_obj->set_property("_isArrayBuffer", Value(true));
                
                if (ctx.has_binding("ArrayBuffer")) {
                    Value arraybuffer_ctor = ctx.get_binding("ArrayBuffer");
                    if (!arraybuffer_ctor.is_undefined()) {
                        buffer_obj->set_property("constructor", arraybuffer_ctor);
                    }
                }
                
                return Value(buffer_obj.release());
            } catch (const std::exception& e) {
                ctx.throw_error(std::string("ArrayBuffer allocation failed: ") + e.what());
                return Value();
            }
        });
    
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

    auto arraybuffer_prototype = ObjectFactory::create_object();

    auto byteLength_getter = ObjectFactory::create_native_function("get byteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer()) {
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
            if (!this_obj || !this_obj->is_array_buffer()) {
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
            return Value();
        }, 2);

    ab_slice_fn->set_property("name", Value(std::string("slice")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("slice", Value(ab_slice_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_resize_fn = ObjectFactory::create_native_function("resize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        }, 1);

    ab_resize_fn->set_property("name", Value(std::string("resize")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("resize", Value(ab_resize_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_transfer_fn = ObjectFactory::create_native_function("transfer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        }, 0);

    ab_transfer_fn->set_property("name", Value(std::string("transfer")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("transfer", Value(ab_transfer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_maxByteLength_fn = ObjectFactory::create_native_function("get maxByteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("ArrayBuffer.prototype.maxByteLength called on non-ArrayBuffer");
                return Value();
            }

            if (this_obj->has_property("maxByteLength")) {
                return this_obj->get_property("maxByteLength");
            }

            if (this_obj->has_property("byteLength")) {
                return this_obj->get_property("byteLength");
            }

            return Value(0.0);
        }, 0);

    PropertyDescriptor maxByteLength_desc(Value(ab_maxByteLength_fn.release()), PropertyAttributes::Configurable);
    maxByteLength_desc.set_enumerable(false);
    arraybuffer_prototype->set_property_descriptor("maxByteLength", maxByteLength_desc);

    auto ab_resizable_fn = ObjectFactory::create_native_function("get resizable",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("ArrayBuffer.prototype.resizable called on non-ArrayBuffer");
                return Value();
            }

            if (this_obj->has_property("maxByteLength") && this_obj->has_property("byteLength")) {
                Value max = this_obj->get_property("maxByteLength");
                Value current = this_obj->get_property("byteLength");
                if (max.is_number() && current.is_number()) {
                    return Value(max.as_number() != current.as_number());
                }
            }

            return Value(false);
        }, 0);

    PropertyDescriptor resizable_desc(Value(ab_resizable_fn.release()), PropertyAttributes::Configurable);
    resizable_desc.set_enumerable(false);
    arraybuffer_prototype->set_property_descriptor("resizable", resizable_desc);

    auto ab_transferToFixedLength_fn = ObjectFactory::create_native_function("transferToFixedLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            return Value();
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

    arraybuffer_constructor->set_property("prototype", Value(arraybuffer_prototype.release()));

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
            auto op_fn = ObjectFactory::create_native_function(op_name,
                [](Context&, const std::vector<Value>&) -> Value { return Value(0.0); }, 3);
            atomics_obj->set_property(op_name, Value(op_fn.release()), PropertyAttributes::BuiltinFunction);
        }
        ctx.register_built_in_object("Atomics", atomics_obj.release());
    }

    // ES2017: SharedArrayBuffer stub constructor
    {
        auto sab_constructor = ObjectFactory::create_native_constructor("SharedArrayBuffer",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                double byte_length = args.empty() ? 0.0 : args[0].to_number();
                auto buf = ObjectFactory::create_object();
                buf->set_property("byteLength", Value(byte_length), PropertyAttributes::None);
                buf->set_property("_isSharedArrayBuffer", Value(true));
                // Set prototype
                if (ctx.has_binding("SharedArrayBuffer")) {
                    Value ctor = ctx.get_binding("SharedArrayBuffer");
                    if (ctor.is_function()) {
                        Value proto = ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) buf->set_prototype(proto.as_object());
                    }
                }
                return Value(buf.release());
            }, 1);

        auto sab_proto = ObjectFactory::create_object();

        // byteLength getter
        auto byte_length_getter = ObjectFactory::create_native_function("get byteLength",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) return Value(0.0);
                return this_obj->get_property("byteLength");
            }, 0);
        PropertyDescriptor bl_desc;
        bl_desc.set_getter(byte_length_getter.release());
        bl_desc.set_configurable(true);
        sab_proto->set_property_descriptor("byteLength", bl_desc);

        // slice stub
        auto sab_slice = ObjectFactory::create_native_function("slice",
            [](Context&, const std::vector<Value>&) -> Value {
                auto obj = ObjectFactory::create_object();
                return Value(static_cast<Object*>(obj.release()));
            }, 2);
        sab_proto->set_property("slice", Value(sab_slice.release()), PropertyAttributes::BuiltinFunction);

        // Symbol.toStringTag = "SharedArrayBuffer"
        Symbol* to_string_tag = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (to_string_tag) {
            PropertyDescriptor tag_desc(Value(std::string("SharedArrayBuffer")),
                static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sab_proto->set_property_descriptor(to_string_tag->to_property_key(), tag_desc);
        }

        sab_constructor->set_property("prototype", Value(sab_proto.release()), PropertyAttributes::None);

        // Symbol.species getter
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    return ctx.get_binding("SharedArrayBuffer");
                }, 0);
            PropertyDescriptor species_desc;
            species_desc.set_getter(species_getter.release());
            species_desc.set_configurable(true);
            sab_constructor->set_property_descriptor(species_sym->to_property_key(), species_desc);
        }

        ctx.register_built_in_object("SharedArrayBuffer", sab_constructor.release());
    }
}

} // namespace Quanta
