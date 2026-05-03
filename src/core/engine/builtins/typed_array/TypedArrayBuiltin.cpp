/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/TypedArrayBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Symbol.h"
#include <cmath>
#include "quanta/parser/AST.h"
#include <algorithm>

namespace Quanta {

void register_typed_array_builtins(Context& ctx) {
    auto uint8array_constructor = ObjectFactory::create_native_constructor("Uint8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint8_array(0).release());
            }

            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint8_array(length).release());
            }

            if (args[0].is_object()) {
                Object* obj = args[0].as_object();

                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_uint8_array_from_buffer(buffer).release());
                }

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     (obj->has_property("length") ? static_cast<uint32_t>(obj->get_property("length").to_number()) : 0);

                    auto typed_array = TypedArrayFactory::create_uint8_array(length);

                    for (uint32_t i = 0; i < length; i++) {
                        Value element = obj->get_element(i);
                        typed_array->set_element(i, element);
                    }

                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_uint8_array(length);

                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }

                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint8_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Uint8Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Uint8Array", uint8array_constructor.release());

    auto uint8clampedarray_constructor = ObjectFactory::create_native_constructor("Uint8ClampedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                auto typed_array = TypedArrayFactory::create_uint8_clamped_array(0);
                return Value(typed_array.release());
            }

            const Value& arg = args[0];

            if (arg.is_number()) {
                size_t length = static_cast<size_t>(arg.to_number());
                auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);
                return Value(typed_array.release());
            }

            if (arg.is_object()) {
                Object* obj = arg.as_object();

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());

                    auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);

                    for (uint32_t i = 0; i < length; i++) {
                        Value element = obj->get_element(i);
                        typed_array->set_element(i, element);
                    }

                    return Value(typed_array.release());
                }

                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_uint8_clamped_array_from_buffer(buffer).release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);

                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }

                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint8_clamped_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Uint8ClampedArray constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Uint8ClampedArray", uint8clampedarray_constructor.release());

    auto float32array_constructor = ObjectFactory::create_native_constructor("Float32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_float32_array(0).release());
            }

            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_float32_array(length).release());
            }

            if (args[0].is_object()) {
                Object* obj = args[0].as_object();

                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_float32_array_from_buffer(buffer).release());
                }

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());
                    auto typed_array = TypedArrayFactory::create_float32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_float32_array(length);
                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_float32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Float32Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Float32Array", float32array_constructor.release());

    auto typedarray_constructor = ObjectFactory::create_native_function("TypedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            ctx.throw_type_error("Abstract class TypedArray not intended to be instantiated directly");
            return Value();
        }, 0);

    PropertyDescriptor typedarray_name_desc(Value(std::string("TypedArray")),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("name", typedarray_name_desc);

    PropertyDescriptor typedarray_length_desc(Value(0.0),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("length", typedarray_length_desc);

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
            return Value(ta->buffer());
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
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.at called on non-TypedArray");
                return Value();
            }

            if (args.empty()) return Value();

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t index = static_cast<int64_t>(args[0].to_number());
            int64_t len = static_cast<int64_t>(ta->length());

            if (index < 0) {
                index = len + index;
            }

            if (index < 0 || index >= len) {
                return Value();
            }

            return ta->get_element(static_cast<size_t>(index));
        }, 1);
    PropertyDescriptor typedarray_at_desc(Value(typedarray_at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("at", typedarray_at_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.forEach called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("forEach requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            for (size_t i = 0; i < length; i++) {
                std::vector<Value> callback_args = {
                    ta->get_element(i),
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                callback->call(ctx, callback_args, thisArg);
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
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            TypedArrayBase* result = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8:
                    result = TypedArrayFactory::create_int8_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8:
                    result = TypedArrayFactory::create_uint8_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED:
                    result = TypedArrayFactory::create_uint8_clamped_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::INT16:
                    result = TypedArrayFactory::create_int16_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT16:
                    result = TypedArrayFactory::create_uint16_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::INT32:
                    result = TypedArrayFactory::create_int32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT32:
                    result = TypedArrayFactory::create_uint32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT32:
                    result = TypedArrayFactory::create_float32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT64:
                    result = TypedArrayFactory::create_float64_array(length).release();
                    break;
                default:
                    ctx.throw_type_error("Unsupported TypedArray type");
                    return Value();
            }

            for (size_t i = 0; i < length; i++) {
                std::vector<Value> callback_args = {
                    ta->get_element(i),
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                Value mapped = callback->call(ctx, callback_args, thisArg);
                result->set_element(i, mapped);
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
                if (result.to_boolean()) {
                    filtered.push_back(element);
                }
            }

            TypedArrayBase* result = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8:
                    result = TypedArrayFactory::create_int8_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8:
                    result = TypedArrayFactory::create_uint8_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED:
                    result = TypedArrayFactory::create_uint8_clamped_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::INT16:
                    result = TypedArrayFactory::create_int16_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT16:
                    result = TypedArrayFactory::create_uint16_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::INT32:
                    result = TypedArrayFactory::create_int32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT32:
                    result = TypedArrayFactory::create_uint32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT32:
                    result = TypedArrayFactory::create_float32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT64:
                    result = TypedArrayFactory::create_float64_array(filtered.size()).release();
                    break;
                default:
                    ctx.throw_type_error("Unsupported TypedArray type");
                    return Value();
            }
            for (size_t i = 0; i < filtered.size(); i++) {
                result->set_element(i, filtered[i]);
            }
            return Value(result);
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("filter", filter_desc);

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (!cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("every", PropertyDescriptor(Value(every_fn.release()), PropertyAttributes::BuiltinFunction));

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(true);
            }
            return Value(false);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("some", PropertyDescriptor(Value(some_fn.release()), PropertyAttributes::BuiltinFunction));

    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                Value el = ta->get_element(i);
                std::vector<Value> cb_args = { el, Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return el;
            }
            return Value();
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("find", PropertyDescriptor(Value(find_fn.release()), PropertyAttributes::BuiltinFunction));

    auto findIndex_fn = ObjectFactory::create_native_function("findIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("findIndex", PropertyDescriptor(Value(findIndex_fn.release()), PropertyAttributes::BuiltinFunction));

    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (args.empty() || ta->length() == 0) return Value(-1.0);
            Value search = args[0];
            size_t from = 0;
            if (args.size() > 1) { int64_t idx = static_cast<int64_t>(args[1].to_number()); from = idx < 0 ? (size_t)(idx + (int64_t)ta->length() < 0 ? 0 : idx + (int64_t)ta->length()) : (size_t)idx; }
            for (size_t i = from; i < ta->length(); i++) {
                if (ta->get_element(i).strict_equals(search)) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("indexOf", PropertyDescriptor(Value(indexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (args.empty() || ta->length() == 0) return Value(-1.0);
            Value search = args[0];
            int64_t from = ta->length() - 1;
            if (args.size() > 1) { int64_t idx = static_cast<int64_t>(args[1].to_number()); from = idx < 0 ? idx + (int64_t)ta->length() : (idx < (int64_t)ta->length() ? idx : (int64_t)ta->length() - 1); }
            for (int64_t i = from; i >= 0; i--) {
                if (ta->get_element(static_cast<size_t>(i)).strict_equals(search)) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("lastIndexOf", PropertyDescriptor(Value(lastIndexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            size_t k = 0; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else { if (ta->length() == 0) { ctx.throw_type_error("Reduce of empty array"); return Value(); } acc = ta->get_element(static_cast<size_t>(0)); k = 1; }
            for (; k < ta->length(); k++) {
                std::vector<Value> cb_args = { acc, ta->get_element(k), Value(static_cast<double>(k)), Value(this_obj) };
                acc = cb->call(ctx, cb_args, Value());
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduce", PropertyDescriptor(Value(reduce_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            int64_t k = (int64_t)ta->length() - 1; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else { if (ta->length() == 0) { ctx.throw_type_error("Reduce of empty array"); return Value(); } acc = ta->get_element(ta->length() - 1); k--; }
            for (; k >= 0; k--) {
                std::vector<Value> cb_args = { acc, ta->get_element(static_cast<size_t>(k)), Value(static_cast<double>(k)), Value(this_obj) };
                acc = cb->call(ctx, cb_args, Value());
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduceRight", PropertyDescriptor(Value(reduceRight_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_join_fn = ObjectFactory::create_native_function("join",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            std::string sep = (args.empty() || args[0].is_undefined()) ? "," : args[0].to_string();
            std::string result;
            for (size_t i = 0; i < ta->length(); i++) { if (i > 0) result += sep; result += ta->get_element(i).to_string(); }
            return Value(result);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("join", PropertyDescriptor(Value(ta_join_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            size_t len = ta->length();
            if (len <= 1) return Value(this_obj);
            Function* cmp = (!args.empty() && args[0].is_function()) ? args[0].as_function() : nullptr;
            std::vector<Value> els; els.reserve(len);
            for (size_t i = 0; i < len; i++) els.push_back(ta->get_element(i));
            std::sort(els.begin(), els.end(), [&](const Value& a, const Value& b) {
                if (cmp) { std::vector<Value> ca = {a, b}; return cmp->call(ctx, ca, Value()).to_number() < 0; }
                return a.to_number() < b.to_number();
            });
            for (size_t i = 0; i < len; i++) ta->set_element(i, els[i]);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("sort", PropertyDescriptor(Value(ta_sort_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            size_t len = ta->length();
            for (size_t i = 0; i < len / 2; i++) { Value t = ta->get_element(i); ta->set_element(i, ta->get_element(len - 1 - i)); ta->set_element(len - 1 - i, t); }
            return Value(this_obj);
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("reverse", PropertyDescriptor(Value(ta_reverse_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length(), start = 0, end = len;
            if (!args.empty()) { int64_t s = static_cast<int64_t>(args[0].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t e = static_cast<int64_t>(args[1].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            size_t sl = end > start ? end - start : 0;
            TypedArrayBase* r = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8: r = TypedArrayFactory::create_int8_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT8: r = TypedArrayFactory::create_uint8_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED: r = TypedArrayFactory::create_uint8_clamped_array(sl).release(); break;
                case TypedArrayBase::ArrayType::INT16: r = TypedArrayFactory::create_int16_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT16: r = TypedArrayFactory::create_uint16_array(sl).release(); break;
                case TypedArrayBase::ArrayType::INT32: r = TypedArrayFactory::create_int32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT32: r = TypedArrayFactory::create_uint32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::FLOAT32: r = TypedArrayFactory::create_float32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::FLOAT64: r = TypedArrayFactory::create_float64_array(sl).release(); break;
                default: ctx.throw_type_error("Unsupported type"); return Value();
            }
            for (size_t i = 0; i < sl; i++) r->set_element(i, ta->get_element(start + i));
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("slice", PropertyDescriptor(Value(ta_slice_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length();
            Value fillVal = args.empty() ? Value() : args[0];
            int64_t start = 0, end = len;
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t s = static_cast<int64_t>(args[1].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 2 && !args[2].is_undefined()) { int64_t e = static_cast<int64_t>(args[2].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            for (int64_t i = start; i < end; i++) ta->set_element(static_cast<size_t>(i), fillVal);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("fill", PropertyDescriptor(Value(ta_fill_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length();
            if (args.empty()) return Value(this_obj);
            int64_t tgt = static_cast<int64_t>(args[0].to_number());
            tgt = tgt < 0 ? (tgt + len < 0 ? 0 : tgt + len) : (tgt > len ? len : tgt);
            int64_t start = 0;
            if (args.size() > 1) { int64_t s = static_cast<int64_t>(args[1].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            int64_t end = len;
            if (args.size() > 2 && !args[2].is_undefined()) { int64_t e = static_cast<int64_t>(args[2].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            int64_t count = end - start;
            if (count <= 0) return Value(this_obj);
            std::vector<Value> tmp; tmp.reserve(count);
            for (int64_t i = 0; i < count; i++) tmp.push_back(ta->get_element(static_cast<size_t>(start + i)));
            for (int64_t i = 0; i < count && tgt + i < len; i++) ta->set_element(static_cast<size_t>(tgt + i), tmp[static_cast<size_t>(i)]);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("copyWithin", PropertyDescriptor(Value(ta_copyWithin_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length()))); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { auto pair = ObjectFactory::create_object(); pair->set_property("0", Value((double)idx)); pair->set_property("1", static_cast<TypedArrayBase*>(it->get_property("__arr").as_object())->get_element(idx)); pair->set_property("length", Value(2.0));
                    res->set_property("done", Value(false)); res->set_property("value", Value(pair.release())); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("entries", PropertyDescriptor(Value(ta_entries_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length())));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", Value((double)idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("keys", PropertyDescriptor(Value(ta_keys_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length()))); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", static_cast<TypedArrayBase*>(it->get_property("__arr").as_object())->get_element(idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("values", PropertyDescriptor(Value(ta_values_fn.release()), PropertyAttributes::BuiltinFunction));

    // ES6: TypedArray.prototype[Symbol.iterator] = values
    auto ta_sym_iterator_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            auto idx = std::make_shared<uint32_t>(0);
            uint32_t len = ta->byte_length() / ta->bytes_per_element();
            auto next = ObjectFactory::create_native_function("next",
                [this_obj, idx, len](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    auto res = ObjectFactory::create_object();
                    if (*idx >= len) {
                        res->set_property("done", Value(true));
                        res->set_property("value", Value());
                    } else {
                        res->set_property("done", Value(false));
                        res->set_property("value", this_obj->get_element(*idx));
                        (*idx)++;
                    }
                    return Value(res.release());
                }, 0);
            iter->set_property("next", Value(next.release()));
            return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property("Symbol.iterator", Value(ta_sym_iterator_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ta_subarray_fn = ObjectFactory::create_native_function("subarray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length(), start = 0, end = len;
            if (!args.empty()) { int64_t s = static_cast<int64_t>(args[0].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t e = static_cast<int64_t>(args[1].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            size_t nl = end > start ? end - start : 0;
            size_t nbo = ta->byte_offset() + start * ta->bytes_per_element();
            std::shared_ptr<ArrayBuffer> sb(ta->buffer(), [](ArrayBuffer*) {});
            TypedArrayBase* r = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8: r = new Int8Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT8: r = new Uint8Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED: r = new Uint8ClampedArray(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::INT16: r = new Int16Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT16: r = new Uint16Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::INT32: r = new Int32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT32: r = new Uint32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::FLOAT32: r = new Float32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::FLOAT64: r = new Float64Array(sb, nbo, nl); break;
                default: ctx.throw_type_error("Unsupported type"); return Value();
            }
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("subarray", PropertyDescriptor(Value(ta_subarray_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.includes called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            uint32_t length = static_cast<uint32_t>(ta->length());
            Value search_element = args.empty() ? Value() : args[0];
            int64_t from_index = 0;
            if (args.size() > 1) {
                from_index = static_cast<int64_t>(args[1].to_number());
            }
            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }
            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; i++) {
                Value element = ta->get_element(i);
                if (search_element.is_number() && element.is_number()) {
                    double sn = search_element.to_number(), en = element.to_number();
                    if (std::isnan(sn) && std::isnan(en)) return Value(true);
                    if (sn == en) return Value(true);
                } else if (element.strict_equals(search_element)) {
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
            ctx.throw_type_error("TypedArray.from must be called on a concrete TypedArray constructor");
            return Value();
        }, 1);
    PropertyDescriptor from_desc(Value(typedarray_from.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("from", from_desc);

    auto typedarray_of = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            ctx.throw_type_error("TypedArray.of must be called on a concrete TypedArray constructor");
            return Value();
        }, 0);
    PropertyDescriptor of_desc(Value(typedarray_of.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("of", of_desc);

    ctx.register_built_in_object("TypedArray", typedarray_constructor.release());

    auto int8array_constructor = ObjectFactory::create_native_constructor("Int8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int8_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int8_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int8Array>(shared_buffer).release());
                }
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int8_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int8_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int8Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Int8Array", int8array_constructor.release());

    auto uint16array_constructor = ObjectFactory::create_native_constructor("Uint16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint16_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint16_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Uint16Array>(shared_buffer).release());
                }
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_uint16_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint16_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Uint16Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Uint16Array", uint16array_constructor.release());

    auto int16array_constructor = ObjectFactory::create_native_constructor("Int16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int16_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int16_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int16Array>(shared_buffer).release());
                }
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int16_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int16_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int16Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Int16Array", int16array_constructor.release());

    auto uint32array_constructor = ObjectFactory::create_native_constructor("Uint32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint32_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint32_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Uint32Array>(shared_buffer).release());
                }
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_uint32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Uint32Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Uint32Array", uint32array_constructor.release());

    auto int32array_constructor = ObjectFactory::create_native_constructor("Int32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int32_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int32_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int32Array>(shared_buffer).release());
                }
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int32Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Int32Array", int32array_constructor.release());

    auto float64array_constructor = ObjectFactory::create_native_constructor("Float64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) {
                return Value(TypedArrayFactory::create_float64_array(0).release());
            }

            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_float64_array(length).release());
            }

            if (args[0].is_object()) {
                Object* obj = args[0].as_object();

                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Float64Array>(shared_buffer).release());
                }

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());
                    auto typed_array = TypedArrayFactory::create_float64_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_float64_array(length);
                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
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
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_float64_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Float64Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("Float64Array", float64array_constructor.release());

    auto bigint64array_constructor = ObjectFactory::create_native_constructor("BigInt64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) return Value(new BigInt64Array(0));
            if (args[0].is_number()) return Value(new BigInt64Array(static_cast<size_t>(args[0].as_number())));
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* ab = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> sb(ab, [](ArrayBuffer*){});
                    return Value(new BigInt64Array(sb));
                }
            }
            ctx.throw_type_error("BigInt64Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("BigInt64Array", bigint64array_constructor.release());

    auto biguint64array_constructor = ObjectFactory::create_native_constructor("BigUint64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) return Value(new BigUint64Array(0));
            if (args[0].is_number()) return Value(new BigUint64Array(static_cast<size_t>(args[0].as_number())));
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* ab = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> sb(ab, [](ArrayBuffer*){});
                    return Value(new BigUint64Array(sb));
                }
            }
            ctx.throw_type_error("BigUint64Array constructor argument not supported");
            return Value();
        });
    ctx.register_built_in_object("BigUint64Array", biguint64array_constructor.release());

    // Set up prototype chains: XArray.prototype.__proto__ = TypedArray.prototype
    // Also: XArray.__proto__ = TypedArray (constructor chain)
    Object* typedarray_ctor_ptr = ctx.get_built_in_object("TypedArray");
    struct TypedInfo { const char* name; int bytes; };
    TypedInfo typed_infos[] = {
        {"Int8Array", 1}, {"Uint8Array", 1}, {"Uint8ClampedArray", 1},
        {"Int16Array", 2}, {"Uint16Array", 2}, {"Int32Array", 4}, {"Uint32Array", 4},
        {"Float32Array", 4}, {"Float64Array", 8}
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
        }
    }

    auto dataview_constructor = ObjectFactory::create_native_constructor("DataView",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            Value result = DataView::constructor(ctx, args);
            
            if (result.is_object()) {
                Object* dataview_obj = result.as_object();
                
                auto get_uint8_method = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8);
                dataview_obj->set_property("getUint8", Value(get_uint8_method.release()));
                
                auto set_uint8_method = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8);
                dataview_obj->set_property("setUint8", Value(set_uint8_method.release()));

                auto get_int8_method = ObjectFactory::create_native_function("getInt8", DataView::js_get_int8);
                dataview_obj->set_property("getInt8", Value(get_int8_method.release()));

                auto set_int8_method = ObjectFactory::create_native_function("setInt8", DataView::js_set_int8);
                dataview_obj->set_property("setInt8", Value(set_int8_method.release()));

                auto get_int16_method = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16);
                dataview_obj->set_property("getInt16", Value(get_int16_method.release()));
                
                auto set_int16_method = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16);
                dataview_obj->set_property("setInt16", Value(set_int16_method.release()));

                auto get_uint16_method = ObjectFactory::create_native_function("getUint16", DataView::js_get_uint16);
                dataview_obj->set_property("getUint16", Value(get_uint16_method.release()));

                auto set_uint16_method = ObjectFactory::create_native_function("setUint16", DataView::js_set_uint16);
                dataview_obj->set_property("setUint16", Value(set_uint16_method.release()));

                auto get_int32_method = ObjectFactory::create_native_function("getInt32", DataView::js_get_int32);
                dataview_obj->set_property("getInt32", Value(get_int32_method.release()));

                auto set_int32_method = ObjectFactory::create_native_function("setInt32", DataView::js_set_int32);
                dataview_obj->set_property("setInt32", Value(set_int32_method.release()));

                auto get_uint32_method = ObjectFactory::create_native_function("getUint32", DataView::js_get_uint32);
                dataview_obj->set_property("getUint32", Value(get_uint32_method.release()));
                
                auto set_uint32_method = ObjectFactory::create_native_function("setUint32", DataView::js_set_uint32);
                dataview_obj->set_property("setUint32", Value(set_uint32_method.release()));
                
                auto get_float32_method = ObjectFactory::create_native_function("getFloat32", DataView::js_get_float32);
                dataview_obj->set_property("getFloat32", Value(get_float32_method.release()));
                
                auto set_float32_method = ObjectFactory::create_native_function("setFloat32", DataView::js_set_float32);
                dataview_obj->set_property("setFloat32", Value(set_float32_method.release()));
                
                auto get_float64_method = ObjectFactory::create_native_function("getFloat64", DataView::js_get_float64);
                dataview_obj->set_property("getFloat64", Value(get_float64_method.release()));

                auto set_float64_method = ObjectFactory::create_native_function("setFloat64", DataView::js_set_float64);
                dataview_obj->set_property("setFloat64", Value(set_float64_method.release()));

                auto get_bigint64_method = ObjectFactory::create_native_function("getBigInt64", DataView::js_get_bigint64);
                dataview_obj->set_property("getBigInt64", Value(get_bigint64_method.release()));

                auto set_bigint64_method = ObjectFactory::create_native_function("setBigInt64", DataView::js_set_bigint64);
                dataview_obj->set_property("setBigInt64", Value(set_bigint64_method.release()));

                auto get_biguint64_method = ObjectFactory::create_native_function("getBigUint64", DataView::js_get_biguint64);
                dataview_obj->set_property("getBigUint64", Value(get_biguint64_method.release()));

                auto set_biguint64_method = ObjectFactory::create_native_function("setBigUint64", DataView::js_set_biguint64);
                dataview_obj->set_property("setBigUint64", Value(set_biguint64_method.release()));
            }

            return result;
        });

    auto dataview_prototype = ObjectFactory::create_object();

    auto get_uint8_proto = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8);
    dataview_prototype->set_property("getUint8", Value(get_uint8_proto.release()));

    auto set_uint8_proto = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8);
    dataview_prototype->set_property("setUint8", Value(set_uint8_proto.release()));

    auto get_int8_proto = ObjectFactory::create_native_function("getInt8", DataView::js_get_int8);
    dataview_prototype->set_property("getInt8", Value(get_int8_proto.release()));

    auto set_int8_proto = ObjectFactory::create_native_function("setInt8", DataView::js_set_int8);
    dataview_prototype->set_property("setInt8", Value(set_int8_proto.release()));

    auto get_int16_proto = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16);
    dataview_prototype->set_property("getInt16", Value(get_int16_proto.release()));

    auto set_int16_proto = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16);
    dataview_prototype->set_property("setInt16", Value(set_int16_proto.release()));

    auto get_uint16_proto = ObjectFactory::create_native_function("getUint16", DataView::js_get_uint16);
    dataview_prototype->set_property("getUint16", Value(get_uint16_proto.release()));

    auto set_uint16_proto = ObjectFactory::create_native_function("setUint16", DataView::js_set_uint16);
    dataview_prototype->set_property("setUint16", Value(set_uint16_proto.release()));

    auto get_int32_proto = ObjectFactory::create_native_function("getInt32", DataView::js_get_int32);
    dataview_prototype->set_property("getInt32", Value(get_int32_proto.release()));

    auto set_int32_proto = ObjectFactory::create_native_function("setInt32", DataView::js_set_int32);
    dataview_prototype->set_property("setInt32", Value(set_int32_proto.release()));

    auto get_uint32_proto = ObjectFactory::create_native_function("getUint32", DataView::js_get_uint32);
    dataview_prototype->set_property("getUint32", Value(get_uint32_proto.release()));

    auto set_uint32_proto = ObjectFactory::create_native_function("setUint32", DataView::js_set_uint32);
    dataview_prototype->set_property("setUint32", Value(set_uint32_proto.release()));

    auto get_float32_proto = ObjectFactory::create_native_function("getFloat32", DataView::js_get_float32);
    dataview_prototype->set_property("getFloat32", Value(get_float32_proto.release()));

    auto set_float32_proto = ObjectFactory::create_native_function("setFloat32", DataView::js_set_float32);
    dataview_prototype->set_property("setFloat32", Value(set_float32_proto.release()));

    auto get_float64_proto = ObjectFactory::create_native_function("getFloat64", DataView::js_get_float64);
    dataview_prototype->set_property("getFloat64", Value(get_float64_proto.release()));

    auto set_float64_proto = ObjectFactory::create_native_function("setFloat64", DataView::js_set_float64);
    dataview_prototype->set_property("setFloat64", Value(set_float64_proto.release()));

    auto get_bigint64_proto = ObjectFactory::create_native_function("getBigInt64", DataView::js_get_bigint64);
    dataview_prototype->set_property("getBigInt64", Value(get_bigint64_proto.release()));

    auto set_bigint64_proto = ObjectFactory::create_native_function("setBigInt64", DataView::js_set_bigint64);
    dataview_prototype->set_property("setBigInt64", Value(set_bigint64_proto.release()));

    auto get_biguint64_proto = ObjectFactory::create_native_function("getBigUint64", DataView::js_get_biguint64);
    dataview_prototype->set_property("getBigUint64", Value(get_biguint64_proto.release()));

    auto set_biguint64_proto = ObjectFactory::create_native_function("setBigUint64", DataView::js_set_biguint64);
    dataview_prototype->set_property("setBigUint64", Value(set_biguint64_proto.release()));

    PropertyDescriptor dataview_tag_desc(Value(std::string("DataView")), PropertyAttributes::Configurable);
    dataview_prototype->set_property_descriptor("Symbol.toStringTag", dataview_tag_desc);

    dataview_constructor->set_property("prototype", Value(dataview_prototype.release()));

    ctx.register_built_in_object("DataView", dataview_constructor.release());

    auto done_function = ObjectFactory::create_native_function("$DONE",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!args.empty() && !args[0].is_undefined()) {
                std::string error_msg = args[0].to_string();
                ctx.throw_exception(Value("Test failed: " + error_msg));
            }
            return Value();
        });
    ctx.get_global_object()->set_property("$DONE", Value(done_function.release()));


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
}

} // namespace Quanta
