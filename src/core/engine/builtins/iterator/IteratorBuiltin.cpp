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
                a->set_property("length",Value((double)i)); return Value(a.release());
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
            arr->set_property("length", Value((double)idx));
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

    iterator_constructor->set_property("prototype", Value(iterator_prototype.release()));
    ctx.register_built_in_object("Iterator", iterator_constructor.release());
}

} // namespace Quanta
