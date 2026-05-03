/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/ArrayBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/Parser.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include <cmath>
#include <functional>
#include <sstream>
#include "quanta/parser/AST.h"

namespace Quanta {

static Value array_species_create(Context& ctx, Object* original_array, uint32_t length) {
    bool is_actual_array = original_array->is_array();
    if (!is_actual_array && original_array->get_type() == Object::ObjectType::Proxy) {
        Object* target = static_cast<Proxy*>(original_array)->get_proxy_target();
        is_actual_array = target && target->is_array();
    }
    if (!is_actual_array) {
        return Value(ObjectFactory::create_array(length).release());
    }
    Value ctor_val = original_array->get_property("constructor");
    if (!ctor_val.is_undefined() && !ctor_val.is_null() &&
        (ctor_val.is_function() || ctor_val.is_object())) {
        Object* ctor = ctor_val.is_function()
            ? static_cast<Object*>(ctor_val.as_function())
            : ctor_val.as_object();
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            Value species_val = ctor->get_property(species_sym->to_property_key());
            if (species_val.is_null() || species_val.is_undefined()) {
                // null/undefined species -> fallback to plain Array
            } else if (species_val.is_function()) {
                Function* species_fn = species_val.as_function();
                Value result = species_fn->construct(ctx, {Value(static_cast<double>(length))});
                if (ctx.has_exception()) return Value();
                return result;
            }
        }
    }
    return Value(ObjectFactory::create_array(length).release());
}

void register_array_builtins(Context& ctx, Object* function_prototype) {
    auto array_constructor = ObjectFactory::create_native_constructor("Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::unique_ptr<Object> array;
            if (args.empty()) {
                array = ObjectFactory::create_array();
            } else if (args.size() == 1 && args[0].is_number()) {
                double length_val = args[0].to_number();
                if (length_val < 0 || length_val >= 4294967296.0 || length_val != std::floor(length_val)) {
                    ctx.throw_range_error("Invalid array length");
                    return Value();
                }
                array = ObjectFactory::create_array();
                array->set_property("length", Value(length_val));
            } else {
                array = ObjectFactory::create_array();
                for (size_t i = 0; i < args.size(); i++) {
                    array->set_element(static_cast<uint32_t>(i), args[i]);
                }
                array->set_property("length", Value(static_cast<double>(args.size())));
            }
            // ES6: subclassing - use new.target.prototype if different from Array.prototype
            Value new_target = ctx.get_new_target();
            if (new_target.is_function()) {
                Value nt_proto = new_target.as_function()->get_property("prototype");
                if (nt_proto.is_object()) {
                    array->set_prototype(nt_proto.as_object());
                }
            } else if (new_target.is_object()) {
                Value nt_proto = new_target.as_object()->get_property("prototype");
                if (nt_proto.is_object()) {
                    array->set_prototype(nt_proto.as_object());
                }
            }
            return Value(array.release());
        }, 1);

    auto isArray_fn = ObjectFactory::create_native_function("isArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(false);
            const Value& arg = args[0];
            if (!arg.is_object()) return Value(false);
            Object* obj = arg.as_object();
            while (obj && obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy = static_cast<Proxy*>(obj);
                if (proxy->is_revoked()) return Value(false);
                Object* target = proxy->get_proxy_target();
                if (!target) return Value(false);
                obj = target;
            }
            return Value(obj && obj->is_array());
        }, 1);
    Function* isArray_ptr = isArray_fn.release();
    PropertyAttributes isArray_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("isArray", Value(isArray_ptr), isArray_attrs);

    auto from_fn = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(ObjectFactory::create_array().release());

            Value arrayLike = args[0];
            Function* mapfn = (args.size() > 1 && args[1].is_function()) ? args[1].as_function() : nullptr;
            Value thisArg = (args.size() > 2) ? args[2] : Value();

            Object* this_binding = ctx.get_this_binding();
            Function* constructor = nullptr;
            if (this_binding && this_binding->is_function()) {
                constructor = static_cast<Function*>(this_binding);
            }

            // ES6: Check for Symbol.iterator first (iterable protocol)
            if (arrayLike.is_object() || arrayLike.is_function()) {
                Object* src_obj = arrayLike.is_function()
                    ? static_cast<Object*>(arrayLike.as_function())
                    : arrayLike.as_object();
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = src_obj->get_property(iter_sym->to_property_key());
                    if (iter_method.is_function()) {
                        Value iterator_obj = iter_method.as_function()->call(ctx, {}, arrayLike);
                        if (ctx.has_exception()) return Value();
                        if (iterator_obj.is_object()) {
                            Object* iterator = iterator_obj.as_object();
                            Value next_method = iterator->get_property("next");
                            if (next_method.is_function()) {
                                auto close_iter = [&iterator_obj, &ctx]() {
                                    if (iterator_obj.is_object()) {
                                        Value rm = iterator_obj.as_object()->get_property("return");
                                        if (rm.is_function()) rm.as_function()->call(ctx, {}, iterator_obj);
                                    }
                                };
                                auto result_arr = ObjectFactory::create_array(0);
                                uint32_t idx = 0;
                                for (uint32_t ii = 0; ii < 100000; ii++) {
                                    Value res = next_method.as_function()->call(ctx, {}, iterator_obj);
                                    if (ctx.has_exception()) return Value();
                                    if (!res.is_object()) break;
                                    Value done = res.as_object()->get_property("done");
                                    if (done.to_boolean()) break;
                                    Value val = res.as_object()->get_property("value");
                                    if (mapfn) {
                                        std::vector<Value> margs = { val, Value(static_cast<double>(idx)) };
                                        val = mapfn->call(ctx, margs, thisArg);
                                        if (ctx.has_exception()) { close_iter(); return Value(); }
                                    }
                                    result_arr->set_element(idx++, val);
                                }
                                result_arr->set_property("length", Value(static_cast<double>(idx)));
                                return Value(result_arr.release());
                            }
                        }
                    }
                }
            }

            // Fallback: array-like (has .length)
            uint32_t length = 0;
            if (arrayLike.is_string()) {
                length = static_cast<uint32_t>(arrayLike.to_string().length());
            } else if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                Value lengthValue = obj->get_property("length");
                length = lengthValue.is_number() ? static_cast<uint32_t>(lengthValue.to_number()) : 0;
            }

            Object* result = nullptr;
            if (constructor) {
                std::vector<Value> constructor_args = { Value(static_cast<double>(length)) };
                Value constructed = constructor->construct(ctx, constructor_args);
                if (constructed.is_object()) {
                    result = constructed.as_object();
                } else {
                    result = ObjectFactory::create_array().release();
                }
            } else {
                result = ObjectFactory::create_array().release();
            }

            if (arrayLike.is_string()) {
                std::string str = arrayLike.to_string();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = Value(std::string(1, str[i]));
                    if (mapfn) {
                        std::vector<Value> mapfn_args = { element, Value(static_cast<double>(i)) };
                        element = mapfn->call(ctx, mapfn_args, thisArg);
                    }
                    result->set_element(i, element);
                }
            } else if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = obj->get_element(i);
                    if (mapfn) {
                        std::vector<Value> mapfn_args = { element, Value(static_cast<double>(i)) };
                        element = mapfn->call(ctx, mapfn_args, thisArg);
                    }
                    result->set_element(i, element);
                }
            }

            result->set_property("length", Value(static_cast<double>(length)));
            return Value(result);
        }, 1);
    Function* from_ptr = from_fn.release();
    PropertyAttributes from_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("from", Value(from_ptr), from_attrs);

    auto of_fn = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_binding = ctx.get_this_binding();
            Function* constructor = nullptr;
            if (this_binding && this_binding->is_function()) {
                constructor = static_cast<Function*>(this_binding);
            }

            Object* result = nullptr;
            if (constructor) {
                std::vector<Value> constructor_args = { Value(static_cast<double>(args.size())) };
                Value constructed = constructor->construct(ctx, constructor_args);
                if (constructed.is_object()) {
                    result = constructed.as_object();
                } else {
                    result = ObjectFactory::create_array().release();
                }
            } else {
                result = ObjectFactory::create_array().release();
            }

            for (size_t i = 0; i < args.size(); i++) {
                result->set_element(static_cast<uint32_t>(i), args[i]);
            }
            result->set_property("length", Value(static_cast<double>(args.size())));
            return Value(result);
        }, 0);
    Function* of_ptr = of_fn.release();
    PropertyAttributes of_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("of", Value(of_ptr), of_attrs);

    auto fromAsync_fn = ObjectFactory::create_native_function("fromAsync",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ObjectFactory::create_array().release());
        });

    PropertyDescriptor fromAsync_length_desc(Value(1.0), PropertyAttributes::None);
    fromAsync_length_desc.set_configurable(true);
    fromAsync_length_desc.set_enumerable(false);
    fromAsync_length_desc.set_writable(false);
    fromAsync_fn->set_property_descriptor("length", fromAsync_length_desc);

    array_constructor->set_property("fromAsync", Value(fromAsync_fn.release()), PropertyAttributes::BuiltinFunction);

    auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_binding = ctx.get_this_binding();
            if (this_binding) {
                return Value(this_binding);
            }
            return Value();
        }, 0);

    PropertyDescriptor species_desc;
    species_desc.set_getter(species_getter.release());
    species_desc.set_enumerable(false);
    species_desc.set_configurable(true);
    array_constructor->set_property_descriptor("Symbol.species", species_desc);

    auto array_prototype = ObjectFactory::create_array();

    array_prototype->set_prototype(function_prototype);

    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.find callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return element;
                }
            }

            return Value();
        });

    PropertyDescriptor find_length_desc(Value(1.0), PropertyAttributes::Configurable);
    find_length_desc.set_enumerable(false);
    find_length_desc.set_writable(false);
    find_fn->set_property_descriptor("length", find_length_desc);

    find_fn->set_property("name", Value(std::string("find")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor find_desc(Value(find_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("find", find_desc);

    auto findLast_fn = ObjectFactory::create_native_function("findLast",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast called on non-object")));
                return Value();
            }

            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast requires a callback function")));
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast callback must be a function")));
                return Value();
            }

            Function* callback_fn = callback.as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback_fn->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return element;
                }
            }
            return Value();
        }, 1);

    findLast_fn->set_property("name", Value(std::string("findLast")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor findLast_desc(Value(findLast_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findLast", findLast_desc);

    auto findLastIndex_fn = ObjectFactory::create_native_function("findLastIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex called on non-object")));
                return Value();
            }

            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex requires a callback function")));
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex callback must be a function")));
                return Value();
            }

            Function* callback_fn = callback.as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback_fn->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return Value(static_cast<double>(i));
                }
            }
            return Value(-1.0);
        }, 1);

    findLastIndex_fn->set_property("name", Value(std::string("findLastIndex")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor findLastIndex_desc(Value(findLastIndex_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findLastIndex", findLastIndex_desc);

    auto with_fn = ObjectFactory::create_native_function("with",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.with called on non-object")));
                return Value();
            }

            uint32_t length = this_obj->get_length();

            if (args.empty()) {
                throw std::runtime_error("TypeError: Array.prototype.with requires an index argument");
            }

            double index_arg = args[0].to_number();
            int32_t actual_index;

            if (index_arg < 0) {
                actual_index = static_cast<int32_t>(length) + static_cast<int32_t>(index_arg);
            } else {
                actual_index = static_cast<int32_t>(index_arg);
            }

            if (actual_index < 0 || actual_index >= static_cast<int32_t>(length)) {
                throw std::runtime_error("RangeError: Array.prototype.with index out of bounds");
            }

            Value new_value = args.size() > 1 ? args[1] : Value();

            auto result = ObjectFactory::create_array();
            for (uint32_t i = 0; i < length; i++) {
                if (i == static_cast<uint32_t>(actual_index)) {
                    result->set_element(i, new_value);
                } else {
                    result->set_element(i, this_obj->get_element(i));
                }
            }
            result->set_length(length);

            return Value(result.release());
        }, 2);

    with_fn->set_property("name", Value(std::string("with")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor with_desc(Value(with_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("with", with_desc);

    auto at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.at called on non-object")));
                return Value();
            }

            if (args.empty()) {
                return Value();
            }

            int32_t index = static_cast<int32_t>(args[0].to_number());
            uint32_t length = this_obj->get_length();

            if (index < 0) {
                index = static_cast<int32_t>(length) + index;
            }

            if (index < 0 || index >= static_cast<int32_t>(length)) {
                return Value();
            }

            return this_obj->get_element(static_cast<uint32_t>(index));
        }, 1);

    at_fn->set_property("name", Value(std::string("at")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor at_desc(Value(at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("at", at_desc);


    auto includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.includes called on non-object")));
                return Value();
            }

            Value search_element = args.empty() ? Value() : args[0];

            Value length_val = this_obj->get_property("length");
            uint32_t length = static_cast<uint32_t>(length_val.to_number());

            int64_t from_index = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }
                from_index = static_cast<int64_t>(args[1].to_number());
            }

            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }

            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; i++) {
                Value element = this_obj->get_property(std::to_string(i));

                if (search_element.is_number() && element.is_number()) {
                    double search_num = search_element.to_number();
                    double element_num = element.to_number();

                    if (std::isnan(search_num) && std::isnan(element_num)) {
                        return Value(true);
                    }

                    if (search_num == element_num) {
                        return Value(true);
                    }
                } else if (element.strict_equals(search_element)) {
                    return Value(true);
                }
            }

            return Value(false);
        }, 1);


    includes_fn->set_property("name", Value(std::string("includes")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor array_includes_desc(Value(includes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("includes", array_includes_desc);

    auto flat_fn = ObjectFactory::create_native_function("flat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            double depth = 1.0;
            if (!args.empty() && !args[0].is_undefined()) {
                depth = args[0].to_number();
                if (std::isnan(depth) || depth < 0) {
                    depth = 0.0;
                }
            }

            std::function<void(Object*, std::unique_ptr<Object>&, double)> flatten_helper;
            flatten_helper = [&](Object* source, std::unique_ptr<Object>& target, double current_depth) {
                uint32_t source_length = source->get_length();
                uint32_t target_length = target->get_length();

                for (uint32_t i = 0; i < source_length; i++) {
                    Value element = source->get_element(i);

                    if (element.is_object() && current_depth > 0) {
                        Object* element_obj = element.as_object();
                        if (element_obj->has_property("length")) {
                            flatten_helper(element_obj, target, current_depth - 1);
                            target_length = target->get_length();
                            continue;
                        }
                    }

                    target->set_element(target_length++, element);
                }

                target->set_length(target_length);
            };

            auto result = ObjectFactory::create_array();
            flatten_helper(this_obj, result, depth);

            return Value(result.release());
        });

    PropertyDescriptor flat_length_desc(Value(0.0), PropertyAttributes::Configurable);
    flat_fn->set_property_descriptor("length", flat_length_desc);

    flat_fn->set_property("name", Value(std::string("flat")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor flat_desc(Value(flat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flat", flat_desc);

    auto flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.flatMap callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value mapped = callback->call(ctx, callback_args, thisArg);

                if (mapped.is_object()) {
                    Object* mapped_obj = mapped.as_object();
                    if (mapped_obj->has_property("length")) {
                        uint32_t mapped_length = mapped_obj->get_length();
                        for (uint32_t j = 0; j < mapped_length; j++) {
                            result->set_element(result_index++, mapped_obj->get_element(j));
                        }
                        continue;
                    }
                }

                result->set_element(result_index++, mapped);
            }

            result->set_length(result_index);
            return Value(result.release());
        }, 1);

    flatMap_fn->set_property("name", Value(std::string("flatMap")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor flatMap_desc(Value(flatMap_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flatMap", flatMap_desc);

    auto fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value fill_value = args.empty() ? Value() : args[0];

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;

                double start_arg = args.size() > 1 ? args[1].to_number() : 0.0;
                int32_t start = start_arg < 0
                    ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg))
                    : std::min(static_cast<uint32_t>(start_arg), length);

                double end_arg = args.size() > 2 && !args[2].is_undefined() ? args[2].to_number() : static_cast<double>(length);
                int32_t end = end_arg < 0
                    ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(end_arg))
                    : std::min(static_cast<uint32_t>(end_arg), length);

                for (int32_t i = start; i < end; i++) {
                    this_obj->set_property(std::to_string(i), fill_value);
                }
                return Value(this_obj);
            }

            uint32_t length = this_obj->get_length();

            double start_arg = args.size() > 1 ? args[1].to_number() : 0.0;
            int32_t start = start_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg))
                : std::min(static_cast<uint32_t>(start_arg), length);

            double end_arg = args.size() > 2 && !args[2].is_undefined() ? args[2].to_number() : static_cast<double>(length);
            int32_t end = end_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(end_arg))
                : std::min(static_cast<uint32_t>(end_arg), length);

            for (int32_t i = start; i < end; i++) {
                this_obj->set_element(static_cast<uint32_t>(i), fill_value);
            }
            return Value(this_obj);
        }, 1);

    PropertyDescriptor fill_length_desc(Value(1.0), PropertyAttributes::Configurable);
    fill_fn->set_property_descriptor("length", fill_length_desc);

    fill_fn->set_property("name", Value(std::string("fill")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor fill_desc(Value(fill_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("fill", fill_desc);

    auto array_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(0));
            result->set_element(1, Value(1));
            result->set_element(2, Value(2));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor keys_desc(Value(array_keys_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("keys", keys_desc);

    auto array_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(1));
            result->set_element(1, Value(2));
            result->set_element(2, Value(3));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor values_desc(Value(array_values_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("values", values_desc);

    auto array_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();

            auto pair0 = ObjectFactory::create_array();
            pair0->set_element(0, Value(0));
            pair0->set_element(1, Value(1));
            pair0->set_property("length", Value(2.0));

            result->set_element(0, Value(pair0.release()));
            result->set_property("length", Value(1.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor entries_desc(Value(array_entries_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("entries", entries_desc);

    auto array_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.toString called on non-object")));
                return Value();
            }

            // Spec: Get(O, "join") - goes through Proxy get trap via virtual dispatch
            Value join_val = this_obj->get_property("join");
            if (join_val.is_function()) {
                Function* join_fn = join_val.as_function();
                std::vector<Value> call_args;
                return join_fn->call(ctx, call_args, Value(this_obj));
            }

            if (this_obj->is_array()) {
                std::ostringstream result;
                uint32_t length = this_obj->get_length();

                for (uint32_t i = 0; i < length; i++) {
                    if (i > 0) {
                        result << ",";
                    }
                    Value element = this_obj->get_element(i);
                    if (!element.is_null() && !element.is_undefined()) {
                        result << element.to_string();
                    }
                }

                return Value(result.str());
            } else {
                return Value(std::string("[object Object]"));
            }
        });
    PropertyDescriptor array_toString_desc(Value(array_toString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toString", array_toString_desc);

    auto array_push_fn = ObjectFactory::create_native_function("push",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.push called on non-object")));
                return Value();
            }

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;
                for (const auto& arg : args) {
                    this_obj->set_property(std::to_string(length), arg);
                    length++;
                }
                this_obj->set_property("length", Value(static_cast<double>(length)));
                return Value(static_cast<double>(length));
            }

            for (const auto& arg : args) {
                this_obj->push(arg);
            }

            return Value(static_cast<double>(this_obj->get_length()));
        }, 1);


    PropertyDescriptor push_desc(Value(array_push_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("push", push_desc);

    auto copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;

                double target_arg = args.empty() ? 0.0 : args[0].to_number();
                int32_t target = target_arg < 0
                    ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(target_arg))
                    : std::min(static_cast<uint32_t>(target_arg), length);

                double start_arg = args.size() > 1 ? args[1].to_number() : 0.0;
                int32_t start = start_arg < 0
                    ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg))
                    : std::min(static_cast<uint32_t>(start_arg), length);

                double end_arg = args.size() > 2 && !args[2].is_undefined() ? args[2].to_number() : static_cast<double>(length);
                int32_t end = end_arg < 0
                    ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(end_arg))
                    : std::min(static_cast<uint32_t>(end_arg), length);

                int32_t count = std::min(end - start, static_cast<int32_t>(length) - target);

                if (count <= 0) {
                    return Value(this_obj);
                }

                // Spec: use HasProperty to handle holes - delete instead of set for missing source slots
                if (start < target && target < start + count) {
                    for (int32_t i = count - 1; i >= 0; i--) {
                        std::string from_key = std::to_string(start + i);
                        std::string to_key = std::to_string(target + i);
                        if (this_obj->has_property(from_key)) {
                            this_obj->set_property(to_key, this_obj->get_property(from_key));
                        } else {
                            this_obj->delete_property(to_key);
                        }
                        if (ctx.has_exception()) return Value();
                    }
                } else {
                    for (int32_t i = 0; i < count; i++) {
                        std::string from_key = std::to_string(start + i);
                        std::string to_key = std::to_string(target + i);
                        if (this_obj->has_property(from_key)) {
                            this_obj->set_property(to_key, this_obj->get_property(from_key));
                        } else {
                            this_obj->delete_property(to_key);
                        }
                        if (ctx.has_exception()) return Value();
                    }
                }

                return Value(this_obj);
            }

            uint32_t length = this_obj->get_length();

            double target_arg = args.empty() ? 0.0 : args[0].to_number();
            int32_t target = target_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(target_arg))
                : std::min(static_cast<uint32_t>(target_arg), length);

            double start_arg = args.size() > 1 ? args[1].to_number() : 0.0;
            int32_t start = start_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg))
                : std::min(static_cast<uint32_t>(start_arg), length);

            double end_arg = args.size() > 2 && !args[2].is_undefined() ? args[2].to_number() : static_cast<double>(length);
            int32_t end = end_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(end_arg))
                : std::min(static_cast<uint32_t>(end_arg), length);

            int32_t count = std::min(end - start, static_cast<int32_t>(length) - target);

            if (count <= 0) {
                return Value(this_obj);
            }

            if (start < target && target < start + count) {
                for (int32_t i = count - 1; i >= 0; i--) {
                    Value val = this_obj->get_element(start + i);
                    this_obj->set_element(target + i, val);
                }
            } else {
                for (int32_t i = 0; i < count; i++) {
                    Value val = this_obj->get_element(start + i);
                    this_obj->set_element(target + i, val);
                }
            }

            return Value(this_obj);
        });

    PropertyDescriptor copyWithin_length_desc(Value(2.0), PropertyAttributes::None);
    copyWithin_length_desc.set_configurable(true);
    copyWithin_length_desc.set_enumerable(false);
    copyWithin_length_desc.set_writable(false);
    copyWithin_fn->set_property_descriptor("length", copyWithin_length_desc);

    PropertyDescriptor copyWithin_desc(Value(copyWithin_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("copyWithin", copyWithin_desc);

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(-1.0);
            }

            if (args.empty()) {
                return Value(-1.0);
            }

            Value searchElement = args[0];
            Value length_val = this_obj->get_property("length");
            uint32_t length = static_cast<uint32_t>(length_val.is_number() ? length_val.as_number() : 0);

            if (length == 0) {
                return Value(-1.0);
            }

            int32_t fromIndex = static_cast<int32_t>(length - 1);
            if (args.size() > 1 && args[1].is_number()) {
                fromIndex = static_cast<int32_t>(args[1].as_number());
                if (fromIndex < 0) {
                    fromIndex = static_cast<int32_t>(length) + fromIndex;
                }
                if (fromIndex >= static_cast<int32_t>(length)) {
                    fromIndex = static_cast<int32_t>(length - 1);
                }
            }

            for (int32_t i = fromIndex; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                if (element.strict_equals(searchElement)) {
                    return Value(static_cast<double>(i));
                }
            }

            return Value(-1.0);
        });

    PropertyDescriptor lastIndexOf_length_desc(Value(1.0), PropertyAttributes::None);
    lastIndexOf_length_desc.set_configurable(true);
    lastIndexOf_length_desc.set_enumerable(false);
    lastIndexOf_length_desc.set_writable(false);
    lastIndexOf_fn->set_property_descriptor("length", lastIndexOf_length_desc);

    PropertyDescriptor lastIndexOf_desc(Value(lastIndexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("lastIndexOf", lastIndexOf_desc);

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("Array.prototype.reduceRight called on null or undefined");
                return Value();
            }

            if (args.empty()) {
                ctx.throw_type_error("Reduce of empty array with no initial value");
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_type_error("Callback must be a function");
                return Value();
            }
            Function* callback_func = static_cast<Function*>(callback.as_object());

            Value length_val = this_obj->get_property("length");
            uint32_t length = static_cast<uint32_t>(length_val.is_number() ? length_val.as_number() : 0);

            if (length == 0) {
                if (args.size() < 2) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
                return args[1];
            }

            Value accumulator;
            int32_t k;

            if (args.size() >= 2) {
                accumulator = args[1];
                k = static_cast<int32_t>(length - 1);
            } else {
                k = static_cast<int32_t>(length - 1);
                bool found = false;
                while (k >= 0) {
                    if (this_obj->has_property(std::to_string(k))) {
                        accumulator = this_obj->get_element(static_cast<uint32_t>(k));
                        k--;
                        found = true;
                        break;
                    }
                    k--;
                }
                if (!found) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
            }

            while (k >= 0) {
                if (this_obj->has_property(std::to_string(k))) {
                    Value element = this_obj->get_element(static_cast<uint32_t>(k));
                    std::vector<Value> callback_args = {
                        accumulator,
                        element,
                        Value(static_cast<double>(k)),
                        Value(this_obj)
                    };
                    accumulator = callback_func->call(ctx, callback_args, Value());
                }
                k--;
            }

            return accumulator;
        });

    PropertyDescriptor reduceRight_length_desc(Value(1.0), PropertyAttributes::None);
    reduceRight_length_desc.set_configurable(true);
    reduceRight_length_desc.set_enumerable(false);
    reduceRight_length_desc.set_writable(false);
    reduceRight_fn->set_property_descriptor("length", reduceRight_length_desc);

    PropertyDescriptor reduceRight_desc(Value(reduceRight_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reduceRight", reduceRight_desc);

    auto toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(std::string(""));

            uint32_t length = this_obj->get_length();
            std::string result;

            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) {
                    result += ",";
                }

                Value element = this_obj->get_element(i);

                if (!element.is_null() && !element.is_undefined()) {
                    if (element.is_object()) {
                        Object* elem_obj = element.as_object();
                        if (elem_obj->has_property("toLocaleString")) {
                            Value toLocaleString_val = elem_obj->get_property("toLocaleString");
                            if (toLocaleString_val.is_function()) {
                                Function* fn = toLocaleString_val.as_function();
                                std::vector<Value> empty_args;
                                Value str_val = fn->call(ctx, empty_args, element);
                                result += str_val.to_string();
                                continue;
                            }
                        }
                    }
                    result += element.to_string();
                }
            }

            return Value(result);
        });
    PropertyDescriptor array_toLocaleString_desc(Value(toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toLocaleString", array_toLocaleString_desc);

    auto toReversed_fn = ObjectFactory::create_native_function("toReversed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array(length);

            for (uint32_t i = 0; i < length; i++) {
                result->set_element(i, this_obj->get_element(length - 1 - i));
            }
            result->set_length(length);
            return Value(result.release());
        }, 0);
    PropertyDescriptor toReversed_desc(Value(toReversed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toReversed", toReversed_desc);

    auto toSorted_fn = ObjectFactory::create_native_function("toSorted",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array(length);

            for (uint32_t i = 0; i < length; i++) {
                result->set_element(i, this_obj->get_element(i));
            }
            result->set_length(length);

            return Value(result.release());
        }, 1);
    PropertyDescriptor toSorted_desc(Value(toSorted_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSorted", toSorted_desc);

    auto toSpliced_fn = ObjectFactory::create_native_function("toSpliced",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            int32_t start = args.empty() ? 0 : static_cast<int32_t>(args[0].to_number());
            uint32_t deleteCount = args.size() < 2 ? (length - start) : static_cast<uint32_t>(args[1].to_number());

            if (start < 0) {
                start = static_cast<int32_t>(length) + start;
                if (start < 0) start = 0;
            }
            if (start > static_cast<int32_t>(length)) start = length;

            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < static_cast<uint32_t>(start); i++) {
                result->set_element(result_index++, this_obj->get_element(i));
            }

            for (size_t i = 2; i < args.size(); i++) {
                result->set_element(result_index++, args[i]);
            }

            uint32_t after_start = static_cast<uint32_t>(start) + deleteCount;
            for (uint32_t i = after_start; i < length; i++) {
                result->set_element(result_index++, this_obj->get_element(i));
            }

            result->set_length(result_index);
            return Value(result.release());
        }, 2);
    PropertyDescriptor toSpliced_desc(Value(toSpliced_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSpliced", toSpliced_desc);

    auto array_concat_fn = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_array = ctx.get_this_binding();
            if (!this_array) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.concat called on null or undefined")));
                return Value();
            }

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_array, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) return Value(ObjectFactory::create_array(0).release());
            uint32_t result_index = 0;

            auto is_spreadable = [](Object* obj) -> bool {
                Value sv = obj->get_property("Symbol.isConcatSpreadable");
                if (!sv.is_undefined()) return sv.to_boolean();
                if (obj->get_type() == Object::ObjectType::Proxy) {
                    Object* target = static_cast<Proxy*>(obj)->get_proxy_target();
                    return target && target->is_array();
                }
                return obj->is_array();
            };

            auto get_length_prop = [](Object* obj) -> uint32_t {
                Value lv = obj->get_property("length");
                if (!lv.is_number()) return 0;
                double n = lv.as_number();
                if (std::isnan(n) || n <= 0) return 0;
                if (n > 0xFFFFFFFFu) return 0xFFFFFFFFu;
                return static_cast<uint32_t>(n);
            };

            auto get_elem_prop = [](Object* obj, uint32_t idx) -> Value {
                return obj->get_property(std::to_string(idx));
            };

            if (is_spreadable(this_array)) {
                uint32_t this_length = get_length_prop(this_array);
                for (uint32_t i = 0; i < this_length; i++) {
                    if (this_array->has_property(std::to_string(i))) {
                        Value elem = get_elem_prop(this_array, i);
                        if (ctx.has_exception()) return Value();
                        result->set_element(result_index, elem);
                    }
                    result_index++;
                }
            } else {
                result->set_element(result_index++, Value(this_array));
            }

            for (const auto& arg : args) {
                if (arg.is_object() || arg.is_function()) {
                    Object* arg_obj = arg.is_function()
                        ? static_cast<Object*>(arg.as_function())
                        : arg.as_object();
                    if (is_spreadable(arg_obj)) {
                        uint32_t arg_length = get_length_prop(arg_obj);
                        for (uint32_t i = 0; i < arg_length; i++) {
                            if (arg_obj->has_property(std::to_string(i))) {
                                Value elem = get_elem_prop(arg_obj, i);
                                if (ctx.has_exception()) return Value();
                                result->set_element(result_index, elem);
                            }
                            result_index++;
                        }
                    } else {
                        result->set_element(result_index++, arg);
                    }
                } else {
                    result->set_element(result_index++, arg);
                }
            }

            result->set_length(result_index);
            return result_val;
        });
    PropertyDescriptor concat_desc(Value(array_concat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("concat", concat_desc);

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(false);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.every callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (!result.to_boolean()) {
                    return Value(false);
                }
            }
            return Value(true);
        }, 1);
    PropertyDescriptor every_desc(Value(every_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("every", every_desc);

    auto filter_fn = ObjectFactory::create_native_function("filter",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.filter callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { auto a = ObjectFactory::create_array(); return Value(a.release()); }
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) continue;
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value test_result = callback->call(ctx, callback_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (test_result.to_boolean()) {
                    result->set_element(result_index++, element);
                }
            }
            result->set_length(result_index);
            return result_val;
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("filter", filter_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) return Value();

            Function* callback = args[0].as_function();
            Value this_arg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                callback->call(ctx, callback_args, this_arg);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    PropertyDescriptor forEach_desc(Value(forEach_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("forEach", forEach_desc);

    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);

            if (args.empty()) return Value(-1.0);
            Value search_element = args[0];

            uint32_t length = this_obj->get_length();

            int32_t start_index = 0;
            if (args.size() > 1) {
                double from_index = args[1].to_number();

                if (std::isnan(from_index)) {
                    start_index = 0;
                }
                else if (from_index < 0) {
                    int32_t relative_index = static_cast<int32_t>(length) + static_cast<int32_t>(from_index);
                    start_index = relative_index < 0 ? 0 : relative_index;
                }
                else {
                    start_index = static_cast<int32_t>(from_index);
                    if (start_index >= static_cast<int32_t>(length)) {
                        return Value(-1.0);
                    }
                }
            }

            for (uint32_t i = static_cast<uint32_t>(start_index); i < length; i++) {
                Value element = this_obj->get_element(i);
                if (element.strict_equals(search_element)) {
                    return Value(static_cast<double>(i));
                }
            }
            return Value(-1.0);
        }, 1);
    PropertyDescriptor array_indexOf_desc(Value(indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("indexOf", array_indexOf_desc);

    auto map_fn = ObjectFactory::create_native_function("map",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.map callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, length);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { auto a = ObjectFactory::create_array(length); return Value(a.release()); }

            for (uint32_t i = 0; i < length; i++) {
                if (this_obj->has_property(std::to_string(i))) {
                    Value element = this_obj->get_element(i);
                    std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                    Value mapped = callback->call(ctx, callback_args, thisArg);
                    if (ctx.has_exception()) return Value();
                    result->set_element(i, mapped);
                }
            }
            result->set_length(length);
            return result_val;
        }, 1);
    PropertyDescriptor map_desc(Value(map_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("map", map_desc);

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
            }

            Function* callback = args[0].as_function();
            uint32_t length = this_obj->get_length();

            if (length == 0 && args.size() < 2) {
                throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
            }

            uint32_t start_index = 0;
            Value accumulator;

            if (args.size() > 1) {
                accumulator = args[1];
            } else {
                bool found = false;
                for (uint32_t i = 0; i < length; i++) {
                    if (this_obj->has_property(std::to_string(i))) {
                        accumulator = this_obj->get_element(i);
                        start_index = i + 1;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
                }
            }

            for (uint32_t i = start_index; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {
                    accumulator,
                    element,
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                accumulator = callback->call(ctx, callback_args);
            }

            return accumulator;
        }, 1);
    PropertyDescriptor reduce_desc(Value(reduce_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reduce", reduce_desc);

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(false);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.some callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (result.to_boolean()) {
                    return Value(true);
                }
            }

            return Value(false);
        }, 1);
    PropertyDescriptor some_desc(Value(some_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("some", some_desc);

    auto findIndex_fn = ObjectFactory::create_native_function("findIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.findIndex callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (result.to_boolean()) {
                    return Value(static_cast<double>(i));
                }
            }

            return Value(-1.0);
        }, 1);
    PropertyDescriptor findIndex_desc(Value(findIndex_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findIndex", findIndex_desc);

    auto join_fn = ObjectFactory::create_native_function("join",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(std::string(""));

            std::string separator = args.empty() ? "," : args[0].to_string();
            std::string result = "";

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) result += separator;
                result += this_obj->get_element(i).to_string();
            }
            return Value(result);
        }, 1);
    PropertyDescriptor join_desc(Value(join_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("join", join_desc);

    auto pop_fn = ObjectFactory::create_native_function("pop",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;
                if (length == 0) {
                    this_obj->set_property("length", Value(0.0));
                    return Value();
                }
                uint32_t new_length = length - 1;
                std::string idx = std::to_string(new_length);
                Value element = this_obj->get_property(idx);
                this_obj->delete_property(idx);
                this_obj->set_property("length", Value(static_cast<double>(new_length)));
                return element;
            }

            uint32_t length = this_obj->get_length();
            if (length == 0) return Value();

            Value element = this_obj->get_element(length - 1);
            this_obj->set_length(length - 1);
            return element;
        }, 0);
    PropertyDescriptor pop_desc(Value(pop_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("pop", pop_desc);

    auto reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(this_obj);

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;
                for (uint32_t lower = 0; lower < length / 2; lower++) {
                    uint32_t upper = length - 1 - lower;
                    std::string lower_key = std::to_string(lower);
                    std::string upper_key = std::to_string(upper);
                    bool lower_exists = this_obj->has_property(lower_key);
                    bool upper_exists = this_obj->has_property(upper_key);
                    if (lower_exists && upper_exists) {
                        Value a = this_obj->get_property(lower_key);
                        Value b = this_obj->get_property(upper_key);
                        this_obj->set_property(lower_key, b);
                        this_obj->set_property(upper_key, a);
                    } else if (lower_exists) {
                        Value a = this_obj->get_property(lower_key);
                        this_obj->set_property(upper_key, a);
                        this_obj->delete_property(lower_key);
                    } else if (upper_exists) {
                        Value b = this_obj->get_property(upper_key);
                        this_obj->set_property(lower_key, b);
                        this_obj->delete_property(upper_key);
                    }
                    if (ctx.has_exception()) return Value();
                }
                return Value(this_obj);
            }

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length / 2; i++) {
                Value temp = this_obj->get_element(i);
                this_obj->set_element(i, this_obj->get_element(length - 1 - i));
                this_obj->set_element(length - 1 - i, temp);
            }
            return Value(this_obj);
        }, 0);
    PropertyDescriptor reverse_desc(Value(reverse_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reverse", reverse_desc);

    auto shift_fn = ObjectFactory::create_native_function("shift",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;
                if (length == 0) {
                    this_obj->set_property("length", Value(0.0));
                    return Value();
                }
                Value first = this_obj->get_property("0");
                for (uint32_t i = 1; i < length; i++) {
                    std::string from_key = std::to_string(i);
                    std::string to_key = std::to_string(i - 1);
                    if (this_obj->has_property(from_key)) {
                        this_obj->set_property(to_key, this_obj->get_property(from_key));
                    } else {
                        this_obj->delete_property(to_key);
                    }
                    if (ctx.has_exception()) return Value();
                }
                this_obj->delete_property(std::to_string(length - 1));
                this_obj->set_property("length", Value(static_cast<double>(length - 1)));
                return first;
            }

            uint32_t length = this_obj->get_length();
            if (length == 0) return Value();

            Value first = this_obj->get_element(0);
            for (uint32_t i = 1; i < length; i++) {
                this_obj->set_element(i - 1, this_obj->get_element(i));
            }
            this_obj->set_length(length - 1);
            return first;
        }, 0);
    PropertyDescriptor shift_desc(Value(shift_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("shift", shift_desc);

    auto slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                auto empty = ObjectFactory::create_array();
                return Value(empty.release());
            }

            uint32_t length = this_obj->get_length();

            int32_t start = 0;
            int32_t end = static_cast<int32_t>(length);

            if (!args.empty()) {
                start = static_cast<int32_t>(args[0].to_number());
            }
            if (args.size() >= 2) {
                end = static_cast<int32_t>(args[1].to_number());
            }

            if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
            if (end < 0) end = std::max(0, static_cast<int32_t>(length) + end);
            if (start < 0) start = 0;
            if (end > static_cast<int32_t>(length)) end = length;
            if (start > end) start = end;

            uint32_t count = (end > start) ? static_cast<uint32_t>(end - start) : 0;
            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, count);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { auto a = ObjectFactory::create_array(count); return Value(a.release()); }

            uint32_t result_index = 0;
            for (int32_t i = start; i < end; i++) {
                Value elem = this_obj->get_element(static_cast<uint32_t>(i));
                result->set_element(result_index++, elem);
            }
            result->set_length(result_index);
            return result_val;
        }, 2);
    PropertyDescriptor slice_desc(Value(slice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("slice", slice_desc);

    auto sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(this_obj);

            uint32_t length = this_obj->get_length();
            if (length <= 1) return Value(this_obj);

            Function* compareFn = nullptr;
            if (!args.empty() && !args[0].is_undefined()) {
                if (!args[0].is_function()) {
                    ctx.throw_type_error("Array.prototype.sort: compareFn must be a function or undefined");
                    return Value();
                }
                compareFn = args[0].as_function();
            }

            auto compare = [&](const Value& a, const Value& b) -> int {
                if (a.is_undefined() && b.is_undefined()) return 0;
                if (a.is_undefined()) return 1;
                if (b.is_undefined()) return -1;

                if (compareFn) {
                    std::vector<Value> compare_args = { a, b };
                    Value result = compareFn->call(ctx, compare_args);
                    double cmp = result.to_number();
                    if (std::isnan(cmp)) return 0;
                    return cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
                } else {
                    std::string str_a = a.to_string();
                    std::string str_b = b.to_string();
                    return str_a.compare(str_b);
                }
            };

            std::function<void(int32_t, int32_t)> quicksort;
            quicksort = [&](int32_t low, int32_t high) {
                if (low < high) {
                    Value pivot = this_obj->get_element(high);
                    int32_t i = low - 1;

                    for (int32_t j = low; j < high; j++) {
                        Value current = this_obj->get_element(j);
                        if (compare(current, pivot) <= 0) {
                            i++;
                            Value temp = this_obj->get_element(i);
                            this_obj->set_element(i, current);
                            this_obj->set_element(j, temp);
                        }
                    }

                    Value temp = this_obj->get_element(i + 1);
                    this_obj->set_element(i + 1, this_obj->get_element(high));
                    this_obj->set_element(high, temp);

                    int32_t pivot_index = i + 1;

                    quicksort(low, pivot_index - 1);
                    quicksort(pivot_index + 1, high);
                }
            };

            quicksort(0, static_cast<int32_t>(length) - 1);

            return Value(this_obj);
        }, 1);
    PropertyDescriptor sort_desc(Value(sort_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("sort", sort_desc);

    auto splice_fn = ObjectFactory::create_native_function("splice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;

                int32_t start = 0;
                if (!args.empty()) {
                    double start_arg = args[0].to_number();
                    if (start_arg < 0) {
                        start = std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg));
                    } else {
                        start = std::min(static_cast<uint32_t>(start_arg), length);
                    }
                }

                uint32_t delete_count = 0;
                if (args.empty()) {
                    return Value(ObjectFactory::create_array().release());
                } else if (args.size() < 2) {
                    delete_count = length - start;
                } else {
                    double delete_arg = args[1].to_number();
                    if (delete_arg < 0) {
                        delete_count = 0;
                    } else {
                        delete_count = std::min(static_cast<uint32_t>(delete_arg), length - static_cast<uint32_t>(start));
                    }
                }

                std::vector<Value> items_to_insert;
                for (size_t i = 2; i < args.size(); i++) {
                    items_to_insert.push_back(args[i]);
                }

                Value result_val = array_species_create(ctx, this_obj, delete_count);
                if (ctx.has_exception()) return Value();
                Object* result = result_val.is_object() ? result_val.as_object()
                               : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                               : nullptr;
                if (!result) { auto a = ObjectFactory::create_array(delete_count); return Value(a.release()); }
                for (uint32_t i = 0; i < delete_count; i++) {
                    result->set_element(i, this_obj->get_property(std::to_string(start + i)));
                }
                result->set_length(delete_count);

                uint32_t item_count = static_cast<uint32_t>(items_to_insert.size());
                uint32_t new_length = length - delete_count + item_count;

                if (item_count > delete_count) {
                    uint32_t shift = item_count - delete_count;
                    for (int32_t i = static_cast<int32_t>(length) - 1; i >= static_cast<int32_t>(start + delete_count); i--) {
                        std::string from_key = std::to_string(i);
                        std::string to_key = std::to_string(i + shift);
                        if (this_obj->has_property(from_key)) {
                            this_obj->set_property(to_key, this_obj->get_property(from_key));
                        } else {
                            this_obj->delete_property(to_key);
                        }
                        if (ctx.has_exception()) return Value();
                    }
                } else if (delete_count > item_count) {
                    uint32_t shift = delete_count - item_count;
                    for (uint32_t i = static_cast<uint32_t>(start) + delete_count; i < length; i++) {
                        std::string from_key = std::to_string(i);
                        std::string to_key = std::to_string(i - shift);
                        if (this_obj->has_property(from_key)) {
                            this_obj->set_property(to_key, this_obj->get_property(from_key));
                        } else {
                            this_obj->delete_property(to_key);
                        }
                        if (ctx.has_exception()) return Value();
                    }
                    for (uint32_t i = new_length; i < length; i++) {
                        this_obj->delete_property(std::to_string(i));
                    }
                }

                for (uint32_t i = 0; i < item_count; i++) {
                    this_obj->set_property(std::to_string(start + i), items_to_insert[i]);
                }

                this_obj->set_property("length", Value(static_cast<double>(new_length)));
                return result_val;
            }

            uint32_t length = this_obj->get_length();

            int32_t start = 0;
            if (!args.empty()) {
                double start_arg = args[0].to_number();
                if (start_arg < 0) {
                    start = std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg));
                } else {
                    start = std::min(static_cast<uint32_t>(start_arg), length);
                }
            }

            uint32_t delete_count = 0;
            if (args.empty()) {
                return Value(ObjectFactory::create_array().release());
            } else if (args.size() < 2) {
                delete_count = length - start;
            } else {
                double delete_arg = args[1].to_number();
                if (delete_arg < 0) {
                    delete_count = 0;
                } else {
                    delete_count = std::min(static_cast<uint32_t>(delete_arg), length - start);
                }
            }

            std::vector<Value> items_to_insert;
            for (size_t i = 2; i < args.size(); i++) {
                items_to_insert.push_back(args[i]);
            }

            // ES6: use @@species constructor for removed-elements result
            Value result_val = array_species_create(ctx, this_obj, delete_count);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { auto a = ObjectFactory::create_array(delete_count); return Value(a.release()); }
            for (uint32_t i = 0; i < delete_count; i++) {
                result->set_element(i, this_obj->get_element(start + i));
            }
            result->set_length(delete_count);

            uint32_t item_count = items_to_insert.size();
            uint32_t new_length = length - delete_count + item_count;

            if (item_count > delete_count) {
                uint32_t shift = item_count - delete_count;
                for (int32_t i = length - 1; i >= static_cast<int32_t>(start + delete_count); i--) {
                    this_obj->set_element(i + shift, this_obj->get_element(i));
                }
            }
            else if (delete_count > item_count) {
                uint32_t shift = delete_count - item_count;
                for (uint32_t i = start + delete_count; i < length; i++) {
                    this_obj->set_element(i - shift, this_obj->get_element(i));
                }
            }

            for (uint32_t i = 0; i < item_count; i++) {
                this_obj->set_element(start + i, items_to_insert[i]);
            }

            this_obj->set_length(new_length);

            return result_val;
        }, 2);
    PropertyDescriptor splice_desc(Value(splice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("splice", splice_desc);

    auto unshift_fn = ObjectFactory::create_native_function("unshift",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(0.0);

            if (this_obj->get_type() == Object::ObjectType::Proxy) {
                Value len_val = this_obj->get_property("length");
                uint32_t length = len_val.is_number() ? static_cast<uint32_t>(len_val.as_number()) : 0;
                uint32_t argCount = static_cast<uint32_t>(args.size());
                for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
                    std::string from_key = std::to_string(i);
                    std::string to_key = std::to_string(i + argCount);
                    if (this_obj->has_property(from_key)) {
                        this_obj->set_property(to_key, this_obj->get_property(from_key));
                    } else {
                        this_obj->delete_property(to_key);
                    }
                    if (ctx.has_exception()) return Value();
                }
                for (uint32_t i = 0; i < argCount; i++) {
                    this_obj->set_property(std::to_string(i), args[i]);
                }
                uint32_t new_length = length + argCount;
                this_obj->set_property("length", Value(static_cast<double>(new_length)));
                return Value(static_cast<double>(new_length));
            }

            uint32_t length = this_obj->get_length();
            uint32_t argCount = args.size();

            for (int32_t i = length - 1; i >= 0; i--) {
                this_obj->set_element(i + argCount, this_obj->get_element(i));
            }

            for (uint32_t i = 0; i < argCount; i++) {
                this_obj->set_element(i, args[i]);
            }

            uint32_t new_length = length + argCount;
            this_obj->set_length(new_length);
            return Value(static_cast<double>(new_length));
        }, 1);
    PropertyDescriptor unshift_desc(Value(unshift_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("unshift", unshift_desc);

    Object* array_proto_ptr = array_prototype.get();

    PropertyDescriptor array_constructor_desc(Value(array_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    array_proto_ptr->set_property_descriptor("constructor", array_constructor_desc);

    auto array_iterator_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            auto iterator = ObjectFactory::create_object();
            uint32_t length = 0;
            Value len_val = this_obj->get_property("length");
            if (!len_val.is_undefined()) length = static_cast<uint32_t>(len_val.to_number());
            auto index = std::make_shared<uint32_t>(0);
            Object* arr_ptr = this_obj;
            auto next_fn = ObjectFactory::create_native_function("next",
                [arr_ptr, length, index](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    auto result = ObjectFactory::create_object();
                    if (*index >= length) {
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                    } else {
                        result->set_property("done", Value(false));
                        result->set_property("value", arr_ptr->get_element(*index));
                        (*index)++;
                    }
                    return Value(result.release());
                }, 0);
            iterator->set_property("next", Value(next_fn.release()));
            if (Iterator::s_array_iterator_prototype_) {
                iterator->set_prototype(Iterator::s_array_iterator_prototype_);
            }
            return Value(iterator.release());
        }, 0);
    array_proto_ptr->set_property("Symbol.iterator", Value(array_iterator_fn.release()), PropertyAttributes::BuiltinFunction);

    Symbol* unscopables_symbol = Symbol::get_well_known(Symbol::UNSCOPABLES);
    if (unscopables_symbol) {
        auto unscopables_obj = ObjectFactory::create_object();
        unscopables_obj->set_prototype(nullptr);
        unscopables_obj->set_property("at", Value(true));
        unscopables_obj->set_property("copyWithin", Value(true));
        unscopables_obj->set_property("entries", Value(true));
        unscopables_obj->set_property("fill", Value(true));
        unscopables_obj->set_property("find", Value(true));
        unscopables_obj->set_property("findIndex", Value(true));
        unscopables_obj->set_property("findLast", Value(true));
        unscopables_obj->set_property("findLastIndex", Value(true));
        unscopables_obj->set_property("flat", Value(true));
        unscopables_obj->set_property("flatMap", Value(true));
        unscopables_obj->set_property("includes", Value(true));
        unscopables_obj->set_property("keys", Value(true));
        unscopables_obj->set_property("values", Value(true));
        PropertyDescriptor unscopables_desc(Value(unscopables_obj.release()), PropertyAttributes::Configurable);
        array_proto_ptr->set_property_descriptor(unscopables_symbol->get_description(), unscopables_desc);
    }

    array_constructor->set_property("prototype", Value(array_prototype.release()), PropertyAttributes::None);

    auto array_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ctx.get_this_binding());
        }, 0);

    PropertyDescriptor array_species_desc;
    array_species_desc.set_getter(array_species_getter.get());
    array_species_desc.set_enumerable(false);
    array_species_desc.set_configurable(true);

    Value array_species_symbol = ctx.get_global_object()->get_property("Symbol");
    if (array_species_symbol.is_object()) {
        Object* symbol_constructor = array_species_symbol.as_object();
        Value species_key = symbol_constructor->get_property("species");
        if (species_key.is_symbol()) {
            array_constructor->set_property_descriptor(species_key.as_symbol()->to_property_key(), array_species_desc);
        }
    }

    array_species_getter.release();

    ObjectFactory::set_array_prototype(array_proto_ptr);

    ctx.register_built_in_object("Array", array_constructor.release());
}

} // namespace Quanta
