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
#include "quanta/core/runtime/Promise.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_set>
#include "quanta/parser/AST.h"

namespace Quanta {

// ToObject for array generics: boxes primitive this (bool/number/string) with the correct prototype. 
// Only Array.prototype methods should call this -- Iterator/Number/String methods must NOT box
static Object* array_to_object(Context& ctx) {
    if (ctx.original_this_was_nullish()) return nullptr;
    Object* obj = ctx.get_this_binding();
    // Check if this call's this was primitive (set by Function::call)
    if (!ctx.has_binding("__primitive_this__")) return obj;
    Value prim = ctx.get_binding("__primitive_this__");
    if (prim.is_symbol() || prim.is_bigint()) {
        // Reuse Object()'s wrapper setup rather than duplicating it here.
        Value obj_ctor_val = ctx.get_binding("Object");
        if (obj_ctor_val.is_function()) {
            Value boxed = obj_ctor_val.as_function()->call(ctx, {prim});
            if (boxed.is_object()) {
                Object* raw = boxed.as_object();
                ctx.set_binding("__primitive_this__", Value(raw));
                return raw;
            }
        }
        return obj;
    }
    if (!prim.is_boolean() && !prim.is_number() && !prim.is_string()) return obj;
    // Box the primitive
    const char* ctor_name = nullptr;
    std::unique_ptr<Object> boxed;
    if (prim.is_boolean()) { boxed = ObjectFactory::create_boolean(prim.to_boolean()); ctor_name = "Boolean"; }
    else if (prim.is_number()) { boxed = ObjectFactory::create_number(prim.to_number()); ctor_name = "Number"; }
    else { boxed = ObjectFactory::create_string(prim.to_string()); ctor_name = "String"; }
    if (ctor_name) {
        if (Object* ctor = ctx.get_built_in_object(ctor_name)) {
            Value proto = ctor->get_property("prototype");
            if (proto.is_object()) boxed->set_prototype(proto.as_object());
        }
    }
    Object* raw = boxed.release();
    // Pin in context so GC can't collect during this call
    ctx.set_binding("__primitive_this__", Value(raw));
    return raw;
}

// ToPrimitive("string") for sort's default comparator: toString() then valueOf(), matching
// String(x) rather than Value::to_string()'s "default" hint (valueOf() first).
static std::string sort_default_to_string(Context& ctx, const Value& v) {
    if (v.is_symbol()) {
        ctx.throw_type_error("Cannot convert a Symbol value to a string");
        return "";
    }
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

// ToNumber via ToPrimitive("number"): valueOf() then toString(), throwing if neither yields a
// primitive (Value::to_number() just falls back to NaN instead).
static double to_number_throwing(Context& ctx, const Value& v) {
    if (v.is_symbol()) {
        ctx.throw_type_error("Cannot convert a Symbol value to a number");
        return 0;
    }
    if (!v.is_object() && !v.is_function()) return v.to_number();
    Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
    Value valueOf_fn = obj->get_property("valueOf");
    if (ctx.has_exception()) return 0;
    if (valueOf_fn.is_function()) {
        Value prim = valueOf_fn.as_function()->call(ctx, {}, v);
        if (ctx.has_exception()) return 0;
        if (!prim.is_object() && !prim.is_function()) return prim.to_number();
    }
    Value toString_fn = obj->get_property("toString");
    if (ctx.has_exception()) return 0;
    if (toString_fn.is_function()) {
        Value prim = toString_fn.as_function()->call(ctx, {}, v);
        if (ctx.has_exception()) return 0;
        if (!prim.is_object() && !prim.is_function()) return prim.to_number();
    }
    ctx.throw_type_error("Cannot convert object to primitive value");
    return 0;
}

// LengthOfArrayLike: ToLength(Get(obj, "length")), clamped to [0, 2^53-1] -- unlike
// Object::get_length() this isn't capped to uint32_t.
static double array_like_length(Context& ctx, Object* obj) {
    Value len_val = obj->get_property("length");
    if (ctx.has_exception()) return 0;
    double n = to_number_throwing(ctx, len_val);
    if (ctx.has_exception()) return 0;
    if (std::isnan(n) || n <= 0) return 0;
    if (n > 9007199254740991.0) return 9007199254740991.0;
    return std::floor(n);
}

// ToIntegerOrInfinity, throwing variant of to_number_throwing.
static double to_integer_or_infinity_throwing(Context& ctx, const Value& v) {
    double n = to_number_throwing(ctx, v);
    if (ctx.has_exception() || std::isnan(n)) return 0;
    if (std::isinf(n)) return n;
    return std::trunc(n);
}

// Array.fromAsync (spec 23.1.2.1): drives iteration via recursive Promise
// chaining, since native functions can't suspend a fiber. Per-step state lives
// as hidden properties on result_promise (GC-rooted, survives microtask ticks);
// handler functions are created once in fa_setup_handlers and reused each step.
static void fa_request_next(Context& ctx, Promise* result_promise);
static void fa_request_arraylike_next(Context& ctx, Promise* result_promise);

static void fa_reject(Context& ctx, Promise* result_promise, const Value& reason) {
    // Close the iterator (if any) before rejecting -- spec requires IteratorClose on
    // abrupt completion so generator finally blocks run (e.g. rejecting thenables).
    if (!result_promise->is_pending()) return;
    Value iter_v = result_promise->get_property("__fa_iter__");
    if (iter_v.is_object()) {
        Value ret_fn = iter_v.as_object()->get_property("return");
        if (ret_fn.is_function()) {
            ret_fn.as_function()->call(ctx, {}, iter_v);
            ctx.clear_exception();
        }
    }
    result_promise->reject(reason);
}

static void fa_set_and_advance(Context& ctx, Promise* result_promise, const Value& value) {
    Value arr_v = result_promise->get_property("__fa_arr__");
    Value idx_v = result_promise->get_property("__fa_idx__");
    uint32_t idx = static_cast<uint32_t>(idx_v.to_number());
    if (arr_v.is_object()) {
        arr_v.as_object()->set_element(idx, value);
        arr_v.as_object()->set_property("length", Value(static_cast<double>(idx + 1)));
    }
    result_promise->set_property("__fa_idx__", Value(static_cast<double>(idx + 1)));
    fa_request_next(ctx, result_promise);
}

// Registers a generic resolve/reject pair on `p` that route into `on_fulfilled_key`
// (looked up on result_promise, called with the resolved value) or generic rejection.
static void fa_chain(Context& ctx, Promise* result_promise, Promise* p, const char* on_fulfilled_key) {
    Value ff = result_promise->get_property(on_fulfilled_key);
    Value fr = result_promise->get_property("__fa_on_reject__");
    if (ctx.has_exception()) return;
    p->then(ff.is_function() ? ff.as_function() : nullptr,
            fr.is_function() ? fr.as_function() : nullptr);
}

static void fa_setup_handlers(Context& ctx, Promise* result_promise) {
    (void)ctx;
    // Generic rejection: reject result_promise with the reason. Bind via closure
    // over the raw Promise* (mirrors Promise.all's pattern of capturing rp).
    Promise* rp = result_promise;
    auto reject_fn = ObjectFactory::create_native_function("",
        [rp](Context& c, const std::vector<Value>& args) -> Value {
            fa_reject(c, rp, args.empty() ? Value() : args[0]);
            return Value();
        });
    result_promise->set_property("__fa_on_reject__", Value(reject_fn.release()));

    // Step 1: handle the (possibly-awaited) iterator-result object {value, done}.
    auto on_next_settled = ObjectFactory::create_native_function("",
        [rp](Context& c, const std::vector<Value>& args) -> Value {
            Value nr = args.empty() ? Value() : args[0];
            if (!nr.is_object()) {
                fa_reject(c, rp, Value(std::string("TypeError: iterator result is not an object")));
                return Value();
            }
            Value done = nr.as_object()->get_property("done");
            if (c.has_exception()) { Value e = c.get_exception(); c.clear_exception(); fa_reject(c, rp, e); return Value(); }
            if (done.to_boolean()) {
                Value arr_v = rp->get_property("__fa_arr__");
                if (rp->is_pending()) rp->fulfill(arr_v);
                return Value();
            }
            Value value = nr.as_object()->get_property("value");
            if (c.has_exception()) { Value e = c.get_exception(); c.clear_exception(); fa_reject(c, rp, e); return Value(); }

            Value used_async_v = rp->get_property("__fa_async__");
            if (!used_async_v.to_boolean()) {
                // Sync iterator (CreateAsyncFromSyncIterator): Await the value too.
                Promise* vp = Promise::resolve(value);
                fa_chain(c, rp, vp, "__fa_on_value__");
            } else {
                Value cb = rp->get_property("__fa_on_value__");
                if (cb.is_function()) cb.as_function()->call(c, {value}, Value());
            }
            return Value();
        });
    result_promise->set_property("__fa_on_next_settled__", Value(on_next_settled.release()));

    // Step 2: value (now resolved) -- apply mapfn if present (and Await its result).
    auto on_value = ObjectFactory::create_native_function("",
        [rp](Context& c, const std::vector<Value>& args) -> Value {
            Value value = args.empty() ? Value() : args[0];
            Value mapfn_v = rp->get_property("__fa_mapfn__");
            if (mapfn_v.is_function()) {
                Value idx_v = rp->get_property("__fa_idx__");
                Value this_arg = rp->get_property("__fa_thisarg__");
                std::vector<Value> margs = { value, idx_v };
                Value mapped = mapfn_v.as_function()->call(c, margs, this_arg);
                if (c.has_exception()) { Value e = c.get_exception(); c.clear_exception(); fa_reject(c, rp, e); return Value(); }
                Promise* mp = Promise::resolve(mapped);
                fa_chain(c, rp, mp, "__fa_on_mapped__");
            } else {
                fa_set_and_advance(c, rp, value);
            }
            return Value();
        });
    result_promise->set_property("__fa_on_value__", Value(on_value.release()));

    // Step 3: mapped value (now resolved) -- store and advance to the next index.
    auto on_mapped = ObjectFactory::create_native_function("",
        [rp](Context& c, const std::vector<Value>& args) -> Value {
            fa_set_and_advance(c, rp, args.empty() ? Value() : args[0]);
            return Value();
        });
    result_promise->set_property("__fa_on_mapped__", Value(on_mapped.release()));
}

// Array-like path (no @@asyncIterator/@@iterator): index through .length,
// Awaiting each element (spec: Set nextValue to ? Await(Get(arrayLike, Pk))).
static void fa_request_arraylike_next(Context& ctx, Promise* result_promise) {
    Value idx_v = result_promise->get_property("__fa_idx__");
    Value len_v = result_promise->get_property("__fa_len__");
    uint32_t idx = static_cast<uint32_t>(idx_v.to_number());
    uint32_t len = static_cast<uint32_t>(len_v.to_number());
    if (idx >= len) {
        Value arr_v = result_promise->get_property("__fa_arr__");
        if (result_promise->is_pending()) result_promise->fulfill(arr_v);
        return;
    }
    Value arraylike = result_promise->get_property("__fa_arraylike__");
    Object* al_obj = arraylike.is_function()
        ? static_cast<Object*>(arraylike.as_function()) : (arraylike.is_object() ? arraylike.as_object() : nullptr);
    if (!al_obj) {
        fa_reject(ctx, result_promise, Value(std::string("TypeError: Array.fromAsync: array-like is not an object")));
        return;
    }
    Value element = al_obj->get_property(std::to_string(idx));
    if (ctx.has_exception()) {
        Value e = ctx.get_exception(); ctx.clear_exception();
        fa_reject(ctx, result_promise, e);
        return;
    }
    Promise* ep = Promise::resolve(element);
    fa_chain(ctx, result_promise, ep, "__fa_on_value__");
}

// Drives the next iteration step: requests the array-like element when no
// iterator is in use, otherwise calls the iterator's next() method.
static void fa_request_next(Context& ctx, Promise* result_promise) {
    Value next_fn_v = result_promise->get_property("__fa_next__");
    if (next_fn_v.is_undefined()) {
        fa_request_arraylike_next(ctx, result_promise);
        return;
    }
    Value iterator_v = result_promise->get_property("__fa_iter__");
    if (!next_fn_v.is_function()) {
        fa_reject(ctx, result_promise, Value(std::string("TypeError: iterator has no next method")));
        return;
    }
    Value next_result = next_fn_v.as_function()->call(ctx, {}, iterator_v);
    if (ctx.has_exception()) {
        Value e = ctx.get_exception(); ctx.clear_exception();
        fa_reject(ctx, result_promise, e);
        return;
    }
    Promise* np = Promise::resolve(next_result);
    fa_chain(ctx, result_promise, np, "__fa_on_next_settled__");
}

// CreateDataPropertyOrThrow: a non-configurable existing property always rejects
// (the new descriptor always specifies configurable:true); otherwise overwrite freely.
static bool create_data_property_or_throw(Context& ctx, Object* target, const std::string& key, const Value& v) {
    if (target->has_own_property(key)) {
        PropertyDescriptor pd = target->get_property_descriptor(key);
        if (!pd.is_configurable()) {
            ctx.throw_type_error("Cannot redefine property: " + key);
            return false;
        }
    } else if (!target->is_extensible()) {
        ctx.throw_type_error("Cannot add property " + key + ", object is not extensible");
        return false;
    }
    target->set_property_descriptor(key, PropertyDescriptor(v, PropertyAttributes::Default));
    return true;
}

// GetFunctionRealm-lite: tracks every realm's intrinsic %Array% so a foreign one can be detected.
static std::unordered_set<Function*>& all_array_intrinsics() {
    static std::unordered_set<Function*> registry;
    return registry;
}

// ArrayCreate's own length check: a plain (non-species) array can't exceed 2^32-1.
static Value array_create_or_range_error(Context& ctx, double length) {
    if (length > 4294967295.0) {
        ctx.throw_range_error("Invalid array length");
        return Value();
    }
    return Value(ObjectFactory::create_array(static_cast<uint32_t>(length)).release());
}

static Value array_species_create(Context& ctx, Object* original_array, double length) {
    if (length == 0) length = 0; // normalize -0 to +0
    bool is_actual_array = original_array->is_array();
    if (!is_actual_array && original_array->get_type() == Object::ObjectType::Proxy) {
        Object* target = static_cast<Proxy*>(original_array)->get_proxy_target();
        while (target && target->get_type() == Object::ObjectType::Proxy) {
            target = static_cast<Proxy*>(target)->get_proxy_target();
        }
        is_actual_array = target && target->is_array();
    }
    if (!is_actual_array) {
        return array_create_or_range_error(ctx, length);
    }
    Value ctor_val = original_array->get_property("constructor");
    if (ctx.has_exception()) return Value();
    if (ctor_val.is_undefined()) {
        return array_create_or_range_error(ctx, length);
    }
    if (ctor_val.is_function() || ctor_val.is_object()) {
        Object* ctor = ctor_val.is_function()
            ? static_cast<Object*>(ctor_val.as_function())
            : ctor_val.as_object();
        // A foreign-realm %Array% is treated as if C were undefined.
        if (ctor_val.is_function() && static_cast<Function*>(ctor_val.as_function())->is_constructor()) {
            Function* ctor_fn = ctor_val.as_function();
            Value this_realm_array = ctx.get_binding("Array");
            bool is_foreign_array_intrinsic = all_array_intrinsics().count(ctor_fn) > 0 &&
                !(this_realm_array.is_function() && this_realm_array.as_function() == ctor_fn);
            if (is_foreign_array_intrinsic) {
                return array_create_or_range_error(ctx, length);
            }
        }
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            Value species_val = ctor->get_property(species_sym->to_property_key());
            if (ctx.has_exception()) return Value();
            if (species_val.is_null() || species_val.is_undefined()) {
                return array_create_or_range_error(ctx, length);
            } else if (species_val.is_function() &&
                       static_cast<Function*>(species_val.as_function())->is_constructor()) {
                Function* species_fn = species_val.as_function();
                Value result = species_fn->construct(ctx, {Value(length)});
                if (ctx.has_exception()) return Value();
                return result;
            } else {
                ctx.throw_type_error("Species constructor is not a constructor");
                return Value();
            }
        }
        return array_create_or_range_error(ctx, length);
    }
    // Non-undefined, non-object constructor (null/number/string/boolean): never
    // a constructor, so step 9 of ArraySpeciesCreate throws unconditionally.
    ctx.throw_type_error("constructor property is not a constructor");
    return Value();
}

// IsArray: unwraps Proxy chains (throwing if revoked), unlike Object::is_array().
static bool is_array_spec(Context& ctx, Object* obj) {
    while (obj && obj->get_type() == Object::ObjectType::Proxy) {
        Proxy* p = static_cast<Proxy*>(obj);
        if (p->is_revoked()) { ctx.throw_type_error("Cannot perform operation on a revoked proxy"); return false; }
        obj = p->get_proxy_target();
    }
    return obj && obj->is_array();
}

// FlattenIntoArray (23.1.3.13.1): recursively copies source's elements into target
// starting at start, flattening nested arrays up to depth levels, optionally mapping
// each element first. Returns the next free target index, or -1 on exception.
static double flatten_into_array(Context& ctx, Object* target, Object* source, double source_len,
                                  double start, double depth, Function* mapper_fn = nullptr,
                                  const Value& this_arg = Value()) {
    double target_index = start;
    for (double source_index = 0; source_index < source_len; source_index++) {
        std::string key = Value(source_index).to_string();
        bool exists = source->has_property(key);
        if (ctx.has_exception()) return -1;
        if (!exists) continue;

        Value element = source->get_property(key);
        if (ctx.has_exception()) return -1;
        if (mapper_fn) {
            std::vector<Value> call_args = {element, Value(source_index), Value(source)};
            element = mapper_fn->call(ctx, call_args, this_arg);
            if (ctx.has_exception()) return -1;
        }

        bool should_flatten = depth > 0 && is_array_spec(ctx, element.is_object() ? element.as_object() : nullptr);
        if (ctx.has_exception()) return -1;
        if (should_flatten) {
            double element_len = array_like_length(ctx, element.as_object());
            if (ctx.has_exception()) return -1;
            target_index = flatten_into_array(ctx, target, element.as_object(), element_len,
                                               target_index, depth - 1);
            if (ctx.has_exception()) return -1;
        } else {
            if (target_index >= 9007199254740991.0) {
                ctx.throw_type_error("flatten target index exceeded 2**53 - 1");
                return -1;
            }
            if (!create_data_property_or_throw(ctx, target, Value(target_index).to_string(), element)) return -1;
            target_index++;
        }
    }
    return target_index;
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
            Value items = args.empty() ? Value() : args[0];
            Value thisArg = (args.size() > 2) ? args[2] : Value();

            // Validate mapfn (spec step 2-3: if not undefined, must be callable).
            Function* mapfn = nullptr;
            if (args.size() > 1 && !args[1].is_undefined()) {
                if (!args[1].is_function()) {
                    ctx.throw_type_error("Array.from: mapfn must be callable");
                    return Value();
                }
                mapfn = args[1].as_function();
            }

            // Determine result array constructor: `this` if it's a constructor, else Array.
            Object* this_binding = array_to_object(ctx);
            Function* ctor = (this_binding && this_binding->is_function())
                ? static_cast<Function*>(this_binding) : nullptr;

            auto make_result = [&](uint32_t length) -> Object* {
                if (ctor) {
                    Value v = ctor->construct(ctx, {Value(static_cast<double>(length))});
                    if (ctx.has_exception()) return nullptr;
                    if (v.is_object()) return v.as_object();
                    if (v.is_function()) return static_cast<Object*>(v.as_function());
                }
                return ObjectFactory::create_array(length).release();
            };

            // null/undefined → TypeError
            if (items.is_null() || items.is_undefined()) {
                ctx.throw_type_error("Array.from called on null or undefined");
                return Value();
            }

            // Iterable protocol
            if (items.is_object() || items.is_function() || items.is_string()) {
                Object* src_obj = nullptr;
                Value items_boxed = items;
                if (items.is_string()) {
                    // Box string via Object() to expose its iterator
                    Value obj_ctor = ctx.get_binding("Object");
                    if (obj_ctor.is_function()) {
                        Value boxed = obj_ctor.as_function()->call(ctx, {items});
                        if (!ctx.has_exception() && boxed.is_object()) { items_boxed = boxed; src_obj = boxed.as_object(); }
                    }
                    if (!src_obj) {
                        // Fallback: iterate over string code units directly
                        std::string str = items.to_string();
                        uint32_t len = static_cast<uint32_t>(str.size());
                        Object* res = make_result(len);
                        if (!res) return Value();
                        for (uint32_t i = 0; i < len; i++) {
                            Value el(std::string(1, str[i]));
                            if (mapfn) { el = mapfn->call(ctx, {el, Value(static_cast<double>(i))}, thisArg); if (ctx.has_exception()) return Value(); }
                            if (!create_data_property_or_throw(ctx, res, std::to_string(i), el)) return Value();
                        }
                        res->set_property("length", Value(static_cast<double>(len)));
                        return Value(res);
                    }
                } else {
                    src_obj = items.is_function() ? static_cast<Object*>(items.as_function()) : items.as_object();
                }

                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym && src_obj) {
                    Value iter_method = src_obj->get_property(iter_sym->to_property_key());
                    if (ctx.has_exception()) return Value();
                    if (!iter_method.is_undefined() && !iter_method.is_null() && !iter_method.is_function()) {
                        ctx.throw_type_error("@@iterator is not callable");
                        return Value();
                    }
                    if (iter_method.is_function()) {
                        // Spec: Construct(C) happens BEFORE GetIterator (calling iter_method).
                        Object* res = nullptr;
                        if (ctor) {
                            Value rv = ctor->construct(ctx, {});
                            if (ctx.has_exception()) return Value();
                            res = rv.is_object() ? rv.as_object()
                                : rv.is_function() ? static_cast<Object*>(rv.as_function()) : nullptr;
                        }
                        if (!res) res = ObjectFactory::create_array(0).release();

                        Value iterator_obj = iter_method.as_function()->call(ctx, {}, items_boxed);
                        if (ctx.has_exception()) return Value();
                        if (!iterator_obj.is_object()) { ctx.throw_type_error("Array.from: iterator must return an object"); return Value(); }
                        Object* iterator = iterator_obj.as_object();
                        Value next_fn = iterator->get_property("next");
                        if (ctx.has_exception()) return Value();
                        if (!next_fn.is_function()) { ctx.throw_type_error("Array.from: iterator.next must be a function"); return Value(); }
                        // IteratorClose: call .return() but preserve the completion value.
                        auto close_iter = [&]() {
                            bool had_exc = ctx.has_exception();
                            Value saved_exc = had_exc ? ctx.get_exception() : Value();
                            if (had_exc) ctx.clear_exception();
                            Value rm = iterator->get_property("return");
                            if (rm.is_function()) { rm.as_function()->call(ctx, {}, iterator_obj); ctx.clear_exception(); }
                            if (had_exc) ctx.throw_exception(saved_exc);
                        };
                        uint32_t idx = 0;
                        for (uint32_t ii = 0; ii < 0xFFFFFFFFu; ii++) {
                            Value result = next_fn.as_function()->call(ctx, {}, iterator_obj);
                            if (ctx.has_exception()) { close_iter(); return Value(); }
                            if (!result.is_object()) { ctx.throw_type_error("Array.from: iterator result not an object"); return Value(); }
                            Value done_v = result.as_object()->get_property("done");
                            if (ctx.has_exception()) { close_iter(); return Value(); }
                            if (done_v.to_boolean()) break;
                            Value val = result.as_object()->get_property("value");
                            if (ctx.has_exception()) { close_iter(); return Value(); }
                            if (mapfn) {
                                val = mapfn->call(ctx, {val, Value(static_cast<double>(idx))}, thisArg);
                                if (ctx.has_exception()) { close_iter(); return Value(); }
                            }
                            if (!create_data_property_or_throw(ctx, res, std::to_string(idx), val)) return Value();
                            idx++;
                        }
                        bool ok = res->set_property("length", Value(static_cast<double>(idx)));
                        if (!ok) { ctx.throw_type_error("Cannot set length"); return Value(); }
                        return Value(res);
                    }
                }
            }

            // Array-like fallback (.length + indexed access).
            Object* al_obj = (items.is_object() || items.is_function())
                ? (items.is_function() ? static_cast<Object*>(items.as_function()) : items.as_object())
                : nullptr;
            double len_d = al_obj ? array_like_length(ctx, al_obj) : 0.0;
            if (ctx.has_exception()) return Value();
            if (len_d > 4294967295.0) { ctx.throw_range_error("Invalid array length"); return Value(); }
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));
            Object* res = make_result(length);
            if (!res) return Value();
            for (uint32_t i = 0; i < length; i++) {
                Value el = al_obj ? al_obj->get_property(std::to_string(i)) : Value();
                if (ctx.has_exception()) return Value();
                if (mapfn) { el = mapfn->call(ctx, {el, Value(static_cast<double>(i))}, thisArg); if (ctx.has_exception()) return Value(); }
                if (!create_data_property_or_throw(ctx, res, std::to_string(i), el)) return Value();
            }
            bool ok = res->set_property("length", Value(static_cast<double>(length)));
            if (!ok) { ctx.throw_type_error("Cannot set length"); return Value(); }
            return Value(res);
        }, 1);
    Function* from_ptr = from_fn.release();
    PropertyAttributes from_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("from", Value(from_ptr), from_attrs);

    auto of_fn = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_binding = array_to_object(ctx);
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
            Value items = args.empty() ? Value() : args[0];
            Value this_arg = args.size() > 2 ? args[2] : Value();

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            // Validate mapfn before anything else (spec step 3b: IsCallable check).
            Function* mapfn = nullptr;
            if (args.size() > 1 && !args[1].is_undefined()) {
                if (!args[1].is_function()) {
                    ctx.throw_type_error("Array.fromAsync mapfn argument must be callable");
                    Value e = ctx.get_exception(); ctx.clear_exception();
                    result_promise->reject(e);
                    return Value(result_promise_obj.release());
                }
                mapfn = args[1].as_function();
            }

            if (items.is_null() || items.is_undefined()) {
                ctx.throw_type_error("Array.fromAsync requires an iterable or array-like object");
                Value e = ctx.get_exception(); ctx.clear_exception();
                result_promise->reject(e);
                return Value(result_promise_obj.release());
            }

            // ToObject: box primitives so we can check for iterator methods and .length.
            Value items_boxed = items;
            if (!items.is_object() && !items.is_function()) {
                Value obj_ctor = ctx.get_binding("Object");
                if (obj_ctor.is_function()) {
                    Value boxed = obj_ctor.as_function()->call(ctx, {items});
                    if (ctx.has_exception()) { Value e = ctx.get_exception(); ctx.clear_exception(); result_promise->reject(e); return Value(result_promise_obj.release()); }
                    if (boxed.is_object() || boxed.is_function()) items_boxed = boxed;
                }
            }

            // Resolve the iterator: prefer @@asyncIterator, then @@iterator
            // (sync iterators are conceptually wrapped via CreateAsyncFromSyncIterator --
            // their yielded values get an extra Await, handled in fa_setup_handlers).
            Value iterator_val;
            Value next_fn_val;
            bool used_async_iterator = false;
            if (items_boxed.is_object() || items_boxed.is_function()) {
                Object* items_obj = items_boxed.is_function()
                    ? static_cast<Object*>(items_boxed.as_function()) : items_boxed.as_object();
                Value iter_method;
                Symbol* async_iter_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
                if (async_iter_sym) {
                    iter_method = items_obj->get_property(async_iter_sym->to_property_key());
                    if (ctx.has_exception()) { Value e = ctx.get_exception(); ctx.clear_exception(); result_promise->reject(e); return Value(result_promise_obj.release()); }
                }
                if (!iter_method.is_undefined() && !iter_method.is_null() && !iter_method.is_function()) {
                    ctx.throw_type_error("@@asyncIterator is not callable");
                    Value e = ctx.get_exception(); ctx.clear_exception();
                    result_promise->reject(e);
                    return Value(result_promise_obj.release());
                }
                if (iter_method.is_function()) {
                    used_async_iterator = true;
                } else {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        iter_method = items_obj->get_property(iter_sym->to_property_key());
                        if (ctx.has_exception()) { Value e = ctx.get_exception(); ctx.clear_exception(); result_promise->reject(e); return Value(result_promise_obj.release()); }
                    }
                    if (!iter_method.is_undefined() && !iter_method.is_null() && !iter_method.is_function()) {
                        ctx.throw_type_error("@@iterator is not callable");
                        Value e = ctx.get_exception(); ctx.clear_exception();
                        result_promise->reject(e);
                        return Value(result_promise_obj.release());
                    }
                }
                if (iter_method.is_function()) {
                    iterator_val = iter_method.as_function()->call(ctx, {}, items_boxed);
                    if (ctx.has_exception()) {
                        Value e = ctx.get_exception(); ctx.clear_exception();
                        result_promise->reject(e);
                        return Value(result_promise_obj.release());
                    }
                    if (!iterator_val.is_object()) {
                        ctx.throw_type_error("Array.fromAsync: iterator must be an object");
                        Value ite = ctx.get_exception(); ctx.clear_exception();
                        result_promise->reject(ite);
                        return Value(result_promise_obj.release());
                    }
                    next_fn_val = iterator_val.as_object()->get_property("next");
                }
            }

            // Use the `this` constructor for the result array if it's a constructor
            // (Array.fromAsync.call(MyClass, ...) should produce instanceof MyClass).
            Object* this_obj2 = ctx.get_this_binding();
            Function* this_ctor = (this_obj2 && this_obj2->is_function())
                ? static_cast<Function*>(this_obj2) : nullptr;
            Object* result_arr;
            if (this_ctor) {
                Value arr_val = this_ctor->construct(ctx, {});
                if (ctx.has_exception()) { Value e = ctx.get_exception(); ctx.clear_exception(); result_promise->reject(e); return Value(result_promise_obj.release()); }
                result_arr = (arr_val.is_object() || arr_val.is_function())
                    ? (arr_val.is_function() ? static_cast<Object*>(arr_val.as_function()) : arr_val.as_object())
                    : ObjectFactory::create_array(0).release();
            } else {
                result_arr = ObjectFactory::create_array(0).release();
            }

            if (next_fn_val.is_function()) {
                result_promise->set_property("__fa_arr__", Value(result_arr));
                result_promise->set_property("__fa_iter__", iterator_val);
                result_promise->set_property("__fa_next__", next_fn_val);
                result_promise->set_property("__fa_mapfn__", mapfn ? Value(mapfn) : Value());
                result_promise->set_property("__fa_thisarg__", this_arg);
                result_promise->set_property("__fa_async__", Value(used_async_iterator));
                result_promise->set_property("__fa_idx__", Value(0.0));
                fa_setup_handlers(ctx, result_promise);
                fa_request_next(ctx, result_promise);
                return Value(result_promise_obj.release());
            }

            // Array-like fallback (no iterator method): spec iterates via .length,
            // Awaiting each element before mapping.
            Object* al_obj = (items_boxed.is_object() || items_boxed.is_function())
                ? (items_boxed.is_function() ? static_cast<Object*>(items_boxed.as_function()) : items_boxed.as_object())
                : nullptr;
            double length_d = al_obj ? array_like_length(ctx, al_obj) : 0.0;
            if (ctx.has_exception()) { Value e = ctx.get_exception(); ctx.clear_exception(); result_promise->reject(e); return Value(result_promise_obj.release()); }
            if (length_d > 4294967295.0) {
                result_promise->reject(Value(std::string("RangeError: Invalid array length")));
                return Value(result_promise_obj.release());
            }
            uint32_t length = static_cast<uint32_t>(length_d < 0 ? 0 : length_d);
            result_promise->set_property("__fa_arr__", Value(result_arr));
            result_promise->set_property("__fa_arraylike__", items_boxed);
            result_promise->set_property("__fa_len__", Value(static_cast<double>(length)));
            result_promise->set_property("__fa_mapfn__", mapfn ? Value(mapfn) : Value());
            result_promise->set_property("__fa_thisarg__", this_arg);
            result_promise->set_property("__fa_async__", Value(true));
            result_promise->set_property("__fa_idx__", Value(0.0));
            fa_setup_handlers(ctx, result_promise);
            fa_request_arraylike_next(ctx, result_promise);
            return Value(result_promise_obj.release());
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
            Object* this_binding = array_to_object(ctx);
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value();

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.find callback must be a function"); return Value();
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast called on non-object")));
                return Value();
            }

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex called on non-object")));
                return Value();
            }

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

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
            if (ctx.has_exception()) return Value();
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double index_arg = args.empty() ? 0.0 : to_integer_or_infinity_throwing(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            double actual_index = index_arg >= 0 ? index_arg : length + index_arg;

            if (actual_index >= length || actual_index < 0) {
                ctx.throw_range_error("Array.prototype.with index out of bounds");
                return Value();
            }
            if (length > 4294967295.0) { ctx.throw_range_error("Invalid array length"); return Value(); }

            Value new_value = args.size() > 1 ? args[1] : Value();

            auto result = ObjectFactory::create_array(static_cast<uint32_t>(length));
            Object* result_obj = result.get();
            for (double i = 0; i < length; i++) {
                Value v = (i == actual_index) ? new_value : this_obj->get_property(Value(i).to_string());
                if (ctx.has_exception()) return Value();
                if (!create_data_property_or_throw(ctx, result_obj, Value(i).to_string(), v)) return Value();
            }

            return Value(result.release());
        }, 2);

    with_fn->set_property("name", Value(std::string("with")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor with_desc(Value(with_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("with", with_desc);

    auto at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.at called on non-object")));
                return Value();
            }

            if (args.empty()) {
                return Value();
            }

            // Spec order: LengthOfArrayLike is captured BEFORE ToIntegerOrInfinity(index),
            // since the latter can run user code (valueOf) that mutates the receiver's length.
            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));
            double idx_d = to_integer_or_infinity_throwing(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            int32_t index = static_cast<int32_t>(std::isnan(idx_d) ? 0.0 : idx_d);

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            Value search_element = args.empty() ? Value() : args[0];

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double from_index = 0;
            if (args.size() > 1) {
                from_index = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
            }

            if (from_index < 0) {
                from_index = std::max(length + from_index, 0.0);
            }

            for (double i = from_index; i < length; i++) {
                Value element = this_obj->get_property(Value(i).to_string());
                if (ctx.has_exception()) return Value();

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double source_len = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double depth = 1.0;
            if (!args.empty() && !args[0].is_undefined()) {
                depth = to_integer_or_infinity_throwing(ctx, args[0]);
                if (ctx.has_exception()) return Value();
                if (depth < 0) depth = 0.0;
            }

            Value result_val = array_species_create(ctx, this_obj, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            double final_index = flatten_into_array(ctx, result, this_obj, source_len, 0, depth);
            if (ctx.has_exception()) return Value();
            bool ok = result->set_property("length", Value(final_index));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            return result_val;
        }, 0);

    PropertyDescriptor flat_length_desc(Value(0.0), PropertyAttributes::Configurable);
    flat_fn->set_property_descriptor("length", flat_length_desc);

    flat_fn->set_property("name", Value(std::string("flat")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor flat_desc(Value(flat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flat", flat_desc);

    auto flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double source_len = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.flatMap mapperFunction must be a function");
                return Value();
            }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            Value result_val = array_species_create(ctx, this_obj, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            double final_index = flatten_into_array(ctx, result, this_obj, source_len, 0, 1, callback, thisArg);
            if (ctx.has_exception()) return Value();
            bool ok = result->set_property("length", Value(final_index));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            return result_val;
        }, 1);

    flatMap_fn->set_property("name", Value(std::string("flatMap")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor flatMap_desc(Value(flatMap_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flatMap", flatMap_desc);

    auto fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
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

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            double start_raw = args.size() > 1 ? to_integer_or_infinity_throwing(ctx, args[1]) : 0.0;
            if (ctx.has_exception()) return Value();
            int32_t start = start_raw < 0
                ? static_cast<int32_t>(std::max(0.0, static_cast<double>(length) + start_raw))
                : static_cast<int32_t>(std::min(start_raw, static_cast<double>(length)));

            double end_raw = args.size() > 2 && !args[2].is_undefined()
                ? to_integer_or_infinity_throwing(ctx, args[2]) : static_cast<double>(length);
            if (ctx.has_exception()) return Value();
            int32_t end = end_raw < 0
                ? static_cast<int32_t>(std::max(0.0, static_cast<double>(length) + end_raw))
                : static_cast<int32_t>(std::min(end_raw, static_cast<double>(length)));

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
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

            // Spec step 3: if join isn't callable, fall back to %Object.prototype.toString%
            // (NOT a hardcoded "[object Object]" -- it must still report e.g. "[object Array]"
            // for an array missing its own join, or "[object Boolean]" for a boxed primitive).
            Value obj_proto = ctx.get_binding("Object");
            if (obj_proto.is_function()) {
                Value proto = static_cast<Object*>(obj_proto.as_function())->get_property("prototype");
                if (proto.is_object()) {
                    Value obj_toString = proto.as_object()->get_property("toString");
                    if (obj_toString.is_function()) {
                        return obj_toString.as_function()->call(ctx, {}, Value(this_obj));
                    }
                }
            }
            return Value(std::string("[object Object]"));
        });
    PropertyDescriptor array_toString_desc(Value(array_toString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toString", array_toString_desc);

    auto array_push_fn = ObjectFactory::create_native_function("push",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.push called on non-object")));
                return Value();
            }

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (length + static_cast<double>(args.size()) > 9007199254740991.0) {
                ctx.throw_type_error("Array.prototype.push: length would exceed 2**53 - 1");
                return Value();
            }

            for (const auto& arg : args) {
                bool ok = this_obj->set_property(Value(length).to_string(), arg);
                if (ctx.has_exception()) return Value();
                if (!ok) {
                    ctx.throw_type_error("Cannot add property, object is not extensible");
                    return Value();
                }
                length++;
            }

            bool ok = this_obj->set_property("length", Value(length));
            if (ctx.has_exception()) return Value();
            if (!ok) {
                ctx.throw_type_error("Cannot set property 'length'");
                return Value();
            }

            return Value(length);
        }, 1);


    PropertyDescriptor push_desc(Value(array_push_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("push", push_desc);

    auto copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            auto relative_to_actual = [&](double rel) {
                return rel < 0 ? std::max(length + rel, 0.0) : std::min(rel, length);
            };

            double target_arg = args.empty() ? 0.0 : to_integer_or_infinity_throwing(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            double target = relative_to_actual(target_arg);

            double start_arg = args.size() > 1 ? to_integer_or_infinity_throwing(ctx, args[1]) : 0.0;
            if (ctx.has_exception()) return Value();
            double start = relative_to_actual(start_arg);

            double end_arg = (args.size() > 2 && !args[2].is_undefined())
                ? to_integer_or_infinity_throwing(ctx, args[2]) : length;
            if (ctx.has_exception()) return Value();
            double end = relative_to_actual(end_arg);

            double count = std::min(end - start, length - target);

            auto copy_one = [&](double from, double to) -> bool {
                std::string from_key = Value(from).to_string();
                std::string to_key = Value(to).to_string();
                bool present = this_obj->has_property(from_key);
                if (ctx.has_exception()) return false;
                if (present) {
                    Value v = this_obj->get_property(from_key);
                    if (ctx.has_exception()) return false;
                    bool ok = this_obj->set_property(to_key, v);
                    if (ctx.has_exception()) return false;
                    if (!ok) { ctx.throw_type_error("Cannot set property '" + to_key + "'"); return false; }
                } else {
                    bool ok = this_obj->delete_property(to_key);
                    if (ctx.has_exception()) return false;
                    if (!ok) { ctx.throw_type_error("Cannot delete property '" + to_key + "'"); return false; }
                }
                return true;
            };

            if (count > 0) {
                if (start < target && target < start + count) {
                    for (double i = count - 1; i >= 0; i--) {
                        if (!copy_one(start + i, target + i)) return Value();
                    }
                } else {
                    for (double i = 0; i < count; i++) {
                        if (!copy_one(start + i, target + i)) return Value();
                    }
                }
            }

            return Value(this_obj);
        }, 2);

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                return Value(-1.0);
            }

            Value searchElement = args.empty() ? Value() : args[0];
            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (length == 0) {
                return Value(-1.0);
            }

            double fromIndex = length - 1;
            if (args.size() > 1) {
                double int_from = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                if (int_from < 0) {
                    fromIndex = length + int_from;
                } else {
                    fromIndex = std::min(int_from, length - 1);
                }
                if (fromIndex == 0) fromIndex = 0; // normalize -0 to +0
            }

            for (double i = fromIndex; i >= 0; i--) {
                std::string key = Value(i).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (!present) continue;
                Value element = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                if (element.strict_equals(searchElement)) {
                    return Value(i);
                }
            }

            return Value(-1.0);
        }, 1);

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.reduceRight callback must be a function");
                return Value();
            }
            Function* callback_func = args[0].as_function();

            if (length == 0 && args.size() < 2) {
                ctx.throw_type_error("Reduce of empty array with no initial value");
                return Value();
            }

            double k = length - 1;
            Value accumulator;

            if (args.size() >= 2) {
                accumulator = args[1];
            } else {
                bool found = false;
                for (; k >= 0; k--) {
                    std::string key = Value(k).to_string();
                    bool present = this_obj->has_property(key);
                    if (ctx.has_exception()) return Value();
                    if (present) {
                        accumulator = this_obj->get_property(key);
                        if (ctx.has_exception()) return Value();
                        k--;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
            }

            for (; k >= 0; k--) {
                std::string key = Value(k).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (!present) continue;
                Value element = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                std::vector<Value> callback_args = { accumulator, element, Value(k), Value(this_obj) };
                accumulator = callback_func->call(ctx, callback_args, Value());
                if (ctx.has_exception()) return Value();
            }

            return accumulator;
        }, 1);

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(std::string(""));

            uint32_t length = this_obj->get_length();
            if (ctx.has_exception()) return Value();
            std::string result;

            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) {
                    result += ",";
                }

                Value element = this_obj->get_element(i);

                if (!element.is_null() && !element.is_undefined()) {
                    // Invoke(element, "toLocaleString") per spec.
                    if (element.is_object() || element.is_function()) {
                        Object* elem_obj = element.is_function()
                            ? static_cast<Object*>(element.as_function()) : element.as_object();
                        if (elem_obj->has_property("toLocaleString")) {
                            Value fn_val = elem_obj->get_property("toLocaleString");
                            if (fn_val.is_function()) {
                                Value str_val = fn_val.as_function()->call(ctx, {}, element);
                                if (ctx.has_exception()) return Value();
                                result += str_val.to_string();
                                continue;
                            }
                        }
                    } else if (element.is_boolean()) {
                        // Box boolean to call its prototype chain toLocaleString with primitive this.
                        Value bool_proto_tls = ctx.get_binding("Boolean");
                        if (bool_proto_tls.is_function()) {
                            Value proto = static_cast<Object*>(bool_proto_tls.as_function())->get_property("prototype");
                            if (proto.is_object()) {
                                Value fn_val = proto.as_object()->get_property("toLocaleString");
                                if (fn_val.is_function()) {
                                    Value str_val = fn_val.as_function()->call(ctx, {}, element);
                                    if (ctx.has_exception()) return Value();
                                    result += str_val.to_string();
                                    continue;
                                }
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            if (ctx.has_exception()) return Value();
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }

            Function* compareFn = nullptr;
            if (!args.empty() && !args[0].is_undefined()) {
                if (!args[0].is_function()) {
                    ctx.throw_type_error("Array.prototype.toSorted: comparefn must be a function or undefined");
                    return Value();
                }
                compareFn = args[0].as_function();
            }

            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            if (length > 4294967295.0) { ctx.throw_range_error("Invalid array length"); return Value(); }

            auto result = ObjectFactory::create_array(static_cast<uint32_t>(length));

            // read-through-holes: Get for every index, no HasProperty check.
            std::vector<Value> items;
            items.reserve(static_cast<size_t>(length));
            for (double i = 0; i < length; i++) {
                items.push_back(this_obj->get_property(Value(i).to_string()));
                if (ctx.has_exception()) return Value();
            }

            auto compare = [&](const Value& a, const Value& b) -> int {
                if (a.is_undefined() && b.is_undefined()) return 0;
                if (a.is_undefined()) return 1;
                if (b.is_undefined()) return -1;
                if (compareFn) {
                    Value cmp_result = compareFn->call(ctx, {a, b});
                    if (ctx.has_exception()) return 0;
                    double cmp = cmp_result.to_number();
                    if (std::isnan(cmp)) return 0;
                    return cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
                }
                std::string str_a = sort_default_to_string(ctx, a);
                if (ctx.has_exception()) return 0;
                std::string str_b = sort_default_to_string(ctx, b);
                if (ctx.has_exception()) return 0;
                return str_a.compare(str_b);
            };
            std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
                return compare(a, b) < 0;
            });
            if (ctx.has_exception()) return Value();

            for (size_t j = 0; j < items.size(); j++) {
                if (!create_data_property_or_throw(ctx, result.get(), std::to_string(j), items[j])) return Value();
            }

            return Value(result.release());
        }, 1);
    PropertyDescriptor toSorted_desc(Value(toSorted_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSorted", toSorted_desc);

    auto toSpliced_fn = ObjectFactory::create_native_function("toSpliced",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double start_arg = args.empty() ? 0.0 : to_integer_or_infinity_throwing(ctx, args[0]);
            if (ctx.has_exception()) return Value();
            double actual_start = start_arg < 0 ? std::max(length + start_arg, 0.0) : std::min(start_arg, length);

            double item_count = args.size() > 2 ? static_cast<double>(args.size() - 2) : 0;
            double actual_delete_count;
            if (args.empty()) {
                actual_delete_count = 0;
            } else if (args.size() == 1) {
                actual_delete_count = length - actual_start;
            } else {
                double dc = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                actual_delete_count = std::min(std::max(dc, 0.0), length - actual_start);
            }

            double new_len = length + item_count - actual_delete_count;
            if (new_len > 9007199254740991.0) {
                ctx.throw_type_error("Array.prototype.toSpliced: resulting length would exceed 2**53 - 1");
                return Value();
            }
            if (new_len > 4294967295.0) { ctx.throw_range_error("Invalid array length"); return Value(); }

            auto result = ObjectFactory::create_array(static_cast<uint32_t>(new_len));
            Object* result_obj = result.get();

            double i = 0;
            double r = actual_start;
            for (; i < actual_start; i++) {
                std::string key = Value(i).to_string();
                Value v = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                if (!create_data_property_or_throw(ctx, result_obj, key, v)) return Value();
            }
            for (size_t arg_i = 2; arg_i < args.size(); arg_i++) {
                if (!create_data_property_or_throw(ctx, result_obj, Value(i).to_string(), args[arg_i])) return Value();
                i++;
            }
            r += actual_delete_count;
            for (; i < new_len; i++, r++) {
                Value v = this_obj->get_property(Value(r).to_string());
                if (ctx.has_exception()) return Value();
                if (!create_data_property_or_throw(ctx, result_obj, Value(i).to_string(), v)) return Value();
            }

            return Value(result.release());
        }, 2);
    PropertyDescriptor toSpliced_desc(Value(toSpliced_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSpliced", toSpliced_desc);

    auto array_concat_fn = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array.prototype.concat called on null or undefined"); return Value(); }
            Object* this_array = array_to_object(ctx);

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_array, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            auto is_spreadable = [&ctx](Object* obj) -> bool {
                Value sv = obj->get_property("Symbol.isConcatSpreadable");
                if (ctx.has_exception()) return false;
                if (!sv.is_undefined()) return sv.to_boolean();
                Object* target = obj;
                while (target && target->get_type() == Object::ObjectType::Proxy) {
                    Proxy* p = static_cast<Proxy*>(target);
                    if (p->is_revoked()) { ctx.throw_type_error("Cannot perform operation on a revoked proxy"); return false; }
                    target = p->get_proxy_target();
                }
                return target && target->is_array();
            };

            double n = 0;
            auto spread_into = [&](Object* obj) -> bool {
                double obj_length = array_like_length(ctx, obj);
                if (ctx.has_exception()) return false;
                for (double i = 0; i < obj_length; i++) {
                    std::string key = Value(i).to_string();
                    bool present = obj->has_property(key);
                    if (ctx.has_exception()) return false;
                    if (present) {
                        Value elem = obj->get_property(key);
                        if (ctx.has_exception()) return false;
                        if (!create_data_property_or_throw(ctx, result, Value(n).to_string(), elem)) return false;
                    }
                    n++;
                }
                return true;
            };

            bool this_spreadable = is_spreadable(this_array);
            if (ctx.has_exception()) return Value();
            if (this_spreadable) {
                if (!spread_into(this_array)) return Value();
            } else {
                if (!create_data_property_or_throw(ctx, result, Value(n).to_string(), Value(this_array))) return Value();
                n++;
            }

            for (const auto& arg : args) {
                if (arg.is_object() || arg.is_function()) {
                    Object* arg_obj = arg.is_function()
                        ? static_cast<Object*>(arg.as_function())
                        : arg.as_object();
                    bool arg_spreadable = is_spreadable(arg_obj);
                    if (ctx.has_exception()) return Value();
                    if (arg_spreadable) {
                        if (!spread_into(arg_obj)) return Value();
                        continue;
                    }
                }
                if (!create_data_property_or_throw(ctx, result, Value(n).to_string(), arg)) return Value();
                n++;
            }

            bool ok = result->set_property("length", Value(n));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
            return result_val;
        }, 1);
    PropertyDescriptor concat_desc(Value(array_concat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("concat", concat_desc);

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(false);

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.every callback must be a function"); return Value();
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            for (uint32_t i = 0; i < length; i++) {
                if (!this_obj->has_property(std::to_string(i))) continue;
                Value element = this_obj->get_property(std::to_string(i));
                if (ctx.has_exception()) return Value();
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (!result.to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    PropertyDescriptor every_desc(Value(every_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("every", every_desc);

    auto filter_fn = ObjectFactory::create_native_function("filter",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.filter callback must be a function");
                return Value();
            }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, 0);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            double to = 0;
            for (double k = 0; k < length; k++) {
                std::string key = Value(k).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (!present) continue;
                Value element = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                std::vector<Value> callback_args = { element, Value(k), Value(this_obj) };
                Value test_result = callback->call(ctx, callback_args, thisArg);
                if (ctx.has_exception()) return Value();
                if (test_result.to_boolean()) {
                    if (!create_data_property_or_throw(ctx, result, Value(to).to_string(), element)) return Value();
                    to++;
                }
            }
            bool ok = result->set_property("length", Value(to));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
            return result_val;
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("filter", filter_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value();

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.forEach callback must be a function");
                return Value();
            }

            Function* callback = args[0].as_function();
            Value this_arg = args.size() > 1 ? args[1] : Value();

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(-1.0);

            Value search_element = args.empty() ? Value() : args[0];

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            if (length == 0) return Value(-1.0);

            double start_index = 0;
            if (args.size() > 1) {
                double from_index = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                if (from_index >= length) return Value(-1.0);
                start_index = from_index < 0 ? std::max(length + from_index, 0.0) : from_index;
                if (start_index == 0) start_index = 0; // normalize -0 to +0
            }

            for (double i = start_index; i < length; i++) {
                std::string key = Value(i).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (!present) continue;
                Value element = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                if (element.strict_equals(search_element)) {
                    return Value(i);
                }
            }
            return Value(-1.0);
        }, 1);
    PropertyDescriptor array_indexOf_desc(Value(indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("indexOf", array_indexOf_desc);

    auto map_fn = ObjectFactory::create_native_function("map",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.map callback must be a function");
                return Value();
            }
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, length);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            for (double k = 0; k < length; k++) {
                std::string key = Value(k).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (present) {
                    Value element = this_obj->get_property(key);
                    if (ctx.has_exception()) return Value();
                    std::vector<Value> callback_args = { element, Value(k), Value(this_obj) };
                    Value mapped = callback->call(ctx, callback_args, thisArg);
                    if (ctx.has_exception()) return Value();
                    if (!create_data_property_or_throw(ctx, result, key, mapped)) return Value();
                }
            }
            return result_val;
        }, 1);
    PropertyDescriptor map_desc(Value(map_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("map", map_desc);

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.reduce callback must be a function");
                return Value();
            }
            Function* callback = args[0].as_function();

            if (length == 0 && args.size() < 2) {
                ctx.throw_type_error("Reduce of empty array with no initial value");
                return Value();
            }

            double k = 0;
            Value accumulator;

            if (args.size() > 1) {
                accumulator = args[1];
            } else {
                bool found = false;
                for (; k < length; k++) {
                    std::string key = Value(k).to_string();
                    bool present = this_obj->has_property(key);
                    if (ctx.has_exception()) return Value();
                    if (present) {
                        accumulator = this_obj->get_property(key);
                        if (ctx.has_exception()) return Value();
                        k++;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
            }

            for (; k < length; k++) {
                std::string key = Value(k).to_string();
                bool present = this_obj->has_property(key);
                if (ctx.has_exception()) return Value();
                if (!present) continue;
                Value element = this_obj->get_property(key);
                if (ctx.has_exception()) return Value();
                std::vector<Value> callback_args = { accumulator, element, Value(k), Value(this_obj) };
                accumulator = callback->call(ctx, callback_args);
                if (ctx.has_exception()) return Value();
            }

            return accumulator;
        }, 1);
    PropertyDescriptor reduce_desc(Value(reduce_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reduce", reduce_desc);

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(false);

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.some callback must be a function"); return Value();
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(-1.0);

            double len_d = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            uint32_t length = static_cast<uint32_t>(len_d > 4294967295.0 ? 4294967295.0 : (len_d < 0 ? 0 : len_d));

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Array.prototype.findIndex callback must be a function"); return Value();
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(std::string(""));

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            std::string separator = ",";
            if (!args.empty() && !args[0].is_undefined()) {
                separator = sort_default_to_string(ctx, args[0]);
                if (ctx.has_exception()) return Value();
            }
            std::string result = "";

            for (double i = 0; i < length; i++) {
                if (i > 0) result += separator;
                Value element = this_obj->get_property(Value(i).to_string());
                if (ctx.has_exception()) return Value();
                if (!element.is_undefined() && !element.is_null()) {
                    result += sort_default_to_string(ctx, element);
                    if (ctx.has_exception()) return Value();
                }
            }
            return Value(result);
        }, 1);
    PropertyDescriptor join_desc(Value(join_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("join", join_desc);

    auto pop_fn = ObjectFactory::create_native_function("pop",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value();

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (length == 0) {
                bool ok = this_obj->set_property("length", Value(0.0));
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
                return Value();
            }

            double new_length = length - 1;
            std::string idx = Value(new_length).to_string();
            Value element = this_obj->get_property(idx);
            if (ctx.has_exception()) return Value();

            bool deleted = this_obj->delete_property(idx);
            if (ctx.has_exception()) return Value();
            if (!deleted) { ctx.throw_type_error("Cannot delete property '" + idx + "'"); return Value(); }

            bool ok = this_obj->set_property("length", Value(new_length));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            return element;
        }, 0);
    PropertyDescriptor pop_desc(Value(pop_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("pop", pop_desc);

    auto reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
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
            if (ctx.has_exception()) return Value();
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value();

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            if (length == 0) {
                bool ok = this_obj->set_property("length", Value(0.0));
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
                return Value();
            }

            Value first = this_obj->get_property("0");
            if (ctx.has_exception()) return Value();

            for (double i = 1; i < length; i++) {
                std::string from_key = Value(i).to_string();
                std::string to_key = Value(i - 1).to_string();
                if (this_obj->has_property(from_key)) {
                    Value v = this_obj->get_property(from_key);
                    if (ctx.has_exception()) return Value();
                    bool ok = this_obj->set_property(to_key, v);
                    if (ctx.has_exception()) return Value();
                    if (!ok) { ctx.throw_type_error("Cannot set property '" + to_key + "'"); return Value(); }
                } else {
                    bool deleted = this_obj->delete_property(to_key);
                    if (ctx.has_exception()) return Value();
                    if (!deleted) { ctx.throw_type_error("Cannot delete property '" + to_key + "'"); return Value(); }
                }
            }

            std::string last_key = Value(length - 1).to_string();
            bool deleted = this_obj->delete_property(last_key);
            if (ctx.has_exception()) return Value();
            if (!deleted) { ctx.throw_type_error("Cannot delete property '" + last_key + "'"); return Value(); }

            bool ok = this_obj->set_property("length", Value(length - 1));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            return first;
        }, 0);
    PropertyDescriptor shift_desc(Value(shift_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("shift", shift_desc);

    auto slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) {
                auto empty = ObjectFactory::create_array();
                return Value(empty.release());
            }

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double start = 0;
            if (!args.empty()) {
                start = to_integer_or_infinity_throwing(ctx, args[0]);
                if (ctx.has_exception()) return Value();
            }
            start = start < 0 ? std::max(length + start, 0.0) : std::min(start, length);

            double end = length;
            if (args.size() >= 2 && !args[1].is_undefined()) {
                end = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
            }
            end = end < 0 ? std::max(length + end, 0.0) : std::min(end, length);

            double count = end > start ? end - start : 0;
            // ES6: use @@species constructor for result
            Value result_val = array_species_create(ctx, this_obj, count);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            double n = 0;
            for (double k = start; k < end; k++) {
                std::string pk = Value(k).to_string();
                bool present = this_obj->has_property(pk);
                if (ctx.has_exception()) return Value();
                if (present) {
                    Value elem = this_obj->get_property(pk);
                    if (ctx.has_exception()) return Value();
                    if (!create_data_property_or_throw(ctx, result, Value(n).to_string(), elem)) return Value();
                }
                n++;
            }
            bool ok = result->set_property("length", Value(n));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
            return result_val;
        }, 2);
    PropertyDescriptor slice_desc(Value(slice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("slice", slice_desc);

    auto sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(this_obj);

            Function* compareFn = nullptr;
            if (!args.empty() && !args[0].is_undefined()) {
                if (!args[0].is_function()) {
                    ctx.throw_type_error("Array.prototype.sort: compareFn must be a function or undefined");
                    return Value();
                }
                compareFn = args[0].as_function();
            }

            uint32_t length = this_obj->get_length();
            if (ctx.has_exception()) return Value();
            if (length <= 1) return Value(this_obj);

            auto compare = [&](const Value& a, const Value& b) -> int {
                if (a.is_undefined() && b.is_undefined()) return 0;
                if (a.is_undefined()) return 1;
                if (b.is_undefined()) return -1;

                if (compareFn) {
                    std::vector<Value> compare_args = { a, b };
                    Value result = compareFn->call(ctx, compare_args);
                    if (ctx.has_exception()) return 0;
                    double cmp = result.to_number();
                    if (std::isnan(cmp)) return 0;
                    return cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
                } else {
                    std::string str_a = sort_default_to_string(ctx, a);
                    if (ctx.has_exception()) return 0;
                    std::string str_b = sort_default_to_string(ctx, b);
                    if (ctx.has_exception()) return 0;
                    return str_a.compare(str_b);
                }
            };

            // Spec: read each present index once (holes get no [[Get]] at all), sort the
            // snapshot in memory, then [[Set]] the sorted items back and [[Delete]] the rest.
            std::vector<Value> items;
            items.reserve(length);
            for (uint32_t i = 0; i < length; i++) {
                if (this_obj->has_property(std::to_string(i))) {
                    items.push_back(this_obj->get_property(std::to_string(i)));
                    if (ctx.has_exception()) return Value();
                }
            }

            std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
                return compare(a, b) < 0;
            });
            if (ctx.has_exception()) return Value();

            uint32_t item_count = static_cast<uint32_t>(items.size());
            for (uint32_t j = 0; j < item_count; j++) {
                this_obj->set_property(std::to_string(j), items[j]);
                if (ctx.has_exception()) return Value();
            }
            for (uint32_t j = item_count; j < length; j++) {
                this_obj->delete_property(std::to_string(j));
                if (ctx.has_exception()) return Value();
            }

            return Value(this_obj);
        }, 1);
    PropertyDescriptor sort_desc(Value(sort_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("sort", sort_desc);

    auto splice_fn = ObjectFactory::create_native_function("splice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();

            double start = 0;
            if (!args.empty()) {
                start = to_integer_or_infinity_throwing(ctx, args[0]);
                if (ctx.has_exception()) return Value();
            }
            start = start < 0 ? std::max(length + start, 0.0) : std::min(start, length);

            double item_count = args.size() > 2 ? static_cast<double>(args.size() - 2) : 0;
            double delete_count;
            if (args.empty()) {
                delete_count = 0;
            } else if (args.size() == 1) {
                delete_count = length - start;
            } else {
                double dc = to_integer_or_infinity_throwing(ctx, args[1]);
                if (ctx.has_exception()) return Value();
                delete_count = std::min(std::max(dc, 0.0), length - start);
            }

            if (length + item_count - delete_count > 9007199254740991.0) {
                ctx.throw_type_error("Array.prototype.splice: resulting length would exceed 2**53 - 1");
                return Value();
            }

            Value result_val = array_species_create(ctx, this_obj, delete_count);
            if (ctx.has_exception()) return Value();
            Object* result = result_val.is_object() ? result_val.as_object()
                           : result_val.is_function() ? static_cast<Object*>(result_val.as_function())
                           : nullptr;
            if (!result) { ctx.throw_type_error("Species constructor did not return an object"); return Value(); }

            for (double k = 0; k < delete_count; k++) {
                std::string from_key = Value(start + k).to_string();
                bool present = this_obj->has_property(from_key);
                if (ctx.has_exception()) return Value();
                if (present) {
                    Value v = this_obj->get_property(from_key);
                    if (ctx.has_exception()) return Value();
                    if (!create_data_property_or_throw(ctx, result, Value(k).to_string(), v)) return Value();
                }
            }
            bool len_ok = result->set_property("length", Value(delete_count));
            if (ctx.has_exception()) return Value();
            if (!len_ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            std::vector<Value> items_to_insert(args.begin() + (args.size() > 2 ? 2 : args.size()), args.end());

            if (item_count < delete_count) {
                for (double k = start; k < length - delete_count; k++) {
                    std::string from_key = Value(k + delete_count).to_string();
                    std::string to_key = Value(k + item_count).to_string();
                    bool present = this_obj->has_property(from_key);
                    if (ctx.has_exception()) return Value();
                    if (present) {
                        Value v = this_obj->get_property(from_key);
                        if (ctx.has_exception()) return Value();
                        bool ok = this_obj->set_property(to_key, v);
                        if (ctx.has_exception()) return Value();
                        if (!ok) { ctx.throw_type_error("Cannot set property '" + to_key + "'"); return Value(); }
                    } else {
                        bool ok = this_obj->delete_property(to_key);
                        if (ctx.has_exception()) return Value();
                        if (!ok) { ctx.throw_type_error("Cannot delete property '" + to_key + "'"); return Value(); }
                    }
                }
                for (double k = length; k > length - delete_count + item_count; k--) {
                    bool ok = this_obj->delete_property(Value(k - 1).to_string());
                    if (ctx.has_exception()) return Value();
                    if (!ok) { ctx.throw_type_error("Cannot delete property"); return Value(); }
                }
            } else if (item_count > delete_count) {
                for (double k = length - delete_count; k > start; k--) {
                    std::string from_key = Value(k + delete_count - 1).to_string();
                    std::string to_key = Value(k + item_count - 1).to_string();
                    bool present = this_obj->has_property(from_key);
                    if (ctx.has_exception()) return Value();
                    if (present) {
                        Value v = this_obj->get_property(from_key);
                        if (ctx.has_exception()) return Value();
                        bool ok = this_obj->set_property(to_key, v);
                        if (ctx.has_exception()) return Value();
                        if (!ok) { ctx.throw_type_error("Cannot set property '" + to_key + "'"); return Value(); }
                    } else {
                        bool ok = this_obj->delete_property(to_key);
                        if (ctx.has_exception()) return Value();
                        if (!ok) { ctx.throw_type_error("Cannot delete property '" + to_key + "'"); return Value(); }
                    }
                }
            }

            for (size_t i = 0; i < items_to_insert.size(); i++) {
                bool ok = this_obj->set_property(Value(start + static_cast<double>(i)).to_string(), items_to_insert[i]);
                if (ctx.has_exception()) return Value();
                if (!ok) { ctx.throw_type_error("Cannot set property"); return Value(); }
            }

            bool final_ok = this_obj->set_property("length", Value(length - delete_count + item_count));
            if (ctx.has_exception()) return Value();
            if (!final_ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }

            return result_val;
        }, 2);
    PropertyDescriptor splice_desc(Value(splice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("splice", splice_desc);

    auto unshift_fn = ObjectFactory::create_native_function("unshift",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value(0.0);

            double length = array_like_length(ctx, this_obj);
            if (ctx.has_exception()) return Value();
            double argCount = static_cast<double>(args.size());

            if (argCount > 0) {
                if (length + argCount > 9007199254740991.0) {
                    ctx.throw_type_error("Array.prototype.unshift: length would exceed 2**53 - 1");
                    return Value();
                }

                for (double i = length - 1; i >= 0; i--) {
                    std::string from_key = Value(i).to_string();
                    std::string to_key = Value(i + argCount).to_string();
                    if (this_obj->has_property(from_key)) {
                        Value v = this_obj->get_property(from_key);
                        if (ctx.has_exception()) return Value();
                        bool ok = this_obj->set_property(to_key, v);
                        if (ctx.has_exception()) return Value();
                        if (!ok) { ctx.throw_type_error("Cannot set property '" + to_key + "'"); return Value(); }
                    } else {
                        bool deleted = this_obj->delete_property(to_key);
                        if (ctx.has_exception()) return Value();
                        if (!deleted) { ctx.throw_type_error("Cannot delete property '" + to_key + "'"); return Value(); }
                    }
                }

                for (size_t i = 0; i < args.size(); i++) {
                    bool ok = this_obj->set_property(Value(static_cast<double>(i)).to_string(), args[i]);
                    if (ctx.has_exception()) return Value();
                    if (!ok) { ctx.throw_type_error("Cannot add property, object is not extensible"); return Value(); }
                }
            }

            double new_length = length + argCount;
            bool ok = this_obj->set_property("length", Value(new_length));
            if (ctx.has_exception()) return Value();
            if (!ok) { ctx.throw_type_error("Cannot set property 'length'"); return Value(); }
            return Value(new_length);
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
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            if (ctx.original_this_was_nullish()) { ctx.throw_type_error("Array method called on null or undefined"); return Value(); }
            Object* this_obj = array_to_object(ctx);
            if (!this_obj) return Value();
            auto iterator = ObjectFactory::create_object();
            struct ArrIterState { Object* arr; uint32_t index = 0; };
            auto state = std::make_shared<ArrIterState>(ArrIterState{this_obj, 0});
            auto next_fn = ObjectFactory::create_native_function("next",
                [state](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    uint32_t length = state->arr->get_length();
                    auto result = ObjectFactory::create_object();
                    if (state->index >= length) {
                        result->set_property("done", Value(true));
                        result->set_property("value", Value());
                    } else {
                        result->set_property("done", Value(false));
                        result->set_property("value", state->arr->get_element(state->index));
                        state->index++;
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

    all_array_intrinsics().insert(array_constructor.get());
    ctx.register_built_in_object("Array", array_constructor.release());
}

} // namespace Quanta
