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

namespace Quanta {

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

        auto iter_map2 = ObjectFactory::create_native_function("map",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("map");return Value();}
                Function* cb=args[0].as_function(); auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; Value r=cb->call(ctx,{v,Value((double)i)},Value()); if(ctx.has_exception())break; a->set_property(std::to_string(i++),r);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("map", Value(iter_map2.release()));

        auto iter_filter2 = ObjectFactory::create_native_function("filter",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("filter");return Value();}
                Function* cb=args[0].as_function(); auto a=ObjectFactory::create_array(); uint32_t i=0,o=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v,Value((double)i++)},Value()).to_boolean())a->set_property(std::to_string(o++),v);}
                a->set_property("length",Value((double)o)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("filter", Value(iter_filter2.release()));

        auto iter_take2 = ObjectFactory::create_native_function("take",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it){return Value();}
                uint32_t lim=args.empty()?0:(uint32_t)args[0].to_number(); auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(i<lim){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("take", Value(iter_take2.release()));

        auto iter_drop2 = ObjectFactory::create_native_function("drop",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it){return Value();}
                uint32_t sk=args.empty()?0:(uint32_t)args[0].to_number();
                for(uint32_t i=0;i<sk;i++){auto[v,d]=call_iter_next(ctx,it);(void)v;if(ctx.has_exception()||d)break;}
                auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("drop", Value(iter_drop2.release()));
    }
}

void register_iterator_constructor(Context& ctx) {
    auto iterator_constructor = ObjectFactory::create_native_function("Iterator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto iterator_obj = ObjectFactory::create_object();

            Object* constructor = ctx.get_this_binding();
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

    // map - returns a new iterator
    auto iter_map_fn = ObjectFactory::create_native_function("map",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("map requires function"); return Value(); }
            Function* mapper = args[0].as_function();
            // Collect all mapped values into array-backed iterator
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value mapped = mapper->call(ctx, {val, Value((double)idx)}, Value());
                if (ctx.has_exception()) break;
                arr->set_property(std::to_string(idx), mapped);
                idx++;
            }
            arr->set_property("length", Value((double)idx));
            // Return array iterator
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("map", Value(iter_map_fn.release()));

    // filter
    auto iter_filter_fn = ObjectFactory::create_native_function("filter",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("filter requires function"); return Value(); }
            Function* pred = args[0].as_function();
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0, out = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = pred->call(ctx, {val, Value((double)idx++)}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) { arr->set_property(std::to_string(out++), val); }
            }
            arr->set_property("length", Value((double)out));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("filter", Value(iter_filter_fn.release()));

    // take
    auto iter_take_fn = ObjectFactory::create_native_function("take",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("take on non-object"); return Value(); }
            uint32_t limit = args.empty() ? 0 : (uint32_t)args[0].to_number();
            auto arr = ObjectFactory::create_array();
            for (uint32_t i = 0; i < limit; i++) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(i), val);
                arr->set_property("length", Value((double)(i+1)));
            }
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("take", Value(iter_take_fn.release()));

    // drop
    auto iter_drop_fn = ObjectFactory::create_native_function("drop",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("drop on non-object"); return Value(); }
            uint32_t skip = args.empty() ? 0 : (uint32_t)args[0].to_number();
            for (uint32_t i = 0; i < skip; i++) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                (void)val;
            }
            // Return remaining iterator
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(idx++), val);
            }
            arr->set_property("length", Value((double)idx));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("drop", Value(iter_drop_fn.release()));

    // flatMap
    auto iter_flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("flatMap requires function"); return Value(); }
            Function* mapper = args[0].as_function();
            auto arr = ObjectFactory::create_array();
            uint32_t out = 0, idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value mapped = mapper->call(ctx, {val, Value((double)idx++)}, Value());
                if (ctx.has_exception()) break;
                // Iterate over mapped value
                if (mapped.is_object()) {
                    Value iter_sym_val = mapped.as_object()->get_property("Symbol(Symbol.iterator)");
                    if (!iter_sym_val.is_function()) {
                        // Try string "@@iterator"
                        iter_sym_val = mapped.as_object()->get_property("@@iterator");
                    }
                    if (iter_sym_val.is_function()) {
                        Value inner = iter_sym_val.as_function()->call(ctx, {}, mapped);
                        if (inner.is_object()) {
                            while (true) {
                                auto [iv, id] = call_next(ctx, inner.as_object());
                                if (ctx.has_exception() || id) break;
                                arr->set_property(std::to_string(out++), iv);
                            }
                            continue;
                        }
                    }
                }
                arr->set_property(std::to_string(out++), mapped);
            }
            arr->set_property("length", Value((double)out));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
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
