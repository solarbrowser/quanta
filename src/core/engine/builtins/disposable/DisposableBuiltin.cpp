/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/DisposableBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"

namespace Quanta {

namespace {

// Internal slots use "[[...]]" keys so they never surface as observable own keys.
constexpr const char* kSyncState = "[[DisposableState]]";
constexpr const char* kSyncStack = "[[DisposableStack]]";
constexpr const char* kAsyncState = "[[AsyncDisposableState]]";
constexpr const char* kAsyncStack = "[[AsyncDisposableStack]]";

// Stack entry kinds: how the captured method gets invoked at dispose time.
enum EntryKind { kUse = 0, kAdopt = 1, kDefer = 2 };

Object* resolve_new_target_prototype(Context& ctx, Object* default_proto) {
    Value nt = ctx.get_new_target();
    if (!nt.is_object() && !nt.is_function()) return default_proto;
    Object* nt_obj = nt.is_function() ? static_cast<Object*>(nt.as_function()) : nt.as_object();
    Value p = nt_obj->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    if (p.is_object()) return p.as_object();
    if (p.is_function()) return static_cast<Object*>(p.as_function());
    return default_proto;
}

// RequireInternalSlot: the slots are string-keyed properties, so an ordinary object
// carrying the same literal key must still be rejected (value types are validated).
bool has_valid_stack_slots(Object* obj, const char* state_key, const char* stack_key) {
    Value st = obj->get_property(state_key);
    if (!st.is_string()) return false;
    const std::string& s = st.as_string()->str();
    if (s != "pending" && s != "disposed") return false;
    return obj->get_property(stack_key).is_object();
}

Object* require_stack_this(Context& ctx, const char* state_key, const char* stack_key, const char* name) {
    Object* obj = ctx.get_this_binding();
    if (!obj || ctx.original_this_was_primitive() || ctx.original_this_was_nullish() ||
        !has_valid_stack_slots(obj, state_key, stack_key)) {
        ctx.throw_type_error(std::string(name) + " requires a DisposableStack this");
        return nullptr;
    }
    return obj;
}

bool is_disposed(Object* stack_obj, const char* state_key) {
    return stack_obj->get_property(state_key).to_string() == "disposed";
}

Object* make_entry(const Value& method, const Value& receiver, EntryKind kind, const Value& arg, bool wrapped_sync) {
    auto entry = ObjectFactory::create_object();
    entry->set_property("m", method);
    entry->set_property("r", receiver);
    entry->set_property("k", Value(static_cast<double>(kind)));
    entry->set_property("v", arg);
    entry->set_property("w", Value(wrapped_sync));
    return entry.release();
}

void push_entry(Object* stack_obj, const char* stack_key, Object* entry) {
    Value arr = stack_obj->get_property(stack_key);
    if (arr.is_object()) arr.as_object()->push(Value(entry));
}

// Calls one entry's disposer; any exception is left pending in ctx.
Value invoke_entry(Context& ctx, Object* entry) {
    Value method = entry->get_property("m");
    if (!method.is_function()) return Value();
    EntryKind kind = static_cast<EntryKind>(static_cast<int>(entry->get_property("k").to_number()));
    switch (kind) {
        case kUse:   return method.as_function()->call(ctx, {}, entry->get_property("r"));
        case kAdopt: return method.as_function()->call(ctx, {entry->get_property("v")}, Value());
        case kDefer: return method.as_function()->call(ctx, {}, Value());
    }
    return Value();
}

Value make_suppressed_error(Context& ctx, const Value& error, const Value& suppressed) {
    Value ctor = ctx.get_binding("SuppressedError");
    if (ctor.is_function()) {
        Value r = ctor.as_function()->construct(ctx, {error, suppressed, Value()});
        if (!ctx.has_exception()) return r;
        ctx.clear_exception();
    }
    return error;
}

Value make_type_error_value(Context& ctx, const std::string& msg) {
    Value ctor = ctx.get_binding("TypeError");
    if (ctor.is_function()) {
        Value r = ctor.as_function()->construct(ctx, {Value(msg)});
        if (!ctx.has_exception()) return r;
        ctx.clear_exception();
    }
    return Value(msg);
}

// GetDisposeMethod: @@dispose for sync; @@asyncDispose falling back to a wrapped
// @@dispose for async. Returns false with a pending TypeError when neither exists.
bool get_dispose_method(Context& ctx, const Value& value, bool async_hint, Value& method_out, bool& wrapped_sync_out) {
    Object* val_obj = value.is_function() ? static_cast<Object*>(value.as_function()) : value.as_object();
    wrapped_sync_out = false;
    if (async_hint) {
        Symbol* async_sym = Symbol::get_well_known(Symbol::ASYNC_DISPOSE);
        Value m = async_sym ? val_obj->get_property(async_sym->to_property_key()) : Value();
        if (ctx.has_exception()) return false;
        if (!m.is_undefined() && !m.is_null()) {
            if (!m.is_function()) { ctx.throw_type_error("Symbol.asyncDispose is not callable"); return false; }
            method_out = m;
            return true;
        }
        wrapped_sync_out = true;
    }
    Symbol* dispose_sym = Symbol::get_well_known(Symbol::DISPOSE);
    Value m = dispose_sym ? val_obj->get_property(dispose_sym->to_property_key()) : Value();
    if (ctx.has_exception()) return false;
    if (m.is_undefined() || m.is_null()) {
        ctx.throw_type_error(async_hint ? "Object is not async disposable" : "Object is not disposable");
        return false;
    }
    if (!m.is_function()) { ctx.throw_type_error("Symbol.dispose is not callable"); return false; }
    method_out = m;
    return true;
}

// DisposeResources for the sync stack: reverse order, exceptions folded into
// SuppressedError([[error]]=newer, [[suppressed]]=older), disposal never stops early.
Value dispose_sync_stack(Context& ctx, Object* stack_obj) {
    stack_obj->set_property(kSyncState, Value(std::string("disposed")));
    Value arr_val = stack_obj->get_property(kSyncStack);
    stack_obj->set_property(kSyncStack, Value(ObjectFactory::create_array(0).release()));
    if (!arr_val.is_object()) return Value();
    Object* arr = arr_val.as_object();
    bool has_error = false;
    Value error;
    for (int64_t i = static_cast<int64_t>(arr->get_length()) - 1; i >= 0; i--) {
        Value entry = arr->get_element(static_cast<uint32_t>(i));
        if (!entry.is_object()) continue;
        invoke_entry(ctx, entry.as_object());
        if (ctx.has_exception()) {
            Value new_error = ctx.get_exception();
            ctx.clear_exception();
            error = has_error ? make_suppressed_error(ctx, new_error, error) : new_error;
            has_error = true;
        }
    }
    if (has_error) {
        ctx.throw_exception(error, true);
    }
    return Value();
}

// Async disposal state machine: entries run in reverse, one per microtask step,
// each result awaited via promise chaining.
void async_dispose_step(Context& ctx, Object* state);

Function* make_step_handler(Object* state, bool is_rejection) {
    Value state_val(state);
    auto fn = ObjectFactory::create_native_function("",
        [state_val, is_rejection](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* st = state_val.as_object();
            if (is_rejection) {
                Value new_error = args.empty() ? Value() : args[0];
                Value old_error = st->get_property("e");
                bool had = st->get_property("h").to_boolean();
                st->set_property("e", had ? make_suppressed_error(ctx, new_error, old_error) : new_error);
                st->set_property("h", Value(true));
            }
            async_dispose_step(ctx, st);
            return Value();
        }, 1);
    // The state must stay GC-reachable through the handler sitting in the promise's records.
    fn->set_property("[[DisposeState]]", state_val);
    return fn.release();
}

void async_dispose_step(Context& ctx, Object* state) {
    Object* arr = state->get_property("s").as_object();
    Promise* result_promise = static_cast<Promise*>(state->get_property("p").as_object());
    while (true) {
        double idx = state->get_property("i").to_number();
        if (idx < 0) break;
        Value entry_val = arr->get_element(static_cast<uint32_t>(idx));
        state->set_property("i", Value(idx - 1.0));
        if (!entry_val.is_object()) continue;
        Object* entry = entry_val.as_object();
        // A method-less entry records `await using x = null/undefined`: no call,
        // but DisposeResources still owes one Await(undefined) at the end.
        if (!entry->get_property("m").is_function()) {
            state->set_property("n", Value(true));
            continue;
        }
        Value r = invoke_entry(ctx, entry);
        if (ctx.has_exception()) {
            Value new_error = ctx.get_exception();
            ctx.clear_exception();
            Value old_error = state->get_property("e");
            bool had = state->get_property("h").to_boolean();
            state->set_property("e", had ? make_suppressed_error(ctx, new_error, old_error) : new_error);
            state->set_property("h", Value(true));
            continue;
        }
        bool wrapped_sync = entry->get_property("w").to_boolean();
        Promise* chain;
        if (!wrapped_sync && r.is_object() && r.as_object()->get_type() == Object::ObjectType::Promise) {
            chain = static_cast<Promise*>(r.as_object());
        } else {
            chain = Promise::resolve(Value());
        }
        state->set_property("a", Value(true));
        chain->then(make_step_handler(state, false), make_step_handler(state, true));
        return;
    }
    if (state->get_property("n").to_boolean() && !state->get_property("a").to_boolean()) {
        state->set_property("a", Value(true));
        Promise* tick = Promise::resolve(Value());
        tick->then(make_step_handler(state, false), make_step_handler(state, true));
        return;
    }
    if (state->get_property("h").to_boolean()) {
        result_promise->reject(state->get_property("e"));
    } else {
        result_promise->fulfill(Value());
    }
}

Value dispose_async_stack(Context& ctx, Object* stack_obj) {
    stack_obj->set_property(kAsyncState, Value(std::string("disposed")));
    Value arr_val = stack_obj->get_property(kAsyncStack);
    stack_obj->set_property(kAsyncStack, Value(ObjectFactory::create_array(0).release()));

    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* promise = static_cast<Promise*>(promise_obj.release());

    auto state = ObjectFactory::create_object();
    state->set_property("s", arr_val.is_object() ? arr_val : Value(ObjectFactory::create_array(0).release()));
    state->set_property("i", Value(static_cast<double>(state->get_property("s").as_object()->get_length()) - 1.0));
    state->set_property("e", Value());
    state->set_property("h", Value(false));
    state->set_property("n", Value(false));
    state->set_property("a", Value(false));
    state->set_property("p", Value(promise));
    async_dispose_step(ctx, state.release());
    return Value(promise);
}

// Registers one of the two stack constructors with its full prototype.
void register_stack(Context& ctx, bool async) {
    const char* ctor_name = async ? "AsyncDisposableStack" : "DisposableStack";
    const char* state_key = async ? kAsyncState : kSyncState;
    const char* stack_key = async ? kAsyncStack : kSyncStack;

    auto prototype = ObjectFactory::create_object();
    Object* proto_ptr = prototype.get();

    auto make_stack_object = [state_key, stack_key](Object* proto) -> Object* {
        auto obj = ObjectFactory::create_object();
        obj->set_prototype(proto);
        obj->set_property(state_key, Value(std::string("pending")), PropertyAttributes::Writable);
        obj->set_property(stack_key, Value(ObjectFactory::create_array(0).release()), PropertyAttributes::Writable);
        return obj.release();
    };

    auto constructor = ObjectFactory::create_native_constructor(ctor_name,
        [proto_ptr, make_stack_object, ctor_name](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            if (!ctx.is_in_constructor_call()) {
                ctx.throw_type_error(std::string(ctor_name) + " requires 'new'");
                return Value();
            }
            Object* proto = resolve_new_target_prototype(ctx, proto_ptr);
            if (ctx.has_exception()) return Value();
            return Value(make_stack_object(proto));
        }, 0);

    auto add_method = [&](const char* name, uint32_t arity,
                          Value (*fn)(Context&, const std::vector<Value>&)) -> Function* {
        auto f = ObjectFactory::create_native_function(name, fn, arity);
        Function* raw = f.get();
        prototype->set_property_descriptor(name,
            PropertyDescriptor(Value(f.release()), PropertyAttributes::BuiltinFunction));
        return raw;
    };

    Function* dispose_fn_raw;
    if (!async) {
        dispose_fn_raw = add_method("dispose", 0, [](Context& ctx, const std::vector<Value>&) -> Value {
            Object* obj = require_stack_this(ctx, kSyncState, kSyncStack, "dispose");
            if (!obj) return Value();
            if (is_disposed(obj, kSyncState)) return Value();
            return dispose_sync_stack(ctx, obj);
        });
    } else {
        dispose_fn_raw = add_method("disposeAsync", 0, [](Context& ctx, const std::vector<Value>&) -> Value {
            // Abrupt this-validation rejects the returned promise instead of throwing.
            Object* obj = ctx.get_this_binding();
            if (!obj || ctx.original_this_was_primitive() || ctx.original_this_was_nullish() ||
                !has_valid_stack_slots(obj, kAsyncState, kAsyncStack)) {
                return Value(Promise::reject_static(make_type_error_value(ctx, "disposeAsync requires an AsyncDisposableStack this")));
            }
            if (is_disposed(obj, kAsyncState)) return Value(Promise::resolve(Value()));
            return dispose_async_stack(ctx, obj);
        });
    }

    {
        auto getter = ObjectFactory::create_native_function("get disposed",
            async ? +[](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* obj = require_stack_this(ctx, kAsyncState, kAsyncStack, "disposed");
                        if (!obj) return Value();
                        return Value(is_disposed(obj, kAsyncState));
                    }
                  : +[](Context& ctx, const std::vector<Value>&) -> Value {
                        Object* obj = require_stack_this(ctx, kSyncState, kSyncStack, "disposed");
                        if (!obj) return Value();
                        return Value(is_disposed(obj, kSyncState));
                    }, 0);
        PropertyDescriptor desc;
        desc.set_getter(getter.release());
        desc.set_enumerable(false);
        desc.set_configurable(true);
        prototype->set_property_descriptor("disposed", desc);
    }

    if (!async) {
        add_method("use", 1, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kSyncState, kSyncStack, "use");
            if (!obj) return Value();
            if (is_disposed(obj, kSyncState)) { ctx.throw_reference_error("DisposableStack already disposed"); return Value(); }
            Value value = args.empty() ? Value() : args[0];
            if (value.is_null() || value.is_undefined()) return value;
            if (!value.is_object() && !value.is_function()) { ctx.throw_type_error("use() value must be an object"); return Value(); }
            Value method;
            bool wrapped_sync;
            if (!get_dispose_method(ctx, value, false, method, wrapped_sync)) return Value();
            push_entry(obj, kSyncStack, make_entry(method, value, kUse, Value(), false));
            return value;
        });
        add_method("adopt", 2, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kSyncState, kSyncStack, "adopt");
            if (!obj) return Value();
            if (is_disposed(obj, kSyncState)) { ctx.throw_reference_error("DisposableStack already disposed"); return Value(); }
            Value on_dispose = args.size() > 1 ? args[1] : Value();
            if (!on_dispose.is_function()) { ctx.throw_type_error("onDispose must be callable"); return Value(); }
            Value value = args.empty() ? Value() : args[0];
            push_entry(obj, kSyncStack, make_entry(on_dispose, Value(), kAdopt, value, false));
            return value;
        });
        add_method("defer", 1, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kSyncState, kSyncStack, "defer");
            if (!obj) return Value();
            if (is_disposed(obj, kSyncState)) { ctx.throw_reference_error("DisposableStack already disposed"); return Value(); }
            Value on_dispose = args.empty() ? Value() : args[0];
            if (!on_dispose.is_function()) { ctx.throw_type_error("onDispose must be callable"); return Value(); }
            push_entry(obj, kSyncStack, make_entry(on_dispose, Value(), kDefer, Value(), false));
            return Value();
        });
    } else {
        add_method("use", 1, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kAsyncState, kAsyncStack, "use");
            if (!obj) return Value();
            if (is_disposed(obj, kAsyncState)) { ctx.throw_reference_error("AsyncDisposableStack already disposed"); return Value(); }
            Value value = args.empty() ? Value() : args[0];
            if (value.is_null() || value.is_undefined()) {
                // Recorded without a method: disposeAsync still awaits once for it.
                push_entry(obj, kAsyncStack, make_entry(Value(), Value(), kUse, Value(), false));
                return value;
            }
            if (!value.is_object() && !value.is_function()) { ctx.throw_type_error("use() value must be an object"); return Value(); }
            Value method;
            bool wrapped_sync;
            if (!get_dispose_method(ctx, value, true, method, wrapped_sync)) return Value();
            push_entry(obj, kAsyncStack, make_entry(method, value, kUse, Value(), wrapped_sync));
            return value;
        });
        add_method("adopt", 2, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kAsyncState, kAsyncStack, "adopt");
            if (!obj) return Value();
            if (is_disposed(obj, kAsyncState)) { ctx.throw_reference_error("AsyncDisposableStack already disposed"); return Value(); }
            Value on_dispose = args.size() > 1 ? args[1] : Value();
            if (!on_dispose.is_function()) { ctx.throw_type_error("onDisposeAsync must be callable"); return Value(); }
            Value value = args.empty() ? Value() : args[0];
            push_entry(obj, kAsyncStack, make_entry(on_dispose, Value(), kAdopt, value, false));
            return value;
        });
        add_method("defer", 1, [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = require_stack_this(ctx, kAsyncState, kAsyncStack, "defer");
            if (!obj) return Value();
            if (is_disposed(obj, kAsyncState)) { ctx.throw_reference_error("AsyncDisposableStack already disposed"); return Value(); }
            Value on_dispose = args.empty() ? Value() : args[0];
            if (!on_dispose.is_function()) { ctx.throw_type_error("onDisposeAsync must be callable"); return Value(); }
            push_entry(obj, kAsyncStack, make_entry(on_dispose, Value(), kDefer, Value(), false));
            return Value();
        });
    }

    {
        // move() builds the new stack directly against the intrinsic prototype
        // (OrdinaryCreateFromConstructor(%...%)), never through the constructor.
        Value proto_holder(proto_ptr);
        auto move_fn = ObjectFactory::create_native_function("move",
            [proto_holder, state_key, stack_key, make_stack_object, ctor_name](Context& ctx, const std::vector<Value>&) -> Value {
                Object* obj = require_stack_this(ctx, state_key, stack_key, "move");
                if (!obj) return Value();
                if (is_disposed(obj, state_key)) {
                    ctx.throw_reference_error(std::string(ctor_name) + " already disposed");
                    return Value();
                }
                Object* new_stack = make_stack_object(proto_holder.as_object());
                new_stack->set_property(stack_key, obj->get_property(stack_key));
                obj->set_property(stack_key, Value(ObjectFactory::create_array(0).release()));
                obj->set_property(state_key, Value(std::string("disposed")));
                return Value(new_stack);
            }, 0);
        prototype->set_property_descriptor("move",
            PropertyDescriptor(Value(move_fn.release()), PropertyAttributes::BuiltinFunction));
    }

    // @@dispose / @@asyncDispose are the same function objects as dispose/disposeAsync.
    Symbol* alias_sym = Symbol::get_well_known(async ? Symbol::ASYNC_DISPOSE : Symbol::DISPOSE);
    if (alias_sym) {
        prototype->set_property_descriptor(alias_sym->to_property_key(),
            PropertyDescriptor(Value(dispose_fn_raw),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));
    }

    Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (tag_sym) {
        prototype->set_property_descriptor(tag_sym->to_property_key(),
            PropertyDescriptor(Value(std::string(ctor_name)), PropertyAttributes::Configurable));
    }

    prototype->set_property_descriptor("constructor",
        PropertyDescriptor(Value(constructor.get()),
            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));
    constructor->set_property("prototype", Value(prototype.release()), PropertyAttributes::None);
    ctx.register_built_in_object(ctor_name, constructor.release());
}

} // namespace

void register_disposable_builtins(Context& ctx) {
    register_stack(ctx, false);
    register_stack(ctx, true);
}

} // namespace Quanta
