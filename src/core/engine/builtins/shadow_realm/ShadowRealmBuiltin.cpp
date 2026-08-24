#include "quanta/core/engine/builtins/ShadowRealmBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/modules/ModuleLoader.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace Quanta {

namespace {

// One Engine per realm, kept for as long as the process runs. A realm's
// intrinsics are reachable from wrapped functions the caller may still hold, so
// nothing here is ever taken back.
std::vector<Engine*>& realm_registry() {
    static thread_local std::vector<Engine*> realms;
    return realms;
}

Engine* realm_of(const Value& v) {
    Object* obj = v.as_object_or_null();
    if (!obj) return nullptr;
    Value slot = obj->get_internal_slot("__shadowrealm__");
    if (!slot.is_number()) return nullptr;
    size_t index = static_cast<size_t>(slot.to_number());
    auto& realms = realm_registry();
    return index < realms.size() ? realms[index] : nullptr;
}

Value wrap_for_caller(Context& caller, Engine* target_realm, const Value& v);

// A function from another realm is never handed over as itself. What crosses is
// a function of THIS realm that calls it, and only values that can cross go in
// or come back out.
Value make_wrapped_function(Context& caller, Engine* target_realm, Object* target) {
    Engine* caller_realm = caller.get_engine();
    auto wrapper = ObjectFactory::create_native_function("",
        [target, target_realm, caller_realm](Context& ctx, std::span<const Value> args,
                                             Value receiver) -> Value {
            (void)receiver;
            Context* target_ctx = target_realm ? target_realm->get_global_context() : nullptr;
            if (!target_ctx) { ctx.throw_type_error("wrapped function has no realm"); return Value(); }

            std::vector<Value> crossed;
            crossed.reserve(args.size());
            for (const Value& a : args) {
                crossed.push_back(wrap_for_caller(*target_ctx, caller_realm, a));
                if (target_ctx->has_exception()) {
                    target_ctx->clear_exception();
                    ctx.throw_type_error("argument cannot cross a realm boundary");
                    return Value();
                }
            }

            // A Proxy is callable without being a Function, and goes through
            // its own apply trap.
            Value result;
            if (target->get_type() == Object::ObjectType::Proxy) {
                result = static_cast<Proxy*>(target)->apply_trap(crossed, Value());
            } else {
                result = static_cast<Function*>(target)->call(*target_ctx, crossed, Value());
            }
            if (target_ctx->has_exception()) {
                // What went wrong inside the other realm does not itself cross:
                // the caller learns that it went wrong, not what with.
                target_ctx->clear_exception();
                ctx.throw_type_error("wrapped function threw");
                return Value();
            }
            Value back = wrap_for_caller(ctx, target_realm, result);
            if (ctx.has_exception()) return Value();
            return back;
        }, 0);

    // length and name are copied from the function being wrapped, as they are
    // there: an accessor that throws makes the wrapping itself fail, and a
    // value of the wrong type is simply absent rather than coerced.
    Object* w = wrapper.get();
    Context* target_ctx = target_realm ? target_realm->get_global_context() : nullptr;

    // The accessor runs in the other realm but the exception can land on
    // whichever context was current, and either way what the caller sees is a
    // TypeError rather than an error object from over there.
    auto crossing_failed = [&](const char* what) -> bool {
        bool failed = (target_ctx && target_ctx->has_exception()) || caller.has_exception();
        if (!failed) return false;
        if (target_ctx && target_ctx->has_exception()) target_ctx->clear_exception();
        if (caller.has_exception()) caller.clear_exception();
        caller.throw_type_error(std::string("wrapped function's ") + what + " threw");
        return true;
    };

    Value len = target->get_property("length");
    if (crossing_failed("length")) return Value();
    // A length is a non-negative integer or +Infinity; anything else the target
    // reports -- a negative number, a fraction, no number at all -- is 0.
    double arity = 0;
    if (len.is_number()) {
        double n = len.to_number();
        if (std::isnan(n)) arity = 0;
        else if (std::isinf(n)) arity = n > 0 ? n : 0;
        else arity = std::max(0.0, std::trunc(n));
    }
    PropertyDescriptor len_desc(Value(arity), PropertyAttributes::Configurable);
    w->set_property_descriptor("length", len_desc);

    Value nm = target->get_property("name");
    if (crossing_failed("name")) return Value();
    PropertyDescriptor name_desc(Value(nm.is_string() ? nm.to_string() : std::string()),
                                 PropertyAttributes::Configurable);
    w->set_property_descriptor("name", name_desc);
    return Value(wrapper.release());
}

// Only two kinds of value cross a realm boundary: a primitive, which is the
// same value everywhere, and a function, which crosses as a wrapper.
Value wrap_for_caller(Context& caller, Engine* target_realm, const Value& v) {
    if (!v.is_object() && !v.is_function()) return v;
    if (v.is_function()) {
        return make_wrapped_function(caller, target_realm,
                                     static_cast<Object*>(v.as_function()));
    }
    // A Proxy over a function is callable too, and callable is what decides
    // whether a value can cross.
    if (Object* obj = v.as_object()) {
        if (obj->get_type() == Object::ObjectType::Proxy &&
            static_cast<Proxy*>(obj)->target_was_callable()) {
            return make_wrapped_function(caller, target_realm, obj);
        }
    }
    caller.throw_type_error("value cannot cross a realm boundary");
    return Value();
}

}  // namespace

void register_shadow_realm_builtins(Context& ctx) {
    auto prototype = ObjectFactory::create_object();
    Object* proto_ptr = prototype.get();

    auto ctor = ObjectFactory::create_native_constructor("ShadowRealm",
        [proto_ptr](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            (void)args;
            if (ctx.get_new_target().is_undefined()) {
                ctx.throw_type_error("Constructor ShadowRealm requires 'new'");
                return Value();
            }
            Engine* realm = new Engine();
            if (!realm || !realm->initialize()) {
                ctx.throw_type_error("ShadowRealm: failed to create a realm");
                return Value();
            }
            // The realm's global is an ordinary object of that realm, so its
            // prototype is that realm's Object.prototype and not the one the
            // engine happened to build first.
            if (Context* realm_ctx = realm->get_global_context()) {
                Object* realm_global = realm_ctx->get_global_object();
                Value object_ctor = realm_ctx->get_binding("Object");
                if (realm_ctx->has_exception()) realm_ctx->clear_exception();
                if (realm_global && object_ctor.is_function()) {
                    Value proto = static_cast<Object*>(object_ctor.as_function())
                                      ->get_property("prototype");
                    if (proto.is_object()) realm_global->set_prototype(proto.as_object());
                }
            }
            auto& realms = realm_registry();
            realms.push_back(realm);

            Object* self = receiver.as_object_or_null();
            std::unique_ptr<Object> made;
            if (!self) { made = ObjectFactory::create_object(); self = made.get(); }
            self->set_prototype(proto_ptr);
            self->set_internal_slot("__shadowrealm__",
                                    Value(static_cast<double>(realms.size() - 1)));
            if (made) return Value(made.release());
            return Value(self);
        }, 0);

    auto evaluate_fn = ObjectFactory::create_native_function("evaluate",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Engine* realm = realm_of(receiver);
            if (!realm) { ctx.throw_type_error("evaluate called on a non-ShadowRealm"); return Value(); }
            if (args.empty() || !args[0].is_string()) {
                ctx.throw_type_error("evaluate expects a string");
                return Value();
            }
            // The realm's own eval, which is what the spec runs and the only
            // path that answers with the script's completion value.
            Context* realm_ctx = realm->get_global_context();
            Object* realm_global = realm_ctx ? realm_ctx->get_global_object() : nullptr;
            if (!realm_global) { ctx.throw_type_error("realm has no global"); return Value(); }
            Value eval_fn = realm_global->get_property("eval");
            if (!eval_fn.is_function()) { ctx.throw_type_error("realm has no eval"); return Value(); }

            Value source = args[0];
            Value result = eval_fn.as_function()->call(*realm_ctx, {source}, Value());
            if (realm_ctx->has_exception()) {
                // Source text that will not parse is a SyntaxError here.
                // Anything the evaluation itself threw is reported as a
                // TypeError: the error object cannot cross either.
                Value exc = realm_ctx->get_exception();
                realm_ctx->clear_exception();
                bool syntax = false;
                std::string msg;
                if (Object* eo = exc.as_object_or_null()) {
                    Value name = eo->get_property("name");
                    if (realm_ctx->has_exception()) realm_ctx->clear_exception();
                    syntax = name.is_string() && name.to_string() == "SyntaxError";
                    Value m = eo->get_property("message");
                    if (realm_ctx->has_exception()) realm_ctx->clear_exception();
                    if (m.is_string()) msg = m.to_string();
                }
                if (syntax) ctx.throw_syntax_error(msg.empty() ? "invalid source text" : msg);
                else ctx.throw_type_error("evaluate threw inside the realm");
                return Value();
            }
            return wrap_for_caller(ctx, realm, result);
        }, 1);
    { PropertyDescriptor d(Value(evaluate_fn.release()),
                           static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                           PropertyAttributes::Configurable));
      proto_ptr->set_property_descriptor("evaluate", d); }

    auto import_value_fn = ObjectFactory::create_native_function("importValue",
        [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            // Validating the receiver, coercing the specifier and checking the
            // export name all happen before there is a promise to reject with:
            // they throw. Only what the import itself does is a rejection.
            Engine* realm = realm_of(receiver);
            if (!realm) {
                ctx.throw_type_error("importValue called on a non-ShadowRealm");
                return Value();
            }
            Value spec_arg = args.empty() ? Value() : args[0];
            if (spec_arg.is_object() || spec_arg.is_function()) {
                Object* o = spec_arg.is_object() ? spec_arg.as_object()
                                                 : static_cast<Object*>(spec_arg.as_function());
                spec_arg = o->to_primitive("string");
                if (ctx.has_exception()) return Value();
            }
            if (spec_arg.is_symbol()) {
                ctx.throw_type_error("Cannot convert a Symbol value to a string");
                return Value();
            }
            std::string specifier = spec_arg.to_string();
            if (args.size() < 2 || !args[1].is_string()) {
                ctx.throw_type_error("importValue expects a string binding name");
                return Value();
            }
            std::string binding = args[1].to_string();

            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* promise = Quanta::as_promise(promise_obj.get());
            if (!promise) return Value(promise_obj.release());
            auto reject_type_error = [&](const std::string& msg) {
                ctx.throw_type_error(msg);
                Value exc = ctx.get_exception();
                ctx.clear_exception();
                promise->reject(exc);
            };

            Context* realm_ctx = realm->get_global_context();
            ModuleLoader* loader = realm->get_module_loader();
            if (!realm_ctx || !loader) {
                reject_type_error("importValue: realm has no module loader");
                return Value(promise_obj.release());
            }
            Module* mod = loader->load_module(specifier, ctx.get_current_filename());
            if (!mod || mod->has_thrown_exception()) {
                if (realm_ctx->has_exception()) realm_ctx->clear_exception();
                reject_type_error("importValue: the module failed to load");
                return Value(promise_obj.release());
            }
            if (!mod->has_export(binding)) {
                reject_type_error("importValue: the module has no export named '" + binding + "'");
                return Value(promise_obj.release());
            }
            Value exported = wrap_for_caller(ctx, realm, mod->get_export(binding));
            if (ctx.has_exception()) {
                Value exc = ctx.get_exception();
                ctx.clear_exception();
                promise->reject(exc);
                return Value(promise_obj.release());
            }
            promise->fulfill(exported);
            return Value(promise_obj.release());
        }, 2);
    { PropertyDescriptor d(Value(import_value_fn.release()),
                           static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                           PropertyAttributes::Configurable));
      proto_ptr->set_property_descriptor("importValue", d); }

    if (Symbol* tag = Symbol::get_well_known(Symbol::TO_STRING_TAG)) {
        PropertyDescriptor d(Value(std::string("ShadowRealm")), PropertyAttributes::Configurable);
        proto_ptr->set_property_descriptor(tag->to_property_key(), d);
    }

    Object* ctor_ptr = ctor.get();
    { PropertyDescriptor d(Value(proto_ptr), PropertyAttributes::None);
      ctor_ptr->set_property_descriptor("prototype", d); }
    { PropertyDescriptor d(Value(ctor_ptr),
                           static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                           PropertyAttributes::Configurable));
      proto_ptr->set_property_descriptor("constructor", d); }

    // The global binding is a property like every other constructor's:
    // writable and configurable, never enumerable.
    if (Object* global = ctx.get_global_object()) {
        PropertyDescriptor d(Value(ctor.release()),
                             static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                             PropertyAttributes::Configurable));
        global->set_property_descriptor("ShadowRealm", d);
    } else {
        ctx.create_binding("ShadowRealm", Value(ctor.release()), true);
    }
    prototype.release();
}

}  // namespace Quanta
