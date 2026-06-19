/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/IteratorBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include <cmath>

namespace Quanta {

// Steps an iterator using an already-resolved next method, not a re-fetched one -- avoids
// re-triggering a `next` getter with side effects on every step.
static std::pair<Value, bool> iterator_helper_step(Context& ctx, const Value& iter_val, const Value& next_method) {
    if (!next_method.is_function()) {
        ctx.throw_type_error("Iterator helper's underlying next is not a function");
        return {Value(), true};
    }
    Value result = next_method.as_function()->call(ctx, {}, iter_val);
    if (ctx.has_exception()) return {Value(), true};
    if (!result.is_object()) {
        ctx.throw_type_error("Iterator result is not an object");
        return {Value(), true};
    }
    bool done = result.as_object()->get_property("done").to_boolean();
    Value val = done ? Value() : result.as_object()->get_property("value");
    return {val, done};
}

// IteratorClose: calls iter.return() if present, preserving any already-pending exception.
static void iterator_helper_close(Context& ctx, const Value& iter_val) {
    if (!iter_val.is_object() && !iter_val.is_function()) return;
    Object* obj = iter_val.is_function() ? static_cast<Object*>(iter_val.as_function()) : iter_val.as_object();
    Value ret_fn = obj->get_property("return");
    if (!ret_fn.is_function()) return;
    bool had_exception = ctx.has_exception();
    Value pending = had_exception ? ctx.get_exception() : Value();
    if (had_exception) ctx.clear_exception();
    ret_fn.as_function()->call(ctx, {}, iter_val);
    if (had_exception) {
        ctx.clear_exception();
        ctx.throw_exception(pending, true);
    }
}

static Object* make_iter_result(const Value& value, bool done) {
    auto result = ObjectFactory::create_object();
    result->set_property("value", done ? Value() : value);
    result->set_property("done", Value(done));
    return result.release();
}

// Scaffolding shared by all Iterator Helpers: stores iter/next as own properties (so the GC
// reaches them through the object graph instead of a captured raw pointer), inherits
// Symbol.iterator from iterator_proto, and wires up `return`. Callers add their own `next`.
static Object* create_iterator_helper_base(Object* iterator_proto, const Value& iter_val, const Value& next_method) {
    auto helper = ObjectFactory::create_object();
    helper->set_prototype(iterator_proto);
    helper->set_property("__ih_iter__", iter_val);
    helper->set_property("__ih_next__", next_method);

    auto return_fn = ObjectFactory::create_native_function("return",
        [](Context& ctx, const std::vector<Value>&) -> Value {
            Object* self = ctx.get_this_binding();
            if (self) {
                Value inner_val = self->get_property("__ih_inner__");
                if (inner_val.is_object() || inner_val.is_function()) iterator_helper_close(ctx, inner_val);
                iterator_helper_close(ctx, self->get_property("__ih_iter__"));
            }
            return Value(make_iter_result(Value(), true));
        }, 0);
    helper->set_property("return", Value(return_fn.release()));

    return helper.release();
}

// Closes all alive columns except `skip`, for early abandonment (one column threw, or the zip ended).
static void iterator_zip_close_others(Context& ctx, Object* iters_arr, Object* alive_arr, uint32_t count, uint32_t skip) {
    for (uint32_t j = count; j-- > 0;) {
        if (j == skip) continue;
        if (alive_arr->get_property(std::to_string(j)).to_boolean())
            iterator_helper_close(ctx, iters_arr->get_property(std::to_string(j)));
    }
}

// Shared next() for zip/zipKeyed: steps every alive column, padding exhausted ones in "longest"
// mode, packaging the row as an array or (zipKeyed) a null-prototype keyed object.
static Value iterator_zip_step(Context& ctx, const std::vector<Value>&) {
    Object* self = ctx.get_this_binding();
    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
    if (self->get_property("__iz_done__").to_boolean()) return Value(make_iter_result(Value(), true));

    uint32_t count = (uint32_t)self->get_property("__iz_count__").to_number();
    std::string mode = self->get_property("__iz_mode__").to_string();
    bool keyed = self->get_property("__iz_keyed__").to_boolean();
    Object* iters_arr = self->get_property("__iz_iters__").as_object();
    Object* nexts_arr = self->get_property("__iz_nexts__").as_object();
    Object* padding_arr = self->get_property("__iz_padding__").as_object();
    Object* alive_arr = self->get_property("__iz_alive__").as_object();
    Object* keys_arr = keyed ? self->get_property("__iz_keys__").as_object() : nullptr;

    if (count == 0) { self->set_property("__iz_done__", Value(true)); return Value(make_iter_result(Value(), true)); }

    auto results = keyed ? ObjectFactory::create_object() : ObjectFactory::create_array();
    if (keyed) results->set_prototype(nullptr);

    for (uint32_t i = 0; i < count; i++) {
        std::string out_key = keyed ? keys_arr->get_property(std::to_string(i)).to_string() : std::to_string(i);
        bool alive = alive_arr->get_property(std::to_string(i)).to_boolean();
        if (!alive) {
            results->set_property(out_key, padding_arr->get_property(std::to_string(i)));
            continue;
        }
        Value iter_val = iters_arr->get_property(std::to_string(i));
        Value next_method = nexts_arr->get_property(std::to_string(i));
        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
        if (ctx.has_exception()) {
            self->set_property("__iz_done__", Value(true));
            iterator_zip_close_others(ctx, iters_arr, alive_arr, count, i);
            return Value();
        }
        if (done) {
            if (mode == "shortest") {
                self->set_property("__iz_done__", Value(true));
                iterator_zip_close_others(ctx, iters_arr, alive_arr, count, i);
                return Value(make_iter_result(Value(), true));
            } else if (mode == "strict") {
                self->set_property("__iz_done__", Value(true));
                if (i != 0) {
                    iterator_zip_close_others(ctx, iters_arr, alive_arr, count, i);
                    ctx.throw_type_error("Iterator.zip: iterables are not the same length (strict mode)");
                    return Value();
                }
                bool mismatch = false;
                for (uint32_t j = 1; j < count; j++) {
                    if (!alive_arr->get_property(std::to_string(j)).to_boolean()) continue;
                    auto [jval, jdone] = iterator_helper_step(ctx, iters_arr->get_property(std::to_string(j)), nexts_arr->get_property(std::to_string(j)));
                    (void)jval;
                    if (ctx.has_exception()) {
                        iterator_zip_close_others(ctx, iters_arr, alive_arr, count, j);
                        return Value();
                    }
                    if (!jdone) mismatch = true;
                }
                if (mismatch) { ctx.throw_type_error("Iterator.zip: iterables are not the same length (strict mode)"); return Value(); }
                return Value(make_iter_result(Value(), true));
            } else { // longest
                alive_arr->set_property(std::to_string(i), Value(false));
                results->set_property(out_key, padding_arr->get_property(std::to_string(i)));
                continue;
            }
        }
        results->set_property(out_key, val);
    }

    if (mode == "longest") {
        bool all_dead = true;
        for (uint32_t i = 0; i < count; i++) if (alive_arr->get_property(std::to_string(i)).to_boolean()) { all_dead = false; break; }
        if (all_dead) { self->set_property("__iz_done__", Value(true)); return Value(make_iter_result(Value(), true)); }
    }

    if (!keyed) results->set_length(count);
    return Value(make_iter_result(Value(results.release()), false));
}

static Value iterator_zip_return(Context& ctx, const std::vector<Value>&) {
    Object* self = ctx.get_this_binding();
    if (self && !self->get_property("__iz_done__").to_boolean()) {
        self->set_property("__iz_done__", Value(true));
        uint32_t count = (uint32_t)self->get_property("__iz_count__").to_number();
        Object* iters_arr = self->get_property("__iz_iters__").as_object();
        Object* alive_arr = self->get_property("__iz_alive__").as_object();
        iterator_zip_close_others(ctx, iters_arr, alive_arr, count, count);
    }
    return Value(make_iter_result(Value(), true));
}

void register_iterator_helpers(Context& ctx) {
    // Add ES2025 Iterator Helpers to %IteratorPrototype%
    if (Iterator::s_iterator_prototype_) {
        Object* iter_proto_obj = Iterator::s_iterator_prototype_;

        auto call_iter_next = [](Context& ctx, Object* iter) -> std::pair<Value,bool> {
            Value nxt = iter->get_property("next");
            if (!nxt.is_function()) return {Value(), true};
            Value res = nxt.as_function()->call(ctx, {}, Value(iter));
            if (ctx.has_exception() || !res.is_object()) return {Value(), true};
            Object* r = res.as_object();
            bool done = r->get_property("done").to_boolean();
            return {r->get_property("value"), done};
        };

        auto iter_toArray = ObjectFactory::create_native_function("toArray",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; Object* it = ctx.get_this_binding(); if (!it) return Value();
                auto a = ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_length(i); return Value(a.release());
            },0);
        iter_proto_obj->set_property("toArray", Value(iter_toArray.release()));

        auto iter_forEach2 = ObjectFactory::create_native_function("forEach",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("forEach requires function");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; cb->call(ctx,{v},Value()); if(ctx.has_exception())break;}
                return Value();
            },1);
        iter_proto_obj->set_property("forEach", Value(iter_forEach2.release()));

        auto iter_reduce2 = ObjectFactory::create_native_function("reduce",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("reduce");return Value();}
                Function* cb=args[0].as_function(); Value acc=args.size()>1?args[1]:Value(); bool has_acc=args.size()>1;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(!has_acc){acc=v;has_acc=true;continue;} acc=cb->call(ctx,{acc,v},Value()); if(ctx.has_exception())break;}
                return acc;
            },1);
        iter_proto_obj->set_property("reduce", Value(iter_reduce2.release()));

        auto iter_some2 = ObjectFactory::create_native_function("some",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("some");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v},Value()).to_boolean())return Value(true);}
                return Value(false);
            },1);
        iter_proto_obj->set_property("some", Value(iter_some2.release()));

        auto iter_every2 = ObjectFactory::create_native_function("every",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("every");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(!cb->call(ctx,{v},Value()).to_boolean())return Value(false);}
                return Value(true);
            },1);
        iter_proto_obj->set_property("every", Value(iter_every2.release()));

        auto iter_find2 = ObjectFactory::create_native_function("find",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("find");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v},Value()).to_boolean())return v;}
                return Value();
            },1);
        iter_proto_obj->set_property("find", Value(iter_find2.release()));

        // Lazy Iterator Helpers, mirroring register_iterator_constructor's versions below.
        auto iter_map2 = ObjectFactory::create_native_function("map",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("map called on non-object"); return Value(); }
                if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("map requires a callable"); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();
                if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_fn__", args[0]);
                helper->set_property("__ih_counter__", Value(0.0));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                        Value mapper_val = self->get_property("__ih_fn__");
                        if (!mapper_val.is_function()) return Value(make_iter_result(Value(), true));
                        Value iter_val = self->get_property("__ih_iter__");
                        Value next_method = self->get_property("__ih_next__");
                        double counter = self->get_property("__ih_counter__").to_number();

                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));

                        Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        self->set_property("__ih_counter__", Value(counter + 1));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        return Value(make_iter_result(mapped, false));
                    }, 0);
                helper->set_property("next", Value(next_fn.release()));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("map", Value(iter_map2.release()));

        auto iter_filter2 = ObjectFactory::create_native_function("filter",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("filter called on non-object"); return Value(); }
                if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("filter requires a callable"); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();
                if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_fn__", args[0]);
                helper->set_property("__ih_counter__", Value(0.0));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                        Value pred_val = self->get_property("__ih_fn__");
                        if (!pred_val.is_function()) return Value(make_iter_result(Value(), true));
                        Value iter_val = self->get_property("__ih_iter__");
                        Value next_method = self->get_property("__ih_next__");
                        double counter = self->get_property("__ih_counter__").to_number();

                        while (true) {
                            auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                            if (ctx.has_exception()) return Value();
                            if (done) return Value(make_iter_result(Value(), true));
                            Value keep = pred_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                            counter += 1;
                            self->set_property("__ih_counter__", Value(counter));
                            if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                            if (keep.to_boolean()) return Value(make_iter_result(val, false));
                        }
                    }, 0);
                helper->set_property("next", Value(next_fn.release()));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("filter", Value(iter_filter2.release()));

        auto iter_take2 = ObjectFactory::create_native_function("take",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("take called on non-object"); return Value(); }
                double limit = args.empty() ? 0.0 : args[0].to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(limit) || limit < 0) { ctx.throw_range_error("Invalid count"); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();
                if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_remaining__", Value(limit));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                        Value iter_val = self->get_property("__ih_iter__");
                        double remaining = self->get_property("__ih_remaining__").to_number();
                        if (remaining <= 0) { iterator_helper_close(ctx, iter_val); return Value(make_iter_result(Value(), true)); }
                        self->set_property("__ih_remaining__", Value(remaining - 1));
                        Value next_method = self->get_property("__ih_next__");
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));
                        return Value(make_iter_result(val, false));
                    }, 0);
                helper->set_property("next", Value(next_fn.release()));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("take", Value(iter_take2.release()));

        auto iter_drop2 = ObjectFactory::create_native_function("drop",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("drop called on non-object"); return Value(); }
                double limit = args.empty() ? 0.0 : args[0].to_number();
                if (ctx.has_exception()) return Value();
                if (std::isnan(limit) || limit < 0) { ctx.throw_range_error("Invalid count"); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();
                if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_remaining__", Value(limit));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                        Value iter_val = self->get_property("__ih_iter__");
                        Value next_method = self->get_property("__ih_next__");
                        double remaining = self->get_property("__ih_remaining__").to_number();
                        while (remaining > 0) {
                            auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                            (void)val;
                            remaining -= 1;
                            self->set_property("__ih_remaining__", Value(remaining));
                            if (ctx.has_exception()) return Value();
                            if (done) return Value(make_iter_result(Value(), true));
                        }
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));
                        return Value(make_iter_result(val, false));
                    }, 0);
                helper->set_property("next", Value(next_fn.release()));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("drop", Value(iter_drop2.release()));
    }
}

void register_iterator_constructor(Context& ctx) {
    auto iterator_constructor = ObjectFactory::create_native_constructor("Iterator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            // Iterator is abstract: only equals new.target for a direct `new Iterator()`, not via super().
            Value new_target = ctx.get_new_target();
            if (new_target.is_function() && new_target.as_function() == ctx.get_built_in_object("Iterator")) {
                ctx.throw_type_error("Abstract class Iterator not directly constructable");
                return Value();
            }

            Object* constructor = ctx.get_this_binding();
            auto iterator_obj = ObjectFactory::create_object();
            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    iterator_obj->set_prototype(prototype_val.as_object());
                }
            }

            return Value(iterator_obj.release());
        });

    auto iterator_prototype = ObjectFactory::create_object();

    auto iterator_next = ObjectFactory::create_native_function("next",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto result = ObjectFactory::create_object();
            result->set_property("done", Value(true));
            result->set_property("value", Value());
            return Value(result.release());
        }, 0);
    iterator_prototype->set_property("next", Value(iterator_next.release()));

    // Helper: call next() on an iterator object and get {value, done}
    // Returns {done:true,value:undefined} if any error
    auto call_next = [](Context& ctx, Object* iter) -> std::pair<Value,bool> {
        Value next_fn = iter->get_property("next");
        if (!next_fn.is_function()) return {Value(), true};
        Value result = next_fn.as_function()->call(ctx, {}, Value(iter));
        if (ctx.has_exception()) return {Value(), true};
        if (!result.is_object()) return {Value(), true};
        Object* res_obj = result.as_object();
        Value done_v = res_obj->get_property("done");
        bool done = done_v.to_boolean();
        Value val = res_obj->get_property("value");
        return {val, done};
    };

    // toArray
    auto iter_toArray_fn = ObjectFactory::create_native_function("toArray",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("toArray on non-object"); return Value(); }
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(idx++), val);
            }
            arr->set_length(idx);
            return Value(arr.release());
        }, 0);
    iterator_prototype->set_property("toArray", Value(iter_toArray_fn.release()));

    // forEach
    auto iter_forEach_fn = ObjectFactory::create_native_function("forEach",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("forEach on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("forEach requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
            }
            return Value();
        }, 1);
    iterator_prototype->set_property("forEach", Value(iter_forEach_fn.release()));

    // reduce
    auto iter_reduce_fn = ObjectFactory::create_native_function("reduce",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("reduce requires function"); return Value(); }
            Function* cb = args[0].as_function();
            Value acc = args.size() > 1 ? args[1] : Value();
            bool has_acc = args.size() > 1;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                if (!has_acc) { acc = val; has_acc = true; continue; }
                acc = cb->call(ctx, {acc, val}, Value());
                if (ctx.has_exception()) break;
            }
            return acc;
        }, 1);
    iterator_prototype->set_property("reduce", Value(iter_reduce_fn.release()));

    // some
    auto iter_some_fn = ObjectFactory::create_native_function("some",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("some requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) return Value(true);
            }
            return Value(false);
        }, 1);
    iterator_prototype->set_property("some", Value(iter_some_fn.release()));

    // every
    auto iter_every_fn = ObjectFactory::create_native_function("every",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("every requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (!r.to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    iterator_prototype->set_property("every", Value(iter_every_fn.release()));

    // find
    auto iter_find_fn = ObjectFactory::create_native_function("find",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("find requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) return val;
            }
            return Value();
        }, 1);
    iterator_prototype->set_property("find", Value(iter_find_fn.release()));

    // Lazy Iterator Helpers: pull from the source only as values are demanded.
    Object* iterator_proto_ptr = iterator_prototype.get();

    auto iter_map_fn = ObjectFactory::create_native_function("map",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("map called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("map requires a callable"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    Value mapper_val = self->get_property("__ih_fn__");
                    if (!mapper_val.is_function()) return Value(make_iter_result(Value(), true));
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double counter = self->get_property("__ih_counter__").to_number();

                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) return Value(make_iter_result(Value(), true));

                    Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                    self->set_property("__ih_counter__", Value(counter + 1));
                    if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                    return Value(make_iter_result(mapped, false));
                }, 0);
            helper->set_property("next", Value(next_fn.release()));
            return Value(helper);
        }, 1);
    iterator_prototype->set_property("map", Value(iter_map_fn.release()));

    auto iter_filter_fn = ObjectFactory::create_native_function("filter",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("filter called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("filter requires a callable"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    Value pred_val = self->get_property("__ih_fn__");
                    if (!pred_val.is_function()) return Value(make_iter_result(Value(), true));
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double counter = self->get_property("__ih_counter__").to_number();

                    while (true) {
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));
                        Value keep = pred_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        counter += 1;
                        self->set_property("__ih_counter__", Value(counter));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        if (keep.to_boolean()) return Value(make_iter_result(val, false));
                    }
                }, 0);
            helper->set_property("next", Value(next_fn.release()));
            return Value(helper);
        }, 1);
    iterator_prototype->set_property("filter", Value(iter_filter_fn.release()));

    auto iter_take_fn = ObjectFactory::create_native_function("take",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("take called on non-object"); return Value(); }
            double limit = args.empty() ? 0.0 : args[0].to_number();
            if (ctx.has_exception()) return Value();
            if (std::isnan(limit) || limit < 0) { ctx.throw_range_error("Invalid count"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_remaining__", Value(limit));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    Value iter_val = self->get_property("__ih_iter__");
                    double remaining = self->get_property("__ih_remaining__").to_number();
                    if (remaining <= 0) { iterator_helper_close(ctx, iter_val); return Value(make_iter_result(Value(), true)); }
                    self->set_property("__ih_remaining__", Value(remaining - 1));
                    Value next_method = self->get_property("__ih_next__");
                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) return Value(make_iter_result(Value(), true));
                    return Value(make_iter_result(val, false));
                }, 0);
            helper->set_property("next", Value(next_fn.release()));
            return Value(helper);
        }, 1);
    iterator_prototype->set_property("take", Value(iter_take_fn.release()));

    auto iter_drop_fn = ObjectFactory::create_native_function("drop",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("drop called on non-object"); return Value(); }
            double limit = args.empty() ? 0.0 : args[0].to_number();
            if (ctx.has_exception()) return Value();
            if (std::isnan(limit) || limit < 0) { ctx.throw_range_error("Invalid count"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_remaining__", Value(limit));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double remaining = self->get_property("__ih_remaining__").to_number();
                    while (remaining > 0) {
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        (void)val;
                        remaining -= 1;
                        self->set_property("__ih_remaining__", Value(remaining));
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));
                    }
                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) return Value(make_iter_result(Value(), true));
                    return Value(make_iter_result(val, false));
                }, 0);
            helper->set_property("next", Value(next_fn.release()));
            return Value(helper);
        }, 1);
    iterator_prototype->set_property("drop", Value(iter_drop_fn.release()));

    auto iter_flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("flatMap called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("flatMap requires a callable"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_method.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return Value(); }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));
            helper->set_property("__ih_inner__", Value());
            helper->set_property("__ih_inner_next__", Value());

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    Value mapper_val = self->get_property("__ih_fn__");
                    if (!mapper_val.is_function()) return Value(make_iter_result(Value(), true));
                    Value iter_val = self->get_property("__ih_iter__");
                    Value outer_next = self->get_property("__ih_next__");

                    while (true) {
                        Value inner_val = self->get_property("__ih_inner__");
                        if (inner_val.is_object() || inner_val.is_function()) {
                            Value inner_next = self->get_property("__ih_inner_next__");
                            auto [ival, idone] = iterator_helper_step(ctx, inner_val, inner_next);
                            if (ctx.has_exception()) return Value();
                            if (!idone) return Value(make_iter_result(ival, false));
                            self->set_property("__ih_inner__", Value());
                            self->set_property("__ih_inner_next__", Value());
                        }

                        double counter = self->get_property("__ih_counter__").to_number();
                        auto [val, done] = iterator_helper_step(ctx, iter_val, outer_next);
                        if (ctx.has_exception()) return Value();
                        if (done) return Value(make_iter_result(Value(), true));

                        Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        self->set_property("__ih_counter__", Value(counter + 1));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        if (!mapped.is_object() && !mapped.is_function()) { ctx.throw_type_error("flatMap mapper must return an iterable"); return Value(); }

                        Object* mapped_obj = mapped.is_function() ? static_cast<Object*>(mapped.as_function()) : mapped.as_object();
                        Symbol* fmap_iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                        Value iter_fn = fmap_iter_sym ? mapped_obj->get_property(fmap_iter_sym->to_property_key()) : Value();
                        if (!iter_fn.is_function()) { ctx.throw_type_error("flatMap mapper result is not iterable"); return Value(); }
                        Value inner = iter_fn.as_function()->call(ctx, {}, mapped);
                        if (ctx.has_exception()) return Value();
                        if (!inner.is_object() && !inner.is_function()) { ctx.throw_type_error("Result of [Symbol.iterator] is not an object"); return Value(); }
                        Object* inner_obj = inner.is_function() ? static_cast<Object*>(inner.as_function()) : inner.as_object();
                        Value inner_next_method = inner_obj->get_property("next");
                        if (ctx.has_exception()) return Value();
                        self->set_property("__ih_inner__", inner);
                        self->set_property("__ih_inner_next__", inner_next_method);
                        // loop: pull from the freshly-set inner iterator next time around
                    }
                }, 0);
            helper->set_property("next", Value(next_fn.release()));
            return Value(helper);
        }, 1);
    iterator_prototype->set_property("flatMap", Value(iter_flatMap_fn.release()));

    // Add @@iterator to prototype
    {
        Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
        if (iter_sym) {
            auto self_iter = ObjectFactory::create_native_function("[Symbol.iterator]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            iterator_prototype->set_property(iter_sym->to_property_key(), Value(self_iter.release()));
        }
    }

    // Static Iterator.from
    auto iterator_from = ObjectFactory::create_native_function("from",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) { ctx.throw_type_error("Iterator.from requires an argument"); return Value(); }
            Value input = args[0];
            // If it has [Symbol.iterator], call it
            if (input.is_object()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_fn = input.as_object()->get_property(iter_sym->to_property_key());
                    if (iter_fn.is_function()) {
                        Value iter = iter_fn.as_function()->call(ctx, {}, input);
                        if (!ctx.has_exception() && iter.is_object()) return iter;
                    }
                }
                return input;
            }
            ctx.throw_type_error("Iterator.from: not iterable");
            return Value();
        }, 1);
    iterator_constructor->set_property("from", Value(iterator_from.release()));

    // Static Iterator.concat(...items): lazily exhausts each item in turn; [Symbol.iterator] is
    // resolved eagerly per item but only called once that item is reached.
    auto iterator_concat = ObjectFactory::create_native_function("concat",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            auto items = ObjectFactory::create_array();
            auto methods = ObjectFactory::create_array();
            uint32_t n = 0;
            for (const auto& item : args) {
                if (!item.is_object() && !item.is_function()) { ctx.throw_type_error("Iterator.concat argument must be an object"); return Value(); }
                Object* item_obj = item.is_function() ? static_cast<Object*>(item.as_function()) : item.as_object();
                Value method = iter_sym ? item_obj->get_property(iter_sym->to_property_key()) : Value();
                if (ctx.has_exception()) return Value();
                if (method.is_null() || method.is_undefined()) { ctx.throw_type_error("Iterator.concat argument is not iterable"); return Value(); }
                if (!method.is_function()) { ctx.throw_type_error("[Symbol.iterator] is not a function"); return Value(); }
                items->set_property(std::to_string(n), item);
                methods->set_property(std::to_string(n), method);
                n++;
            }
            items->set_property("length", Value((double)n));
            methods->set_property("length", Value((double)n));

            auto helper = ObjectFactory::create_object();
            helper->set_prototype(iterator_proto_ptr);
            helper->set_property("__ic_items__", Value(items.release()));
            helper->set_property("__ic_methods__", Value(methods.release()));
            helper->set_property("__ic_index__", Value(0.0));
            helper->set_property("__ic_inner__", Value());
            helper->set_property("__ic_inner_next__", Value());

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    while (true) {
                        Value inner_val = self->get_property("__ic_inner__");
                        if (inner_val.is_object() || inner_val.is_function()) {
                            Value inner_next = self->get_property("__ic_inner_next__");
                            auto [val, done] = iterator_helper_step(ctx, inner_val, inner_next);
                            if (ctx.has_exception()) return Value();
                            if (!done) return Value(make_iter_result(val, false));
                            self->set_property("__ic_inner__", Value());
                            self->set_property("__ic_inner_next__", Value());
                        }
                        double index = self->get_property("__ic_index__").to_number();
                        Object* items_obj = self->get_property("__ic_items__").as_object();
                        double total = items_obj->get_property("length").to_number();
                        if (index >= total) return Value(make_iter_result(Value(), true));
                        Value item = items_obj->get_property(std::to_string((uint32_t)index));
                        Object* methods_obj = self->get_property("__ic_methods__").as_object();
                        Value method = methods_obj->get_property(std::to_string((uint32_t)index));
                        self->set_property("__ic_index__", Value(index + 1));

                        Value inner = method.as_function()->call(ctx, {}, item);
                        if (ctx.has_exception()) return Value();
                        if (!inner.is_object() && !inner.is_function()) { ctx.throw_type_error("Result of [Symbol.iterator] is not an object"); return Value(); }
                        Object* inner_obj = inner.is_function() ? static_cast<Object*>(inner.as_function()) : inner.as_object();
                        Value inner_next_method = inner_obj->get_property("next");
                        if (ctx.has_exception()) return Value();
                        self->set_property("__ic_inner__", inner);
                        self->set_property("__ic_inner_next__", inner_next_method);
                    }
                }, 0);
            helper->set_property("next", Value(next_fn.release()));

            auto return_fn = ObjectFactory::create_native_function("return",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (self) {
                        Value inner_val = self->get_property("__ic_inner__");
                        if (inner_val.is_object() || inner_val.is_function()) {
                            iterator_helper_close(ctx, inner_val);
                            self->set_property("__ic_inner__", Value());
                        }
                        Object* items_obj = self->get_property("__ic_items__").as_object();
                        double total = items_obj ? items_obj->get_property("length").to_number() : 0;
                        self->set_property("__ic_index__", Value(total));
                    }
                    return Value(make_iter_result(Value(), true));
                }, 0);
            helper->set_property("return", Value(return_fn.release()));

            return Value(helper.release());
        }, 0);
    iterator_constructor->set_property("concat", Value(iterator_concat.release()), PropertyAttributes::BuiltinFunction);

    // Static Iterator.zip(iterables, options): inner iterators are collected eagerly, per-row
    // stepping is lazy. "padding" (mode "longest") is read once into a fixed per-column array,
    // not re-read per row.
    auto iterator_zip = ObjectFactory::create_native_function("zip",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            Value iterables_val = args.empty() ? Value() : args[0];
            if (!iterables_val.is_object() && !iterables_val.is_function()) { ctx.throw_type_error("Iterator.zip: iterables must be an object"); return Value(); }

            Value options = args.size() > 1 ? args[1] : Value();
            Object* options_obj = nullptr;
            std::unique_ptr<Object> default_options;
            if (options.is_undefined()) {
                default_options = ObjectFactory::create_object();
                options_obj = default_options.get();
            } else if (options.is_object() || options.is_function()) {
                options_obj = options.is_function() ? static_cast<Object*>(options.as_function()) : options.as_object();
            } else {
                ctx.throw_type_error("Iterator.zip: options must be an object"); return Value();
            }

            Value mode_val = options_obj->get_property("mode");
            if (ctx.has_exception()) return Value();
            std::string mode = "shortest";
            if (!mode_val.is_undefined()) {
                if (!mode_val.is_string()) { ctx.throw_type_error("Iterator.zip: mode must be a string"); return Value(); }
                mode = mode_val.to_string();
                if (mode != "shortest" && mode != "longest" && mode != "strict") { ctx.throw_type_error("Iterator.zip: invalid mode"); return Value(); }
            }

            Value padding_option;
            if (mode == "longest") {
                padding_option = options_obj->get_property("padding");
                if (ctx.has_exception()) return Value();
                if (!padding_option.is_undefined() && !padding_option.is_object() && !padding_option.is_function()) {
                    ctx.throw_type_error("Iterator.zip: padding must be an object"); return Value();
                }
            }

            Object* iterables_obj = iterables_val.is_function() ? static_cast<Object*>(iterables_val.as_function()) : iterables_val.as_object();
            Value outer_iter_fn = iter_sym ? iterables_obj->get_property(iter_sym->to_property_key()) : Value();
            if (ctx.has_exception()) return Value();
            if (!outer_iter_fn.is_function()) { ctx.throw_type_error("Iterator.zip: iterables is not iterable"); return Value(); }
            Value outer_iter = outer_iter_fn.as_function()->call(ctx, {}, iterables_val);
            if (ctx.has_exception()) return Value();
            if (!outer_iter.is_object() && !outer_iter.is_function()) { ctx.throw_type_error("Result of [Symbol.iterator] is not an object"); return Value(); }
            Object* outer_iter_obj = outer_iter.is_function() ? static_cast<Object*>(outer_iter.as_function()) : outer_iter.as_object();
            Value outer_next = outer_iter_obj->get_property("next");
            if (ctx.has_exception()) return Value();

            std::vector<Value> iters;
            std::vector<Value> iter_nexts;
            while (true) {
                auto [item, done] = iterator_helper_step(ctx, outer_iter, outer_next);
                if (ctx.has_exception()) {
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                if (done) break;
                if (!item.is_object() && !item.is_function()) {
                    ctx.throw_type_error("Iterator.zip: each iterable item must be an object");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Object* item_obj = item.is_function() ? static_cast<Object*>(item.as_function()) : item.as_object();
                Value method = iter_sym ? item_obj->get_property(iter_sym->to_property_key()) : Value();
                Value inner_iter = item;
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (!method.is_undefined() && !method.is_null()) {
                    if (!method.is_function()) {
                        ctx.throw_type_error("[Symbol.iterator] is not a function");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        return Value();
                    }
                    inner_iter = method.as_function()->call(ctx, {}, item);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                    if (!inner_iter.is_object() && !inner_iter.is_function()) {
                        ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        return Value();
                    }
                }
                Object* inner_obj = inner_iter.is_function() ? static_cast<Object*>(inner_iter.as_function()) : inner_iter.as_object();
                Value inner_next = inner_obj->get_property("next");
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                iters.push_back(inner_iter);
                iter_nexts.push_back(inner_next);
            }

            uint32_t iter_count = (uint32_t)iters.size();
            std::vector<Value> padding(iter_count, Value());
            if (mode == "longest" && !padding_option.is_undefined()) {
                Object* padding_obj = padding_option.is_function() ? static_cast<Object*>(padding_option.as_function()) : padding_option.as_object();
                Value pad_iter_fn = iter_sym ? padding_obj->get_property(iter_sym->to_property_key()) : Value();
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (!pad_iter_fn.is_function()) {
                    ctx.throw_type_error("Iterator.zip: padding is not iterable");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Value pad_iter = pad_iter_fn.as_function()->call(ctx, {}, padding_option);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (!pad_iter.is_object() && !pad_iter.is_function()) {
                    ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Object* pad_obj = pad_iter.is_function() ? static_cast<Object*>(pad_iter.as_function()) : pad_iter.as_object();
                Value pad_next = pad_obj->get_property("next");
                bool exhausted = false;
                for (uint32_t i = 0; i < iter_count && !exhausted; i++) {
                    auto [pv, pd] = iterator_helper_step(ctx, pad_iter, pad_next);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                    if (pd) exhausted = true;
                    else padding[i] = pv;
                }
                if (!exhausted) iterator_helper_close(ctx, pad_iter);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
            }

            auto helper = ObjectFactory::create_object();
            helper->set_prototype(iterator_proto_ptr);
            auto iters_arr = ObjectFactory::create_array();
            auto nexts_arr = ObjectFactory::create_array();
            auto padding_arr = ObjectFactory::create_array();
            auto alive_arr = ObjectFactory::create_array();
            for (uint32_t i = 0; i < iter_count; i++) {
                iters_arr->set_property(std::to_string(i), iters[i]);
                nexts_arr->set_property(std::to_string(i), iter_nexts[i]);
                padding_arr->set_property(std::to_string(i), padding[i]);
                alive_arr->set_property(std::to_string(i), Value(true));
            }
            iters_arr->set_property("length", Value((double)iter_count));
            nexts_arr->set_property("length", Value((double)iter_count));
            padding_arr->set_property("length", Value((double)iter_count));
            alive_arr->set_property("length", Value((double)iter_count));
            helper->set_property("__iz_iters__", Value(iters_arr.release()));
            helper->set_property("__iz_nexts__", Value(nexts_arr.release()));
            helper->set_property("__iz_padding__", Value(padding_arr.release()));
            helper->set_property("__iz_alive__", Value(alive_arr.release()));
            helper->set_property("__iz_count__", Value((double)iter_count));
            helper->set_property("__iz_mode__", Value(mode));
            helper->set_property("__iz_done__", Value(false));
            helper->set_property("__iz_keyed__", Value(false));

            auto next_fn = ObjectFactory::create_native_function("next", iterator_zip_step, 0);
            helper->set_property("next", Value(next_fn.release()));
            auto return_fn = ObjectFactory::create_native_function("return", iterator_zip_return, 0);
            helper->set_property("return", Value(return_fn.release()));

            return Value(helper.release());
        }, 1);
    iterator_constructor->set_property("zip", Value(iterator_zip.release()), PropertyAttributes::BuiltinFunction);

    // Static Iterator.zipKeyed: like zip, but columns come from iterables' own enumerable keys
    // and each row is a null-prototype object keyed the same way.
    auto iterator_zipKeyed = ObjectFactory::create_native_function("zipKeyed",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            Value iterables_val = args.empty() ? Value() : args[0];
            if (!iterables_val.is_object() && !iterables_val.is_function()) { ctx.throw_type_error("Iterator.zipKeyed: iterables must be an object"); return Value(); }

            Value options = args.size() > 1 ? args[1] : Value();
            Object* options_obj = nullptr;
            std::unique_ptr<Object> default_options;
            if (options.is_undefined()) {
                default_options = ObjectFactory::create_object();
                options_obj = default_options.get();
            } else if (options.is_object() || options.is_function()) {
                options_obj = options.is_function() ? static_cast<Object*>(options.as_function()) : options.as_object();
            } else {
                ctx.throw_type_error("Iterator.zipKeyed: options must be an object"); return Value();
            }

            Value mode_val = options_obj->get_property("mode");
            if (ctx.has_exception()) return Value();
            std::string mode = "shortest";
            if (!mode_val.is_undefined()) {
                if (!mode_val.is_string()) { ctx.throw_type_error("Iterator.zipKeyed: mode must be a string"); return Value(); }
                mode = mode_val.to_string();
                if (mode != "shortest" && mode != "longest" && mode != "strict") { ctx.throw_type_error("Iterator.zipKeyed: invalid mode"); return Value(); }
            }

            Value padding_option;
            if (mode == "longest") {
                padding_option = options_obj->get_property("padding");
                if (ctx.has_exception()) return Value();
                if (!padding_option.is_undefined() && !padding_option.is_object() && !padding_option.is_function()) {
                    ctx.throw_type_error("Iterator.zipKeyed: padding must be an object"); return Value();
                }
            }

            Object* iterables_obj = iterables_val.is_function() ? static_cast<Object*>(iterables_val.as_function()) : iterables_val.as_object();
            std::vector<std::string> keys;
            std::vector<Value> iters;
            std::vector<Value> iter_nexts;
            for (const auto& key : iterables_obj->get_own_property_keys()) {
                PropertyDescriptor desc = iterables_obj->get_property_descriptor(key);
                if (!desc.is_enumerable()) continue;
                Value value = iterables_obj->get_property(key);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (value.is_undefined()) continue;
                if (!value.is_object() && !value.is_function()) {
                    ctx.throw_type_error("Iterator.zipKeyed: each iterable value must be an object");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Object* value_obj = value.is_function() ? static_cast<Object*>(value.as_function()) : value.as_object();
                Value method = iter_sym ? value_obj->get_property(iter_sym->to_property_key()) : Value();
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                Value inner_iter = value;
                if (!method.is_undefined() && !method.is_null()) {
                    if (!method.is_function()) {
                        ctx.throw_type_error("[Symbol.iterator] is not a function");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        return Value();
                    }
                    inner_iter = method.as_function()->call(ctx, {}, value);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                    if (!inner_iter.is_object() && !inner_iter.is_function()) {
                        ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        return Value();
                    }
                }
                Object* inner_obj = inner_iter.is_function() ? static_cast<Object*>(inner_iter.as_function()) : inner_iter.as_object();
                Value inner_next = inner_obj->get_property("next");
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                keys.push_back(key);
                iters.push_back(inner_iter);
                iter_nexts.push_back(inner_next);
            }

            uint32_t iter_count = (uint32_t)iters.size();
            std::vector<Value> padding(iter_count, Value());
            if (mode == "longest" && !padding_option.is_undefined()) {
                Object* padding_obj = padding_option.is_function() ? static_cast<Object*>(padding_option.as_function()) : padding_option.as_object();
                Value pad_iter_fn = iter_sym ? padding_obj->get_property(iter_sym->to_property_key()) : Value();
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (!pad_iter_fn.is_function()) {
                    ctx.throw_type_error("Iterator.zipKeyed: padding is not iterable");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Value pad_iter = pad_iter_fn.as_function()->call(ctx, {}, padding_option);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                if (!pad_iter.is_object() && !pad_iter.is_function()) {
                    ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                    for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                    return Value();
                }
                Object* pad_obj = pad_iter.is_function() ? static_cast<Object*>(pad_iter.as_function()) : pad_iter.as_object();
                Value pad_next = pad_obj->get_property("next");
                bool exhausted = false;
                for (uint32_t i = 0; i < iter_count && !exhausted; i++) {
                    auto [pv, pd] = iterator_helper_step(ctx, pad_iter, pad_next);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                    if (pd) exhausted = true;
                    else padding[i] = pv;
                }
                if (!exhausted) iterator_helper_close(ctx, pad_iter);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
            }

            auto helper = ObjectFactory::create_object();
            helper->set_prototype(iterator_proto_ptr);
            auto iters_arr = ObjectFactory::create_array();
            auto nexts_arr = ObjectFactory::create_array();
            auto padding_arr = ObjectFactory::create_array();
            auto alive_arr = ObjectFactory::create_array();
            auto keys_arr = ObjectFactory::create_array();
            for (uint32_t i = 0; i < iter_count; i++) {
                iters_arr->set_property(std::to_string(i), iters[i]);
                nexts_arr->set_property(std::to_string(i), iter_nexts[i]);
                padding_arr->set_property(std::to_string(i), padding[i]);
                alive_arr->set_property(std::to_string(i), Value(true));
                keys_arr->set_property(std::to_string(i), Value(keys[i]));
            }
            iters_arr->set_property("length", Value((double)iter_count));
            nexts_arr->set_property("length", Value((double)iter_count));
            padding_arr->set_property("length", Value((double)iter_count));
            alive_arr->set_property("length", Value((double)iter_count));
            keys_arr->set_property("length", Value((double)iter_count));
            helper->set_property("__iz_iters__", Value(iters_arr.release()));
            helper->set_property("__iz_nexts__", Value(nexts_arr.release()));
            helper->set_property("__iz_padding__", Value(padding_arr.release()));
            helper->set_property("__iz_alive__", Value(alive_arr.release()));
            helper->set_property("__iz_keys__", Value(keys_arr.release()));
            helper->set_property("__iz_count__", Value((double)iter_count));
            helper->set_property("__iz_mode__", Value(mode));
            helper->set_property("__iz_done__", Value(false));
            helper->set_property("__iz_keyed__", Value(true));

            auto next_fn = ObjectFactory::create_native_function("next", iterator_zip_step, 0);
            helper->set_property("next", Value(next_fn.release()));
            auto return_fn = ObjectFactory::create_native_function("return", iterator_zip_return, 0);
            helper->set_property("return", Value(return_fn.release()));

            return Value(helper.release());
        }, 1);
    iterator_constructor->set_property("zipKeyed", Value(iterator_zipKeyed.release()), PropertyAttributes::BuiltinFunction);

    iterator_constructor->set_property("prototype", Value(iterator_prototype.release()));
    ctx.register_built_in_object("Iterator", iterator_constructor.release());
}

} // namespace Quanta
