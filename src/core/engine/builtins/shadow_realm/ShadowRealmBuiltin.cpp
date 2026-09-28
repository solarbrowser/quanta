#include "quanta/core/engine/builtins/ShadowRealmBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"

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

// `deliver_ctx`: where an exception this call raises actually lands, checked
// by whatever C++ code runs right after -- null means "same as caller" (the
// original, still-correct behavior when caller already IS the real ambient
// context, e.g. every within-a-wrapper's-own-call-time use below). Only
// evaluate()/importValue()'s own TOP-LEVEL calls pass caller as home_ctx (the
// realm whose intrinsics construct the error, per WrappedFunctionCreate/
// PerformShadowRealmEval) while the exception itself must still land on the
// real caller ctx that native-call dispatch checks afterward -- conflating
// the two here is exactly the "realm hint vs. delivery context" bug already
// fixed once this session (JSON.stringify's BigInt TypeError).
Value wrap_for_caller(Context& caller, Engine* target_realm, const Value& v, Context* deliver_ctx = nullptr);

// A function from another realm is never handed over as itself. What crosses is
// a function of THIS realm that calls it, and only values that can cross go in
// or come back out.
Value make_wrapped_function(Context& caller, Engine* target_realm, Object* target, Context* deliver_ctx = nullptr) {
    Context& deliver = deliver_ctx ? *deliver_ctx : caller;
    Engine* caller_realm = caller.get_engine();
    auto wrapper = ObjectFactory::create_native_function("",
        [target, target_realm, caller_realm](Context& ctx, std::span<const Value> args,
                                             Value receiver) -> Value {
            (void)receiver;
            // Errors a WrappedFunction throws come from ITS OWN creation
            // realm (caller_realm, captured when this wrapper was built),
            // not the ambient realm of whoever is calling it THIS time --
            // test262's wrapped-function-throws-typeerror-from-caller-realm.js
            // calls the same wrapped function from two different realms and
            // expects each realm's own TypeError.
            Context* caller_ctx = caller_realm ? caller_realm->get_global_context() : &ctx;
            Context* target_ctx = target_realm ? target_realm->get_global_context() : nullptr;
            if (!target_ctx) { ctx.throw_type_error_as(*caller_ctx, "wrapped function has no realm"); return Value(); }

            std::vector<Value> crossed;
            crossed.reserve(args.size());
            for (const Value& a : args) {
                crossed.push_back(wrap_for_caller(*target_ctx, caller_realm, a));
                if (target_ctx->has_exception()) {
                    target_ctx->clear_exception();
                    ctx.throw_type_error_as(*caller_ctx, "argument cannot cross a realm boundary");
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
                ctx.throw_type_error_as(*caller_ctx, "wrapped function threw");
                return Value();
            }
            // GetWrappedValue(callerRealm, result): callerRealm is F.[[Realm]],
            // fixed when this WrappedFunction was created (caller_ctx), not the
            // ambient realm of whoever happens to be calling fn() THIS time --
            // same "caller realm" concept as the throw sites above, now for the
            // success path (test262: "callable results from WrappedFunction
            // should be wrapped in caller realm").
            Value back = wrap_for_caller(*caller_ctx, target_realm, result, &ctx);
            if (ctx.has_exception()) return Value();
            return back;
        }, 0);

    // length and name are copied from the function being wrapped, as they are
    // there: an accessor that throws makes the wrapping itself fail, and a
    // value of the wrong type is simply absent rather than coerced.
    Object* w = wrapper.get();
    // WrappedFunctionCreate step 5: obj.[[Prototype]] is callerRealm's own
    // %Function.prototype%, not whichever realm's setup last ran (the
    // ordinary create_native_function default -- see fixup_new_function_
    // realm's own doc comment).
    Engine::fixup_new_function_realm(w, &caller);
    // The closure above holds target only as a raw C++ pointer, invisible to
    // the collector. Nothing in the target realm necessarily keeps it
    // reachable on its own -- an expression-statement result like `new
    // Proxy(fn, {})` at the end of an evaluated string is never bound to
    // anything there. Pinning it back onto the wrapper closes the loop: as
    // long as the wrapper (what the caller actually holds) is reachable, so
    // is the thing it calls into.
    w->set_internal_slot("__wrapped_target__", Value(target));
    Context* target_ctx = target_realm ? target_realm->get_global_context() : nullptr;

    // The accessor/trap runs via Object::current_context_ (get_property/the
    // Proxy trap machinery have no Context& of their own to thread through),
    // which at this point is whichever context is actually running this
    // native call -- deliver, not necessarily caller (home_ctx) once
    // evaluate() has been borrowed onto another realm's receiver. Checked
    // against all three of target_ctx/caller/deliver since which one
    // actually ends up holding it isn't pinned down by this call itself;
    // either way what the caller sees is a TypeError, not an error object
    // from over there.
    auto crossing_failed = [&](const char* what) -> bool {
        bool failed = (target_ctx && target_ctx->has_exception()) || caller.has_exception() || deliver.has_exception();
        if (!failed) return false;
        if (target_ctx && target_ctx->has_exception()) target_ctx->clear_exception();
        if (caller.has_exception()) caller.clear_exception();
        if (deliver.has_exception()) deliver.clear_exception();
        deliver.throw_type_error_as(caller, std::string("wrapped function's ") + what + " threw");
        return true;
    };

    // CopyNameAndLength's own HasOwnProperty(Target, "length"/"name") step,
    // done first and separately from the Get below: for a Proxy this is what
    // actually reaches the "getOwnPropertyDescriptor" trap (Get does not --
    // absent a "get" trap, [[Get]] forwards straight to the Proxy's target),
    // so a trap that always throws (test262's throws-typeerror-wrap-throwing.js)
    // would otherwise never fire.
    if (target->get_type() == Object::ObjectType::Proxy) {
        static_cast<Proxy*>(target)->get_own_property_descriptor_trap(Value(std::string("length")));
        if (crossing_failed("length")) return Value();
    }
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

    if (target->get_type() == Object::ObjectType::Proxy) {
        static_cast<Proxy*>(target)->get_own_property_descriptor_trap(Value(std::string("name")));
        if (crossing_failed("name")) return Value();
    }
    Value nm = target->get_property("name");
    if (crossing_failed("name")) return Value();
    PropertyDescriptor name_desc(Value(nm.is_string() ? nm.to_string() : std::string()),
                                 PropertyAttributes::Configurable);
    w->set_property_descriptor("name", name_desc);
    return Value(wrapper.release());
}

// Only two kinds of value cross a realm boundary: a primitive, which is the
// same value everywhere, and a function, which crosses as a wrapper.
Value wrap_for_caller(Context& caller, Engine* target_realm, const Value& v, Context* deliver_ctx) {
    if (!v.is_object() && !v.is_function()) return v;
    if (v.is_function()) {
        return make_wrapped_function(caller, target_realm,
                                     static_cast<Object*>(v.as_function()), deliver_ctx);
    }
    // A Proxy over a function is callable too, and callable is what decides
    // whether a value can cross.
    if (Object* obj = v.as_object()) {
        if (obj->get_type() == Object::ObjectType::Proxy &&
            static_cast<Proxy*>(obj)->target_was_callable()) {
            return make_wrapped_function(caller, target_realm, obj, deliver_ctx);
        }
    }
    Context& deliver = deliver_ctx ? *deliver_ctx : caller;
    deliver.throw_type_error_as(caller, "value cannot cross a realm boundary");
    return Value();
}

}  // namespace

void register_shadow_realm_builtins(Context& ctx) {
    // This realm, captured once at registration time -- see Engine::fixup_
    // new_object_realm's own doc comment. ShadowRealm.prototype.evaluate/
    // importValue's own errors (and importValue's own promise, and a
    // WrappedFunction's own [[Prototype]] when built at evaluate()'s top
    // level) come from THIS realm -- the "current Realm Record" of the
    // evaluate/importValue FUNCTION OBJECT itself (10.2.1 [[Call]]), which
    // is whichever realm registered it, not the ambient per-call ctx (the
    // CALLING realm, which differs once evaluate is borrowed onto another
    // realm's ShadowRealm instance via Function.prototype.call -- test262's
    // wrapped-function-proto-from-caller-realm.js).
    Context* home_ctx = &ctx;
    auto prototype = ObjectFactory::create_object();
    Object* proto_ptr = prototype.get();

    auto ctor = ObjectFactory::create_native_constructor_with_new_target("ShadowRealm",
        [proto_ptr](Context& ctx, std::span<const Value> args, Value receiver, bool is_construct, Value new_target) -> Value {
            (void)args;
            (void)new_target;
            if (!is_construct) {
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
            self->initialize_prototype(proto_ptr);
            self->set_internal_slot("__shadowrealm__",
                                    Value(static_cast<double>(realms.size() - 1)));
            if (made) return Value(made.release());
            return Value(self);
        }, 0);

    auto evaluate_fn = ObjectFactory::create_native_function("evaluate",
        [home_ctx](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            Engine* realm = realm_of(receiver);
            if (!realm) { ctx.throw_type_error_as(*home_ctx, "evaluate called on a non-ShadowRealm"); return Value(); }
            if (args.empty() || !args[0].is_string()) {
                ctx.throw_type_error_as(*home_ctx, "evaluate expects a string");
                return Value();
            }
            // The realm's own eval, which is what the spec runs and the only
            // path that answers with the script's completion value.
            Context* realm_ctx = realm->get_global_context();
            Object* realm_global = realm_ctx ? realm_ctx->get_global_object() : nullptr;
            if (!realm_global) { ctx.throw_type_error_as(*home_ctx, "realm has no global"); return Value(); }
            Value eval_fn = realm_global->get_property("eval");
            if (!eval_fn.is_function()) { ctx.throw_type_error_as(*home_ctx, "realm has no eval"); return Value(); }

            std::string source = args[0].to_string();

            // A failure to parse sourceText ITSELF crosses as a real SyntaxError
            // (test262: "SyntaxError exposed to Parent"). Only what goes wrong
            // during the SUBSEQUENT evaluation -- including a SyntaxError from
            // code the evaluated script itself passes to a nested eval() at
            // runtime, thrown well after sourceText's own parse already
            // succeeded -- is reported as a TypeError below (test262: "...coming
            // after runtime evaluation"). A real .name==="SyntaxError" check on
            // whatever the eval call happened to throw can't tell these apart,
            // so sourceText is parsed here, separately and first, purely to
            // answer that one question; the actual run below still reparses it
            // via the realm's own eval (unavoidable -- eval is the only path
            // that also answers with the script's completion value).
            {
                Lexer::LexerOptions lex_opts;
                Lexer lexer(source, lex_opts);
                auto tokens = lexer.tokenize();
                if (lexer.has_errors()) {
                    auto& errors = lexer.get_errors();
                    ctx.throw_syntax_error_as(*home_ctx, errors.empty() ? "invalid source text" : errors[0]);
                    return Value();
                }
                Parser::ParseOptions parse_opts;
                Parser parser(std::move(tokens), parse_opts);
                parser.set_source(source);
                auto program = parser.parse_program();
                if (parser.has_errors() || !program) {
                    auto& errors = parser.get_errors();
                    std::string msg = errors.empty() ? "invalid source text" : errors[0].message;
                    if (msg.substr(0, 13) == "SyntaxError: ") msg = msg.substr(13);
                    ctx.throw_syntax_error_as(*home_ctx, msg);
                    return Value();
                }
            }

            Value result = eval_fn.as_function()->call(*realm_ctx, {Value(source)}, Value());
            if (realm_ctx->has_exception()) {
                // Whatever went wrong crosses as a TypeError regardless of what
                // it actually was: the error object itself cannot cross either.
                realm_ctx->clear_exception();
                ctx.throw_type_error_as(*home_ctx, "evaluate threw inside the realm");
                return Value();
            }
            return wrap_for_caller(*home_ctx, realm, result, &ctx);
        }, 1);
    { PropertyDescriptor d(Value(evaluate_fn.release()),
                           static_cast<PropertyAttributes>(PropertyAttributes::Writable |
                                                           PropertyAttributes::Configurable));
      proto_ptr->set_property_descriptor("evaluate", d); }

    auto import_value_fn = ObjectFactory::create_native_function("importValue",
        [home_ctx](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            // Validating the receiver, coercing the specifier and checking the
            // export name all happen before there is a promise to reject with:
            // they throw. Only what the import itself does is a rejection.
            Engine* realm = realm_of(receiver);
            if (!realm) {
                ctx.throw_type_error_as(*home_ctx, "importValue called on a non-ShadowRealm");
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
                ctx.throw_type_error_as(*home_ctx, "Cannot convert a Symbol value to a string");
                return Value();
            }
            std::string specifier = spec_arg.to_string();
            if (args.size() < 2 || !args[1].is_string()) {
                ctx.throw_type_error_as(*home_ctx, "importValue expects a string binding name");
                return Value();
            }
            std::string binding = args[1].to_string();

            // NewPromiseCapability(%Promise%) uses the CURRENT Realm Record's
            // own %Promise% (this function's home_ctx), same reasoning as
            // evaluate()'s own errors above.
            auto promise_obj = ObjectFactory::create_promise(home_ctx);
            Promise* promise = Quanta::as_promise(promise_obj.get());
            if (!promise) return Value(promise_obj.release());
            auto reject_type_error = [&](const std::string& msg) {
                ctx.throw_type_error_as(*home_ctx, msg);
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
            Value exported = wrap_for_caller(*home_ctx, realm, mod->get_export(binding), &ctx);
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
