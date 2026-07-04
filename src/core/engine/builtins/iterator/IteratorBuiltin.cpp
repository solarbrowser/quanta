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
#include "quanta/core/runtime/ProxyReflect.h"
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
    Value done_val = result.as_object()->get_property("done");
    if (ctx.has_exception()) return {Value(), true};
    if (done_val.to_boolean()) return {Value(), true};
    Value val = result.as_object()->get_property("value");
    if (ctx.has_exception()) return {Value(), true};
    return {val, false};
}

// IteratorClose: a pending throw completion survives both a throwing "return"
// getter (GetMethod) and a throwing return() call; with a normal completion,
// those inner errors propagate instead.
static void iterator_helper_close(Context& ctx, const Value& iter_val) {
    if (!iter_val.is_object() && !iter_val.is_function()) return;
    Object* obj = iter_val.is_function() ? static_cast<Object*>(iter_val.as_function()) : iter_val.as_object();
    bool had_exception = ctx.has_exception();
    Value pending = had_exception ? ctx.get_exception() : Value();
    if (had_exception) ctx.clear_exception();

    Value ret_fn = obj->get_property("return");
    if (ctx.has_exception()) {
        if (had_exception) {
            ctx.clear_exception();
            ctx.throw_exception(pending, true);
        }
        return;
    }
    if (ret_fn.is_undefined() || ret_fn.is_null()) {
        if (had_exception) ctx.throw_exception(pending, true);
        return;
    }
    if (!ret_fn.is_function()) {
        if (had_exception) ctx.throw_exception(pending, true);
        else ctx.throw_type_error("iterator return is not callable");
        return;
    }
    Value inner = ret_fn.as_function()->call(ctx, {}, iter_val);
    if (had_exception) {
        ctx.clear_exception();
        ctx.throw_exception(pending, true);
        return;
    }
    if (ctx.has_exception()) return;
    if (!inner.is_object() && !inner.is_function()) {
        ctx.throw_type_error("iterator return did not return an object");
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
// Symbol.iterator from iterator_proto, and wires up `return`. Callers add their own `next`
// via set_guarded_next (which also rejects reentrant calls while one is already running).
static Object* create_iterator_helper_base(Object* iterator_proto, const Value& iter_val, const Value& next_method) {
    auto helper = ObjectFactory::create_object();
    helper->set_prototype(iterator_proto);
    helper->set_property("__ih_iter__", iter_val);
    helper->set_property("__ih_next__", next_method);
    helper->set_property("__ih_running__", Value(false));

    auto return_fn = ObjectFactory::create_native_function("return",
        [](Context& ctx, const std::vector<Value>&) -> Value {
            Object* self = ctx.get_this_binding();
            if (self) {
                Value inner_val = self->get_property("__ih_inner__");
                if (inner_val.is_object() || inner_val.is_function()) {
                    iterator_helper_close(ctx, inner_val);
                    self->set_property("__ih_inner__", Value());
                }
                Value iter_val = self->get_property("__ih_iter__");
                if (iter_val.is_object() || iter_val.is_function()) {
                    iterator_helper_close(ctx, iter_val);
                    self->set_property("__ih_iter__", Value());
                }
            }
            return Value(make_iter_result(Value(), true));
        }, 0);
    helper->set_property("return", Value(return_fn.release()));

    return helper.release();
}

// Wraps a helper's actual `next` logic with reentrancy protection: a `next` call made while
// the helper is already mid-call (e.g. the underlying iterator's own `next` recursively calls
// back into this helper) throws instead of recursing, matching GeneratorResume's running check.
static void set_guarded_next(Object* helper, std::unique_ptr<Object> actual_next_fn) {
    helper->set_property("__ih_actual_next__", Value(actual_next_fn.release()));
    auto guarded = ObjectFactory::create_native_function("next",
        [](Context& ctx, const std::vector<Value>&) -> Value {
            Object* self = ctx.get_this_binding();
            if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
            // If __ih_iter__ was cleared (exhausted or closed), report done immediately.
            Value ih_iter = self->get_property("__ih_iter__");
            if (!ih_iter.is_object() && !ih_iter.is_function())
                return Value(make_iter_result(Value(), true));
            if (self->get_property("__ih_running__").to_boolean()) {
                ctx.throw_type_error("Iterator helper is already running");
                return Value();
            }
            self->set_property("__ih_running__", Value(true));
            Value actual = self->get_property("__ih_actual_next__");
            Value result = actual.as_function()->call(ctx, {}, Value(self));
            self->set_property("__ih_running__", Value(false));
            return result;
        }, 0);
    helper->set_property("next", Value(guarded.release()));
}

// IteratorCloseAll over the still-open (alive) columns, in reverse order.
// Callers mark exhausted/failed columns dead in alive_arr before closing.
static void iterator_zip_close_all(Context& ctx, Object* iters_arr, Object* alive_arr, uint32_t count) {
    for (uint32_t j = count; j-- > 0;) {
        if (alive_arr->get_property(std::to_string(j)).to_boolean())
            iterator_helper_close(ctx, iters_arr->get_property(std::to_string(j)));
    }
}

// Shared next() for zip/zipKeyed: steps every alive column, padding exhausted ones in "longest"
// mode, packaging the row as an array or (zipKeyed) a null-prototype keyed object.
static Value iterator_zip_step(Context& ctx, const std::vector<Value>&) {
    Object* self = ctx.get_this_binding();
    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
    // GeneratorValidate runs before the completed-state check: a reentrant call
    // while executing is a TypeError even if the generator was just completed.
    if (self->get_property("__iz_running__").to_boolean()) {
        ctx.throw_type_error("Iterator.zip helper is already running");
        return Value();
    }
    if (self->get_property("__iz_done__").to_boolean()) return Value(make_iter_result(Value(), true));
    self->set_property("__iz_running__", Value(true));
    self->set_property("__iz_started__", Value(true));

    uint32_t count = (uint32_t)self->get_property("__iz_count__").to_number();
    std::string mode = self->get_property("__iz_mode__").to_string();
    bool keyed = self->get_property("__iz_keyed__").to_boolean();
    Object* iters_arr = self->get_property("__iz_iters__").as_object();
    Object* nexts_arr = self->get_property("__iz_nexts__").as_object();
    Object* padding_arr = self->get_property("__iz_padding__").as_object();
    Object* alive_arr = self->get_property("__iz_alive__").as_object();
    Object* keys_arr = keyed ? self->get_property("__iz_keys__").as_object() : nullptr;

    auto mark_dead = [&](uint32_t idx) { alive_arr->set_property(std::to_string(idx), Value(false)); };
    // The pending exception set before closing survives; return() errors are swallowed.
    auto finish_throwing = [&]() -> Value {
        self->set_property("__iz_done__", Value(true));
        iterator_zip_close_all(ctx, iters_arr, alive_arr, count);
        self->set_property("__iz_running__", Value(false));
        return Value();
    };
    auto finish_done = [&]() -> Value {
        self->set_property("__iz_done__", Value(true));
        self->set_property("__iz_running__", Value(false));
        return Value(make_iter_result(Value(), true));
    };

    if (count == 0) return finish_done();

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
            mark_dead(i);
            return finish_throwing();
        }
        if (done) {
            mark_dead(i);
            if (mode == "shortest") {
                self->set_property("__iz_done__", Value(true));
                iterator_zip_close_all(ctx, iters_arr, alive_arr, count);
                self->set_property("__iz_running__", Value(false));
                return Value(make_iter_result(Value(), true));
            } else if (mode == "strict") {
                if (i != 0) {
                    ctx.throw_type_error("Iterator.zip: iterables are not the same length (strict mode)");
                    return finish_throwing();
                }
                // i == 0: every remaining column must also be exhausted; stop at
                // the first live one and close the still-open columns in reverse.
                for (uint32_t j = 1; j < count; j++) {
                    if (!alive_arr->get_property(std::to_string(j)).to_boolean()) continue;
                    auto [jval, jdone] = iterator_helper_step(ctx, iters_arr->get_property(std::to_string(j)), nexts_arr->get_property(std::to_string(j)));
                    (void)jval;
                    if (ctx.has_exception()) {
                        mark_dead(j);
                        return finish_throwing();
                    }
                    if (jdone) {
                        mark_dead(j);
                        continue;
                    }
                    ctx.throw_type_error("Iterator.zip: iterables are not the same length (strict mode)");
                    return finish_throwing();
                }
                return finish_done();
            } else { // longest
                results->set_property(out_key, padding_arr->get_property(std::to_string(i)));
                continue;
            }
        }
        results->set_property(out_key, val);
    }

    if (mode == "longest") {
        bool all_dead = true;
        for (uint32_t i = 0; i < count; i++) if (alive_arr->get_property(std::to_string(i)).to_boolean()) { all_dead = false; break; }
        if (all_dead) return finish_done();
    }

    if (!keyed) results->set_length(count);
    self->set_property("__iz_running__", Value(false));
    return Value(make_iter_result(Value(results.release()), false));
}

static Value iterator_zip_return(Context& ctx, const std::vector<Value>&) {
    Object* self = ctx.get_this_binding();
    if (!self) return Value(make_iter_result(Value(), true));
    // A reentrant return() while the helper is mid-call (e.g. from an inner
    // iterator's own return) sees the generator in the executing state.
    if (self->get_property("__iz_running__").to_boolean()) {
        ctx.throw_type_error("Iterator.zip helper is already running");
        return Value();
    }
    if (!self->get_property("__iz_done__").to_boolean()) {
        self->set_property("__iz_done__", Value(true));
        // At suspendedStart the generator is completed without resuming, so the
        // closes run outside the body and reentrant calls see state "completed".
        // Once started, return() resumes the body and closes run while "executing".
        bool started = self->get_property("__iz_started__").to_boolean();
        if (started) self->set_property("__iz_running__", Value(true));
        uint32_t count = (uint32_t)self->get_property("__iz_count__").to_number();
        Object* iters_arr = self->get_property("__iz_iters__").as_object();
        Object* alive_arr = self->get_property("__iz_alive__").as_object();
        iterator_zip_close_all(ctx, iters_arr, alive_arr, count);
        if (started) self->set_property("__iz_running__", Value(false));
        if (ctx.has_exception()) return Value();
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

        // %IteratorPrototype%[Symbol.dispose]: GetMethod(O, "return") then call it if present.
        Symbol* dispose_sym = Symbol::get_well_known(Symbol::DISPOSE);
        if (dispose_sym) {
            auto iter_dispose = ObjectFactory::create_native_function("[Symbol.dispose]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("Symbol.dispose called on non-object"); return Value(); }
                    Value ret_fn = self->get_property("return");
                    if (ctx.has_exception()) return Value();
                    if (ret_fn.is_function()) {
                        ret_fn.as_function()->call(ctx, {}, Value(self));
                        if (ctx.has_exception()) return Value();
                    }
                    return Value();
                }, 0);
            iter_proto_obj->set_property(dispose_sym->to_property_key(), Value(iter_dispose.release()), PropertyAttributes::BuiltinFunction);
        }

        (void)call_iter_next;
        auto iter_toArray = ObjectFactory::create_native_function("toArray",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; Object* it = ctx.get_this_binding(); if (!it) return Value();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                auto a = ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break; a->set_property(std::to_string(i++),v);}
                a->set_length(i); return Value(a.release());
            },0);
        iter_proto_obj->set_property("toArray", Value(iter_toArray.release()));

        auto iter_forEach2 = ObjectFactory::create_native_function("forEach",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding();
                if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("forEach requires a callable");iterator_helper_close(ctx,Value(it));return Value();}
                Function* cb=args[0].as_function();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                double counter=0;
                while(true){auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break; cb->call(ctx,{v,Value(counter)},Value()); counter+=1; if(ctx.has_exception()){iterator_helper_close(ctx,Value(it));return Value();}}
                return Value();
            },1);
        iter_proto_obj->set_property("forEach", Value(iter_forEach2.release()));

        auto iter_reduce2 = ObjectFactory::create_native_function("reduce",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding();
                if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("reduce requires a callable");iterator_helper_close(ctx,Value(it));return Value();}
                Function* cb=args[0].as_function();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                Value acc=args.size()>1?args[1]:Value(); bool has_acc=args.size()>1; double counter=0;
                while(true){
                    auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break;
                    if(!has_acc){acc=v;has_acc=true;counter+=1;continue;}
                    acc=cb->call(ctx,{acc,v,Value(counter)},Value()); counter+=1;
                    if(ctx.has_exception()){iterator_helper_close(ctx,Value(it));return Value();}
                }
                if (!has_acc) { ctx.throw_type_error("Reduce of empty iterator with no initial value"); return Value(); }
                return acc;
            },1);
        iter_proto_obj->set_property("reduce", Value(iter_reduce2.release()));

        auto iter_some2 = ObjectFactory::create_native_function("some",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding();
                if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("some requires a callable");iterator_helper_close(ctx,Value(it));return Value();}
                Function* cb=args[0].as_function();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                double counter=0;
                while(true){
                    auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break;
                    Value r=cb->call(ctx,{v,Value(counter)},Value()); counter+=1;
                    if(ctx.has_exception()){iterator_helper_close(ctx,Value(it));return Value();}
                    if(r.to_boolean()){iterator_helper_close(ctx,Value(it));return Value(true);}
                }
                return Value(false);
            },1);
        iter_proto_obj->set_property("some", Value(iter_some2.release()));

        auto iter_every2 = ObjectFactory::create_native_function("every",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding();
                if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("every requires a callable");iterator_helper_close(ctx,Value(it));return Value();}
                Function* cb=args[0].as_function();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                double counter=0;
                while(true){
                    auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break;
                    Value r=cb->call(ctx,{v,Value(counter)},Value()); counter+=1;
                    if(ctx.has_exception()){iterator_helper_close(ctx,Value(it));return Value();}
                    if(!r.to_boolean()){iterator_helper_close(ctx,Value(it));return Value(false);}
                }
                return Value(true);
            },1);
        iter_proto_obj->set_property("every", Value(iter_every2.release()));

        auto iter_find2 = ObjectFactory::create_native_function("find",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding();
                if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("find requires a callable");iterator_helper_close(ctx,Value(it));return Value();}
                Function* cb=args[0].as_function();
                Value next_method = it->get_property("next");
                if (ctx.has_exception()) return Value();
                double counter=0;
                while(true){
                    auto[v,d]=iterator_helper_step(ctx,Value(it),next_method); if(ctx.has_exception())return Value(); if(d)break;
                    Value r=cb->call(ctx,{v,Value(counter)},Value()); counter+=1;
                    if(ctx.has_exception()){iterator_helper_close(ctx,Value(it));return Value();}
                    if(r.to_boolean()){iterator_helper_close(ctx,Value(it));return v;}
                }
                return Value();
            },1);
        iter_proto_obj->set_property("find", Value(iter_find2.release()));

        // Lazy Iterator Helpers, mirroring register_iterator_constructor's versions below.
        auto iter_map2 = ObjectFactory::create_native_function("map",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("map called on non-object"); return Value(); }
                if (args.empty() || !args[0].is_function()) {
                    ctx.throw_type_error("map requires a callable");
                    iterator_helper_close(ctx, Value(iter));
                    return Value();
                }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_fn__", args[0]);
                helper->set_property("__ih_counter__", Value(0.0));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        Value mapper_val = self->get_property("__ih_fn__");
                        Value iter_val = self->get_property("__ih_iter__");
                        Value next_method = self->get_property("__ih_next__");
                        double counter = self->get_property("__ih_counter__").to_number();

                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }

                        Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        self->set_property("__ih_counter__", Value(counter + 1));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        return Value(make_iter_result(mapped, false));
                    }, 0);
                set_guarded_next(helper, std::move(next_fn));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("map", Value(iter_map2.release()));

        auto iter_filter2 = ObjectFactory::create_native_function("filter",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("filter called on non-object"); return Value(); }
                if (args.empty() || !args[0].is_function()) {
                    ctx.throw_type_error("filter requires a callable");
                    iterator_helper_close(ctx, Value(iter));
                    return Value();
                }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_fn__", args[0]);
                helper->set_property("__ih_counter__", Value(0.0));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        Value pred_val = self->get_property("__ih_fn__");
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
                set_guarded_next(helper, std::move(next_fn));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("filter", Value(iter_filter2.release()));

        auto iter_take2 = ObjectFactory::create_native_function("take",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("take called on non-object"); return Value(); }
                double num_limit = args.empty() ? std::nan("") : args[0].to_number();
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
                if (std::isnan(num_limit)) { ctx.throw_range_error("Invalid count"); iterator_helper_close(ctx, Value(iter)); return Value(); }
                double limit = (std::isinf(num_limit) && num_limit > 0) ? num_limit : std::trunc(num_limit);
                if (limit < 0) { ctx.throw_range_error("Invalid count"); iterator_helper_close(ctx, Value(iter)); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_remaining__", Value(limit));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
                        Value iter_val = self->get_property("__ih_iter__");
                        double remaining = self->get_property("__ih_remaining__").to_number();
                        if (remaining <= 0) { iterator_helper_close(ctx, iter_val); self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                        self->set_property("__ih_remaining__", Value(remaining - 1));
                        Value next_method = self->get_property("__ih_next__");
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                        return Value(make_iter_result(val, false));
                    }, 0);
                set_guarded_next(helper, std::move(next_fn));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("take", Value(iter_take2.release()));

        auto iter_drop2 = ObjectFactory::create_native_function("drop",
            [iter_proto_obj](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* iter = ctx.get_this_binding();
                if (!iter) { ctx.throw_type_error("drop called on non-object"); return Value(); }
                double num_limit = args.empty() ? std::nan("") : args[0].to_number();
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
                if (std::isnan(num_limit)) { ctx.throw_range_error("Invalid count"); iterator_helper_close(ctx, Value(iter)); return Value(); }
                double limit = (std::isinf(num_limit) && num_limit > 0) ? num_limit : std::trunc(num_limit);
                if (limit < 0) { ctx.throw_range_error("Invalid count"); iterator_helper_close(ctx, Value(iter)); return Value(); }
                Value next_method = iter->get_property("next");
                if (ctx.has_exception()) return Value();

                Object* helper = create_iterator_helper_base(iter_proto_obj, Value(iter), next_method);
                helper->set_property("__ih_remaining__", Value(limit));

                auto next_fn = ObjectFactory::create_native_function("next",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* self = ctx.get_this_binding();
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
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                        return Value(make_iter_result(val, false));
                    }, 0);
                set_guarded_next(helper, std::move(next_fn));
                return Value(helper);
            }, 1);
        iter_proto_obj->set_property("drop", Value(iter_drop2.release()));
    }
}

void register_iterator_constructor(Context& ctx) {
    auto iterator_constructor = ObjectFactory::create_native_constructor("Iterator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value new_target = ctx.get_new_target();
            // Throw if called as plain function (no new.target) OR as direct `new Iterator()`.
            if (new_target.is_undefined() ||
                (new_target.is_function() && new_target.as_function() == ctx.get_built_in_object("Iterator"))) {
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

    // toArray
    auto iter_toArray_fn = ObjectFactory::create_native_function("toArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("toArray on non-object"); return Value(); }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                arr->set_property(std::to_string(idx++), val);
            }
            arr->set_length(idx);
            return Value(arr.release());
        }, 0);
    { PropertyDescriptor _d(Value(iter_toArray_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("toArray", _d); }

    // forEach
    auto iter_forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("forEach on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("forEach requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Function* cb = args[0].as_function();
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            double counter = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                cb->call(ctx, {val, Value(counter)}, Value());
                counter += 1;
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
            }
            return Value();
        }, 1);
    { PropertyDescriptor _d(Value(iter_forEach_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("forEach", _d); }

    // reduce
    auto iter_reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("reduce on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("reduce requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Function* cb = args[0].as_function();
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            Value acc = args.size() > 1 ? args[1] : Value();
            bool has_acc = args.size() > 1;
            double counter = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                if (!has_acc) { acc = val; has_acc = true; counter += 1; continue; }
                acc = cb->call(ctx, {acc, val, Value(counter)}, Value());
                counter += 1;
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
            }
            if (!has_acc) { ctx.throw_type_error("Reduce of empty iterator with no initial value"); return Value(); }
            return acc;
        }, 1);
    { PropertyDescriptor _d(Value(iter_reduce_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("reduce", _d); }

    // some
    auto iter_some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("some on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("some requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Function* cb = args[0].as_function();
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            double counter = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                Value r = cb->call(ctx, {val, Value(counter)}, Value());
                counter += 1;
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
                if (r.to_boolean()) { iterator_helper_close(ctx, Value(iter)); return Value(true); }
            }
            return Value(false);
        }, 1);
    { PropertyDescriptor _d(Value(iter_some_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("some", _d); }

    // every
    auto iter_every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("every on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("every requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Function* cb = args[0].as_function();
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            double counter = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                Value r = cb->call(ctx, {val, Value(counter)}, Value());
                counter += 1;
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
                if (!r.to_boolean()) { iterator_helper_close(ctx, Value(iter)); return Value(false); }
            }
            return Value(true);
        }, 1);
    { PropertyDescriptor _d(Value(iter_every_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("every", _d); }

    // find
    auto iter_find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("find on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("find requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Function* cb = args[0].as_function();
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            double counter = 0;
            while (true) {
                auto [val, done] = iterator_helper_step(ctx, Value(iter), next_method);
                if (ctx.has_exception()) return Value();
                if (done) break;
                Value r = cb->call(ctx, {val, Value(counter)}, Value());
                counter += 1;
                if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
                if (r.to_boolean()) { iterator_helper_close(ctx, Value(iter)); return val; }
            }
            return Value();
        }, 1);
    { PropertyDescriptor _d(Value(iter_find_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("find", _d); }

    // Lazy Iterator Helpers: pull from the source only as values are demanded.
    Object* iterator_proto_ptr = iterator_prototype.get();

    auto iter_map_fn = ObjectFactory::create_native_function("map",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("map called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("map requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (next_method.is_undefined()) {
                ctx.throw_type_error("Iterator next is undefined");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    Value mapper_val = self->get_property("__ih_fn__");
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double counter = self->get_property("__ih_counter__").to_number();

                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }

                    Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                    self->set_property("__ih_counter__", Value(counter + 1));
                    if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                    return Value(make_iter_result(mapped, false));
                }, 0);
            set_guarded_next(helper, std::move(next_fn));
            return Value(helper);
        }, 1);
    { PropertyDescriptor _d(Value(iter_map_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("map", _d); }

    auto iter_filter_fn = ObjectFactory::create_native_function("filter",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("filter called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("filter requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (next_method.is_undefined()) {
                ctx.throw_type_error("Iterator next is undefined");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    Value pred_val = self->get_property("__ih_fn__");
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double counter = self->get_property("__ih_counter__").to_number();

                    while (true) {
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        if (ctx.has_exception()) return Value();
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                        Value keep = pred_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        counter += 1;
                        self->set_property("__ih_counter__", Value(counter));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        if (keep.to_boolean()) return Value(make_iter_result(val, false));
                    }
                }, 0);
            set_guarded_next(helper, std::move(next_fn));
            return Value(helper);
        }, 1);
    { PropertyDescriptor _d(Value(iter_filter_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("filter", _d); }

    auto iter_take_fn = ObjectFactory::create_native_function("take",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("take called on non-object"); return Value(); }
            double num_limit = args.empty() ? std::nan("") : args[0].to_number();
            if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
            if (std::isnan(num_limit)) {
                ctx.throw_range_error("Invalid count");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            double limit = (std::isinf(num_limit) && num_limit > 0) ? num_limit : std::trunc(num_limit);
            if (limit < 0) {
                ctx.throw_range_error("Invalid count");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (next_method.is_undefined()) {
                ctx.throw_type_error("Iterator next is undefined");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_remaining__", Value(limit));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    Value iter_val = self->get_property("__ih_iter__");
                    double remaining = self->get_property("__ih_remaining__").to_number();
                    if (remaining <= 0) { iterator_helper_close(ctx, iter_val); self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                    self->set_property("__ih_remaining__", Value(remaining - 1));
                    Value next_method = self->get_property("__ih_next__");
                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                    return Value(make_iter_result(val, false));
                }, 0);
            set_guarded_next(helper, std::move(next_fn));
            return Value(helper);
        }, 1);
    { PropertyDescriptor _d(Value(iter_take_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("take", _d); }

    auto iter_drop_fn = ObjectFactory::create_native_function("drop",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("drop called on non-object"); return Value(); }
            double num_limit = args.empty() ? std::nan("") : args[0].to_number();
            if (ctx.has_exception()) { iterator_helper_close(ctx, Value(iter)); return Value(); }
            if (std::isnan(num_limit)) {
                ctx.throw_range_error("Invalid count");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            double limit = (std::isinf(num_limit) && num_limit > 0) ? num_limit : std::trunc(num_limit);
            if (limit < 0) {
                ctx.throw_range_error("Invalid count");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (next_method.is_undefined()) {
                ctx.throw_type_error("Iterator next is undefined");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_remaining__", Value(limit));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    Value iter_val = self->get_property("__ih_iter__");
                    Value next_method = self->get_property("__ih_next__");
                    double remaining = self->get_property("__ih_remaining__").to_number();
                    while (remaining > 0) {
                        auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                        (void)val;
                        remaining -= 1;
                        self->set_property("__ih_remaining__", Value(remaining));
                        if (ctx.has_exception()) return Value();
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                    }
                    auto [val, done] = iterator_helper_step(ctx, iter_val, next_method);
                    if (ctx.has_exception()) return Value();
                    if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }
                    return Value(make_iter_result(val, false));
                }, 0);
            set_guarded_next(helper, std::move(next_fn));
            return Value(helper);
        }, 1);
    { PropertyDescriptor _d(Value(iter_drop_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("drop", _d); }

    auto iter_flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("flatMap called on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("flatMap requires a callable");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }
            Value next_method = iter->get_property("next");
            if (ctx.has_exception()) return Value();
            if (next_method.is_undefined()) {
                ctx.throw_type_error("Iterator next is undefined");
                iterator_helper_close(ctx, Value(iter));
                return Value();
            }

            Object* helper = create_iterator_helper_base(iterator_proto_ptr, Value(iter), next_method);
            helper->set_property("__ih_fn__", args[0]);
            helper->set_property("__ih_counter__", Value(0.0));
            helper->set_property("__ih_inner__", Value());
            helper->set_property("__ih_inner_next__", Value());

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    Value mapper_val = self->get_property("__ih_fn__");
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
                        if (done) { self->set_property("__ih_iter__", Value()); return Value(make_iter_result(Value(), true)); }

                        Value mapped = mapper_val.as_function()->call(ctx, {val, Value(counter)}, Value());
                        self->set_property("__ih_counter__", Value(counter + 1));
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        if (!mapped.is_object() && !mapped.is_function()) {
                            ctx.throw_type_error("flatMap mapper result must be an object");
                            iterator_helper_close(ctx, iter_val);
                            return Value();
                        }

                        // GetIteratorFlattenable: if @@iterator is absent, `mapped` itself is
                        // used as the inner iterator (its own `next` drives iteration directly).
                        Object* mapped_obj = mapped.is_function() ? static_cast<Object*>(mapped.as_function()) : mapped.as_object();
                        Symbol* fmap_iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                        Value iter_fn = fmap_iter_sym ? mapped_obj->get_property(fmap_iter_sym->to_property_key()) : Value();
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        Value inner;
                        if (iter_fn.is_undefined() || iter_fn.is_null()) {
                            inner = mapped;
                        } else if (!iter_fn.is_function()) {
                            ctx.throw_type_error("[Symbol.iterator] is not a function");
                            iterator_helper_close(ctx, iter_val);
                            return Value();
                        } else {
                            inner = iter_fn.as_function()->call(ctx, {}, mapped);
                            if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                            if (!inner.is_object() && !inner.is_function()) {
                                ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                                iterator_helper_close(ctx, iter_val);
                                return Value();
                            }
                        }
                        Object* inner_obj = inner.is_function() ? static_cast<Object*>(inner.as_function()) : inner.as_object();
                        Value inner_next_method = inner_obj->get_property("next");
                        if (ctx.has_exception()) { iterator_helper_close(ctx, iter_val); return Value(); }
                        self->set_property("__ih_inner__", inner);
                        self->set_property("__ih_inner_next__", inner_next_method);
                        // loop: pull from the freshly-set inner iterator next time around
                    }
                }, 0);
            set_guarded_next(helper, std::move(next_fn));
            return Value(helper);
        }, 1);
    { PropertyDescriptor _d(Value(iter_flatMap_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable|PropertyAttributes::Configurable)); iterator_prototype->set_property_descriptor("flatMap", _d); }

    // Add @@iterator to prototype (writable:true, enumerable:false, configurable:true per spec)
    {
        Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
        if (iter_sym) {
            auto self_iter = ObjectFactory::create_native_function("[Symbol.iterator]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    // Spec: return the raw this value.
                    Value prim = ctx.get_binding("__primitive_this__");
                    if (prim.is_number() || prim.is_string() || prim.is_boolean() ||
                        prim.is_bigint() || prim.is_symbol()) return prim;
                    if (ctx.original_this_was_nullish()) return Value();
                    try { return ctx.get_binding("this"); } catch (...) {}
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor sym_iter_desc(Value(self_iter.release()), PropertyAttributes::BuiltinFunction);
            iterator_prototype->set_property_descriptor(iter_sym->to_property_key(), sym_iter_desc);
        }
    }

    // Iterator.prototype[Symbol.toStringTag]: accessor with same SetterThatIgnoresPrototypeProperties semantics.
    {
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            Object* iter_proto_home = iterator_prototype.get();
            auto tag_get = ObjectFactory::create_native_function("get [Symbol.toStringTag]",
                [](Context& ctx, const std::vector<Value>&) -> Value { (void)ctx; return Value(std::string("Iterator")); }, 0);
            std::string tag_key = tag_sym->to_property_key();
            auto tag_set = ObjectFactory::create_native_function("set [Symbol.toStringTag]",
                [iter_proto_home, tag_key](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self || ctx.original_this_was_nullish() || ctx.original_this_was_primitive()) {
                        ctx.throw_type_error("[Symbol.toStringTag] setter: this is not an object"); return Value();
                    }
                    if (self == iter_proto_home || self == Iterator::s_iterator_prototype_) {
                        ctx.throw_type_error("Cannot assign to read only property");
                        return Value();
                    }
                    Value v = args.empty() ? Value() : args[0];
                    self->set_property(tag_key, v);
                    return Value();
                }, 1);
            PropertyDescriptor tag_desc;
            tag_desc.set_getter(tag_get.release());
            tag_desc.set_setter(tag_set.release());
            tag_desc.set_enumerable(false);
            tag_desc.set_configurable(true);
            iterator_prototype->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
    }

    // Iterator.prototype.constructor: accessor per ES2022+ class semantics.
    // The setter implements SetterThatIgnoresPrototypeProperties(%IteratorPrototype%, "constructor", v):
    // throws if this is not an object, throws if this IS %IteratorPrototype% (non-writable emulation),
    // otherwise creates/sets the property on this.
    {
        Object* iter_ctor_raw = iterator_constructor.get();
        Object* iter_proto_home = iterator_prototype.get();
        auto ctor_get = ObjectFactory::create_native_function("get constructor",
            [iter_ctor_raw](Context& ctx, const std::vector<Value>&) -> Value {
                (void)ctx; return Value(iter_ctor_raw);
            }, 0);
        auto ctor_set = ObjectFactory::create_native_function("set constructor",
            [iter_proto_home](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* self = ctx.get_this_binding();
                if (!self || ctx.original_this_was_nullish() || ctx.original_this_was_primitive()) {
                    ctx.throw_type_error("constructor setter: this is not an object"); return Value();
                }
                if (self == iter_proto_home || self == Iterator::s_iterator_prototype_) {
                    ctx.throw_type_error("Cannot assign to read only property 'constructor'");
                    return Value();
                }
                Value v = args.empty() ? Value() : args[0];
                // If this has an own descriptor for "constructor", use Set; else CreateDataPropertyOrThrow.
                PropertyDescriptor existing = self->get_property_descriptor("constructor");
                if (existing.is_data_descriptor() || existing.is_accessor_descriptor()) {
                    self->set_property("constructor", v);
                } else {
                    PropertyDescriptor new_desc(v, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
                    self->set_property_descriptor("constructor", new_desc);
                }
                if (ctx.has_exception()) return Value();
                return Value();
            }, 1);
        PropertyDescriptor ctor_desc;
        ctor_desc.set_getter(ctor_get.release());
        ctor_desc.set_setter(ctor_set.release());
        ctor_desc.set_enumerable(false);
        ctor_desc.set_configurable(true);
        iterator_prototype->set_property_descriptor("constructor", ctor_desc);
    }

    // %WrapForValidIteratorPrototype%: shared prototype for all non-Iterator-instance from() results.
    // Its [[Prototype]] is Iterator.prototype; it holds "next" and "return" delegating to the inner iter.
    auto wrap_proto = ObjectFactory::create_object();
    wrap_proto->set_prototype(iterator_proto_ptr);
    Object* wrap_proto_raw = wrap_proto.get();

    // WrapForValidIteratorPrototype.next: delegates to stored __wfvi_next__ called with __wfvi_iter__.
    {
        auto wfvi_next = ObjectFactory::create_native_function("next",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Object* self = ctx.get_this_binding();
                if (!self) { ctx.throw_type_error("next on non-object"); return Value(); }
                Value iter = self->get_property("__wfvi_iter__");
                Value nxt = self->get_property("__wfvi_next__");
                Value result = nxt.as_function()->call(ctx, {}, iter);
                if (ctx.has_exception()) return Value();
                if (!result.is_object()) { ctx.throw_type_error("Iterator result is not an object"); return Value(); }
                return result;
            }, 0);
        PropertyDescriptor d(Value(wfvi_next.release()), PropertyAttributes::BuiltinFunction);
        wrap_proto->set_property_descriptor("next", d);
    }
    // WrapForValidIteratorPrototype.return: reads "return" from inner at call time (GetMethod per call).
    {
        auto wfvi_return = ObjectFactory::create_native_function("return",
            [wrap_proto_raw](Context& ctx, const std::vector<Value>&) -> Value {
                Object* self = ctx.get_this_binding();
                // Validate receiver: must be an instance whose prototype IS %WrapForValidIteratorPrototype%.
                if (!self || self->get_prototype() != wrap_proto_raw) {
                    ctx.throw_type_error("Iterator.from return: incompatible receiver");
                    return Value();
                }
                Value iter = self->get_property("__wfvi_iter__");
                if (!iter.is_object() && !iter.is_function()) return Value(make_iter_result(Value(), true));
                Object* iter_obj = iter.is_function() ? static_cast<Object*>(iter.as_function()) : iter.as_object();
                Value ret_method = iter_obj->get_property("return");
                if (ctx.has_exception()) return Value();
                if (ret_method.is_undefined() || ret_method.is_null())
                    return Value(make_iter_result(Value(), true));
                if (!ret_method.is_function()) { ctx.throw_type_error("return is not a function"); return Value(); }
                return ret_method.as_function()->call(ctx, {}, iter);
            }, 0);
        PropertyDescriptor d(Value(wfvi_return.release()), PropertyAttributes::BuiltinFunction);
        wrap_proto->set_property_descriptor("return", d);
    }
    // Store wrap_proto on the constructor for SubClass.from() to find.
    iterator_constructor->set_property("__wfvi_proto__", Value(wrap_proto.release()));

    // Static Iterator.from ( O )
    auto iterator_from = ObjectFactory::create_native_function("from",
        [wrap_proto_raw, iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value { (void)iterator_proto_ptr;
            Value O = args.empty() ? Value() : args[0];

            // Primitives except String → TypeError.
            if (!O.is_object() && !O.is_function()) {
                if (!O.is_string()) {
                    ctx.throw_type_error("Iterator.from: argument must be an object or string");
                    return Value();
                }
                // String: box so we can call [Symbol.iterator] on it via the object path below.
                // GetIteratorFlattenable for strings: O[Symbol.iterator] exists → call it → string iterator.
                // We represent a string as a temporary JS value; for now treat as unknown iterable.
            }

            // GetIteratorFlattenable(O, "reject-strings" = false):
            Object* obj = nullptr;
            if (O.is_object()) obj = O.as_object();
            else if (O.is_function()) obj = static_cast<Object*>(O.as_function());

            Value inner_iter;
            Value inner_next;

            if (obj) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                Value method = iter_sym ? obj->get_property(iter_sym->to_property_key()) : Value();
                if (ctx.has_exception()) return Value();

                if (method.is_undefined() || method.is_null()) {
                    // No @@iterator: use O itself as the raw iterator.
                    inner_iter = O;
                    inner_next = obj->get_property("next");
                    if (ctx.has_exception()) return Value();
                } else if (!method.is_function()) {
                    ctx.throw_type_error("[Symbol.iterator] is not callable");
                    return Value();
                } else {
                    inner_iter = method.as_function()->call(ctx, {}, O);
                    if (ctx.has_exception()) return Value();
                    if (!inner_iter.is_object() && !inner_iter.is_function()) {
                        ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                        return Value();
                    }
                    Object* ii = inner_iter.is_function() ? static_cast<Object*>(inner_iter.as_function()) : inner_iter.as_object();
                    inner_next = ii->get_property("next");
                    if (ctx.has_exception()) return Value();
                }
            } else if (O.is_string()) {
                // String primitive: GetV(O, Symbol.iterator) with O (the primitive) as receiver for getters.
                Value str_ctor = ctx.get_binding("String");
                if (str_ctor.is_function()) {
                    Value str_proto = str_ctor.as_function()->get_property("prototype");
                    if (str_proto.is_object()) {
                        Symbol* iter_sym2 = Symbol::get_well_known(Symbol::ITERATOR);
                        if (iter_sym2) {
                            // GetV: get the property descriptor, call getter with O (primitive) as this.
                            PropertyDescriptor d = str_proto.as_object()->get_property_descriptor(iter_sym2->to_property_key());
                            Value iter_method2;
                            if (d.is_accessor_descriptor() && d.get_getter()) {
                                Function* getter = dynamic_cast<Function*>(d.get_getter());
                                if (getter) { iter_method2 = getter->call(ctx, {}, O); if (ctx.has_exception()) return Value(); }
                            } else if (d.is_data_descriptor()) {
                                iter_method2 = d.get_value();
                            } else {
                                iter_method2 = str_proto.as_object()->get_property(iter_sym2->to_property_key());
                            }
                            if (iter_method2.is_function()) {
                                inner_iter = iter_method2.as_function()->call(ctx, {}, O);
                                if (ctx.has_exception()) return Value();
                                if (inner_iter.is_object()) {
                                    inner_next = inner_iter.as_object()->get_property("next");
                                    if (ctx.has_exception()) return Value();
                                }
                            }
                        }
                    }
                }
                if (!inner_iter.is_object()) {
                    ctx.throw_type_error("Iterator.from: could not get string iterator");
                    return Value();
                }
            } else {
                ctx.throw_type_error("Iterator.from: unsupported argument type");
                return Value();
            }

            // If the inner_iter already inherits from Iterator.prototype, return it directly.
            Object* inner_obj = inner_iter.is_function() ? static_cast<Object*>(inner_iter.as_function()) : (inner_iter.is_object() ? inner_iter.as_object() : nullptr);
            if (inner_obj) {
                Object* proto = inner_obj->get_prototype();
                while (proto) {
                    if (proto == iterator_proto_ptr) {
                        // Already an Iterator instance -- return as-is.
                        return inner_iter;
                    }
                    proto = proto->get_prototype();
                }
            }

            // Wrap in a WrapForValidIteratorPrototype instance.
            auto wrapper = ObjectFactory::create_object();
            wrapper->set_prototype(wrap_proto_raw);
            wrapper->set_property("__wfvi_iter__", inner_iter);
            wrapper->set_property("__wfvi_next__", inner_next);
            return Value(wrapper.release());
        }, 1);
    {
        PropertyDescriptor from_desc(Value(iterator_from.release()), PropertyAttributes::BuiltinFunction);
        iterator_constructor->set_property_descriptor("from", from_desc);
    }

    // Static Iterator.concat(...items): lazily exhausts each item in turn; [Symbol.iterator] is
    // resolved eagerly per item but only called once that item is reached.
    auto iterator_concat = ObjectFactory::create_native_function("concat",
        [iterator_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value { (void)iterator_proto_ptr;
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
            helper->set_prototype(Iterator::s_iterator_prototype_);
            helper->set_property("__ic_items__", Value(items.release()));
            helper->set_property("__ic_methods__", Value(methods.release()));
            helper->set_property("__ic_index__", Value(0.0));
            helper->set_property("__ic_inner__", Value());
            helper->set_property("__ic_inner_next__", Value());
            helper->set_property("__ic_running__", Value(false));

            auto next_fn = ObjectFactory::create_native_function("next",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self) { ctx.throw_type_error("next called on non-object"); return Value(); }
                    if (self->get_property("__ic_running__").to_boolean()) {
                        ctx.throw_type_error("Iterator.concat helper is already running");
                        return Value();
                    }
                    self->set_property("__ic_running__", Value(true));
                    while (true) {
                        Value inner_val = self->get_property("__ic_inner__");
                        if (inner_val.is_object() || inner_val.is_function()) {
                            Value inner_next = self->get_property("__ic_inner_next__");
                            auto [val, done] = iterator_helper_step(ctx, inner_val, inner_next);
                            if (ctx.has_exception()) { self->set_property("__ic_running__", Value(false)); return Value(); }
                            if (!done) { self->set_property("__ic_running__", Value(false)); return Value(make_iter_result(val, false)); }
                            self->set_property("__ic_inner__", Value());
                            self->set_property("__ic_inner_next__", Value());
                        }
                        double index = self->get_property("__ic_index__").to_number();
                        Object* items_obj = self->get_property("__ic_items__").as_object();
                        double total = items_obj->get_property("length").to_number();
                        if (index >= total) { self->set_property("__ic_running__", Value(false)); return Value(make_iter_result(Value(), true)); }
                        Value item = items_obj->get_property(std::to_string((uint32_t)index));
                        Object* methods_obj = self->get_property("__ic_methods__").as_object();
                        Value method = methods_obj->get_property(std::to_string((uint32_t)index));
                        self->set_property("__ic_index__", Value(index + 1));

                        Value inner = method.as_function()->call(ctx, {}, item);
                        if (ctx.has_exception()) { self->set_property("__ic_running__", Value(false)); return Value(); }
                        if (!inner.is_object() && !inner.is_function()) {
                            self->set_property("__ic_running__", Value(false));
                            ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                            return Value();
                        }
                        Object* inner_obj = inner.is_function() ? static_cast<Object*>(inner.as_function()) : inner.as_object();
                        Value inner_next_method = inner_obj->get_property("next");
                        if (ctx.has_exception()) { self->set_property("__ic_running__", Value(false)); return Value(); }
                        self->set_property("__ic_inner__", inner);
                        self->set_property("__ic_inner_next__", inner_next_method);
                    }
                }, 0);
            helper->set_property("next", Value(next_fn.release()));

            auto return_fn = ObjectFactory::create_native_function("return",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (self) {
                        // A reentrant return() (e.g. from the inner iterator's own
                        // return) sees the generator in the executing state.
                        if (self->get_property("__ic_running__").to_boolean()) {
                            ctx.throw_type_error("Iterator.concat helper is already running");
                            return Value();
                        }
                        Value inner_val = self->get_property("__ic_inner__");
                        if (inner_val.is_object() || inner_val.is_function()) {
                            self->set_property("__ic_running__", Value(true));
                            iterator_helper_close(ctx, inner_val);
                            self->set_property("__ic_running__", Value(false));
                            self->set_property("__ic_inner__", Value());
                        }
                        Object* items_obj = self->get_property("__ic_items__").as_object();
                        double total = items_obj ? items_obj->get_property("length").to_number() : 0;
                        self->set_property("__ic_index__", Value(total));
                        if (ctx.has_exception()) return Value();
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
                    iterator_helper_close(ctx, outer_iter);
                    return Value();
                }
                Object* item_obj = item.is_function() ? static_cast<Object*>(item.as_function()) : item.as_object();
                Value method = iter_sym ? item_obj->get_property(iter_sym->to_property_key()) : Value();
                Value inner_iter = item;
                // GetIteratorFlattenable errors below should also close the outer iter.
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); iterator_helper_close(ctx, outer_iter); return Value(); }
                if (!method.is_undefined() && !method.is_null()) {
                    if (!method.is_function()) {
                        ctx.throw_type_error("[Symbol.iterator] is not a function");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        iterator_helper_close(ctx, outer_iter);
                        return Value();
                    }
                    inner_iter = method.as_function()->call(ctx, {}, item);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); iterator_helper_close(ctx, outer_iter); return Value(); }
                    if (!inner_iter.is_object() && !inner_iter.is_function()) {
                        ctx.throw_type_error("Result of [Symbol.iterator] is not an object");
                        for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it);
                        iterator_helper_close(ctx, outer_iter);
                        return Value();
                    }
                }
                Object* inner_obj = inner_iter.is_function() ? static_cast<Object*>(inner_iter.as_function()) : inner_iter.as_object();
                Value inner_next = inner_obj->get_property("next");
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); iterator_helper_close(ctx, outer_iter); return Value(); }
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
            helper->set_prototype(Iterator::s_iterator_prototype_);
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
            helper->set_property("__iz_running__", Value(false));

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
            // A Proxy must be observed through its [[OwnPropertyKeys]] and
            // [[GetOwnProperty]] traps, not the plain object tables.
            bool iterables_is_proxy = iterables_obj->get_type() == Object::ObjectType::Proxy;
            std::vector<std::string> own_keys;
            if (iterables_is_proxy) {
                own_keys = static_cast<Proxy*>(iterables_obj)->own_keys_trap();
                if (ctx.has_exception()) return Value();
            } else {
                own_keys = iterables_obj->get_own_property_keys();
            }
            for (const auto& key : own_keys) {
                PropertyDescriptor desc = iterables_is_proxy
                    ? static_cast<Proxy*>(iterables_obj)->get_own_property_descriptor_trap(Value(key))
                    : iterables_obj->get_property_descriptor(key);
                if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
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
                // For zipKeyed, padding is a plain object: read Get(paddingOption, key) per key.
                Object* padding_obj = padding_option.is_function() ? static_cast<Object*>(padding_option.as_function()) : padding_option.as_object();
                for (uint32_t i = 0; i < iter_count; i++) {
                    Value pad_val = padding_obj->get_property(keys[i]);
                    if (ctx.has_exception()) { for (auto it = iters.rbegin(); it != iters.rend(); ++it) iterator_helper_close(ctx, *it); return Value(); }
                    padding[i] = pad_val;
                }
            }

            auto helper = ObjectFactory::create_object();
            helper->set_prototype(Iterator::s_iterator_prototype_);
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
            helper->set_property("__iz_running__", Value(false));

            auto next_fn = ObjectFactory::create_native_function("next", iterator_zip_step, 0);
            helper->set_property("next", Value(next_fn.release()));
            auto return_fn = ObjectFactory::create_native_function("return", iterator_zip_return, 0);
            helper->set_property("return", Value(return_fn.release()));

            return Value(helper.release());
        }, 1);
    iterator_constructor->set_property("zipKeyed", Value(iterator_zipKeyed.release()), PropertyAttributes::BuiltinFunction);

    Object* iter_proto_raw = iterator_prototype.get();
    // Iterator.prototype is non-writable, non-enumerable, non-configurable per spec
    {
        PropertyDescriptor proto_desc(Value(iterator_prototype.release()), PropertyAttributes::None);
        iterator_constructor->set_property_descriptor("prototype", proto_desc);
    }
    // Iterator.length = 0 (writable:false, enumerable:false, configurable:true)
    {
        PropertyDescriptor len_desc(Value(0.0), PropertyAttributes::Configurable);
        iterator_constructor->set_property_descriptor("length", len_desc);
    }
    ctx.register_built_in_object("Iterator", iterator_constructor.release());

    if (Iterator::s_iterator_prototype_ && Iterator::s_iterator_prototype_ != iter_proto_raw) {
        Iterator::s_iterator_prototype_->set_prototype(iter_proto_raw);
    }
}

} // namespace Quanta
